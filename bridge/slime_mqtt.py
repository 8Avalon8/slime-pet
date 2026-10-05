#!/usr/bin/env python3
"""The slime in Home Assistant, over MQTT. Stdlib only, Python 3.9+, any OS.

Publishes what the pet senses and does as Home Assistant entities (MQTT discovery), and takes
commands back, so automations can use it and it can be part of the home:

  sensors   someone there (Interaction module PIR), you in front of the camera, ambient light,
            battery and charging, Claude's state and "Claude is waiting for you", level, quiet
            hours, focus minutes left
  controls  say something (notify: shown, and read aloud when that is on), start / stop the
            focus timer, volume, screen brightness, background music, quiet hours on / off

Set SLIME_MQTT_URL to turn it on; slime_buddy.py then runs it alongside (one poll of the pet for
both), or run this file on its own:

    export SLIME_MQTT_URL=mqtt://user:password@homeassistant.local:1883   # mqtts:// for TLS
    python3 slime_mqtt.py            # Ctrl-C to stop; -v logs every message

SLIME_MQTT_ID (default "slime") names the device's topics, slime/<id>/..., so two pets can share
a broker; SLIME_MQTT_PREFIX is Home Assistant's discovery prefix (default "homeassistant").
"""
import json
import os
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slime_brain as brain  # noqa: E402
import slime_tts as tts  # noqa: E402

VERBOSE = "-v" in sys.argv
KEEPALIVE = 60


def log(*a):
    print(time.strftime("%H:%M:%S"), "mqtt:", *a, flush=True)


def settings():
    """(url, id, prefix), url None when MQTT is off."""
    url = os.environ.get("SLIME_MQTT_URL", "").strip() or None
    node = "".join(c for c in os.environ.get("SLIME_MQTT_ID", "slime") if c.isalnum() or c in "_-") or "slime"
    return url, node, os.environ.get("SLIME_MQTT_PREFIX", "homeassistant").strip("/") or "homeassistant"


# ---------------- a small MQTT 3.1.1 client (QoS 0) ----------------

CONNECT, CONNACK, PUBLISH, SUBSCRIBE, SUBACK, PINGREQ, PINGRESP, DISCONNECT = 1, 2, 3, 8, 9, 12, 13, 14


def _str(s):
    b = s.encode("utf-8") if isinstance(s, str) else s
    return struct.pack("!H", len(b)) + b


def _packet(kind, flags, body):
    n, length = len(body), b""
    while True:
        byte, n = n % 128, n // 128
        length += bytes([byte | (0x80 if n else 0)])
        if not n:
            break
    return bytes([kind << 4 | flags]) + length + body


def connect_packet(client_id, user=None, password=None, will=None, keepalive=KEEPALIVE):
    """will: (topic, payload, retain)."""
    flags = 0x02  # clean session
    payload = _str(client_id)
    if will:
        flags |= 0x04 | (0x20 if will[2] else 0)
        payload += _str(will[0]) + _str(will[1])
    if user is not None:
        flags |= 0x80
        payload += _str(user)
        if password is not None:
            flags |= 0x40
            payload += _str(password)
    return _packet(CONNECT, 0, _str("MQTT") + bytes([4, flags]) + struct.pack("!H", keepalive) + payload)


def publish_packet(topic, payload, retain=False):
    data = payload.encode("utf-8") if isinstance(payload, str) else payload
    return _packet(PUBLISH, 0x01 if retain else 0, _str(topic) + data)


def subscribe_packet(pid, topics):
    return _packet(SUBSCRIBE, 0x02, struct.pack("!H", pid) + b"".join(_str(t) + b"\x00" for t in topics))


def read_packet(sock):
    """(kind, flags, body) of the next packet; raises OSError when the connection is gone."""
    def exact(n):
        out = b""
        while len(out) < n:
            got = sock.recv(n - len(out))
            if not got:
                raise OSError("connection closed")
            out += got
        return out
    first = exact(1)[0]
    n, mult = 0, 1
    for _ in range(4):
        byte = exact(1)[0]
        n += (byte & 0x7F) * mult
        mult *= 128
        if not byte & 0x80:
            break
    return first >> 4, first & 0x0F, exact(n) if n else b""


def parse_publish(flags, body):
    """(topic, payload bytes) of a PUBLISH body."""
    (tlen,) = struct.unpack("!H", body[:2])
    topic = body[2:2 + tlen].decode("utf-8", "replace")
    rest = body[2 + tlen:]
    if flags & 0x06:  # QoS 1 or 2 carries a packet id
        rest = rest[2:]
    return topic, rest


class Client:
    """Connects (and reconnects) in a thread of its own; on_message(topic, payload) and
    on_connect() are called from that thread."""

    def __init__(self, url, client_id, will, subscriptions, on_message, on_connect):
        u = urllib.parse.urlsplit(url if "://" in url else "mqtt://" + url)
        self.tls = u.scheme in ("mqtts", "ssl", "tls")
        self.host, self.port = u.hostname or "localhost", u.port or (8883 if self.tls else 1883)
        self.user = urllib.parse.unquote(u.username) if u.username else None
        self.password = urllib.parse.unquote(u.password) if u.password else None
        self.client_id, self.will, self.subs = client_id, will, subscriptions
        self.on_message, self.on_connect = on_message, on_connect
        self.sock, self.lock, self.stopped = None, threading.Lock(), False

    def where(self):
        return "%s:%d" % (self.host, self.port)

    def publish(self, topic, payload, retain=False):
        """False when not connected (the message is dropped: the next state publish catches up)."""
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(publish_packet(topic, payload, retain))
                return True
            except OSError:
                return False

    def _send(self, data):
        with self.lock:
            self.sock.sendall(data)

    def _session(self):
        sock = socket.create_connection((self.host, self.port), timeout=10)
        if self.tls:
            sock = ssl.create_default_context().wrap_socket(sock, server_hostname=self.host)
        sock.sendall(connect_packet(self.client_id, self.user, self.password, self.will))
        kind, _, body = read_packet(sock)
        if kind != CONNACK or len(body) < 2 or body[1] != 0:
            code = body[1] if kind == CONNACK and len(body) > 1 else -1
            sock.close()
            raise PermissionError({4: "bad user name or password", 5: "not authorized"}.get(code, "refused (%d)" % code))
        sock.sendall(subscribe_packet(1, self.subs))
        sock.settimeout(KEEPALIVE / 2)
        with self.lock:
            self.sock = sock
        self.on_connect()
        while not self.stopped:
            try:
                kind, flags, body = read_packet(sock)
            except socket.timeout:
                self._send(_packet(PINGREQ, 0, b""))
                continue
            if kind == PUBLISH:
                topic, payload = parse_publish(flags, body)
                try:
                    self.on_message(topic, payload)
                except Exception as e:  # one bad command does not drop the connection
                    log("command failed:", e)

    def run(self):
        delay = 2
        while not self.stopped:
            try:
                self._session()
                delay = 2
            except PermissionError as e:
                log("broker %s: %s" % (self.where(), e))
                delay = 300
            except (OSError, ssl.SSLError) as e:
                if self.stopped:
                    break
                log("broker %s unreachable (%s); retrying in %d s" % (self.where(), e, delay))
            with self.lock:
                if self.sock:
                    try:
                        self.sock.close()
                    except OSError:
                        pass
                self.sock = None
            if not self.stopped:
                time.sleep(delay)
                delay = min(delay * 2, 60)

    def start(self):
        threading.Thread(target=self.run, daemon=True, name="mqtt").start()

    def stop(self):
        self.stopped = True
        with self.lock:
            if self.sock:
                try:
                    self.sock.sendall(_packet(DISCONNECT, 0, b""))
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None


# ---------------- the pet as Home Assistant entities ----------------

CLAUDE_STATES = ["idle", "think", "work", "wait"]


def entities(en=False):
    """(component, key, name, extra config) for every entity. State comes from the JSON published
    on slime/<id>/state under key; commands arrive on slime/<id>/set/<key>."""
    t = (lambda zh, e: e) if en else (lambda zh, e: zh)
    return [
        ("binary_sensor", "motion", t("有人", "Someone there"), {"device_class": "occupancy"}),
        ("binary_sensor", "face", t("看到你", "Sees you"), {"device_class": "occupancy"}),
        ("sensor", "light", t("环境光", "Ambient light"), {"state_class": "measurement", "icon": "mdi:brightness-5"}),
        ("sensor", "battery", t("电量", "Battery"), {"device_class": "battery", "unit_of_measurement": "%",
                                                    "state_class": "measurement"}),
        ("binary_sensor", "charging", t("充电中", "Charging"), {"device_class": "battery_charging"}),
        ("sensor", "claude", t("Claude 状态", "Claude"), {"device_class": "enum", "options": CLAUDE_STATES,
                                                         "icon": "mdi:robot"}),
        ("binary_sensor", "waiting", t("Claude 在等你", "Claude waiting for you"), {"icon": "mdi:account-clock"}),
        ("sensor", "level", t("等级", "Level"), {"icon": "mdi:star"}),
        ("binary_sensor", "night", t("夜间勿扰中", "Quiet hours now"), {"icon": "mdi:weather-night"}),
        ("sensor", "focus", t("专注剩余", "Focus left"), {"unit_of_measurement": "min", "icon": "mdi:timer-sand"}),
        ("notify", "say", t("说话", "Say"), {}),
        ("button", "focus_start", t("开始专注", "Start focus"), {"icon": "mdi:timer-play"}),
        ("button", "focus_stop", t("结束专注", "Stop focus"), {"icon": "mdi:timer-off"}),
        ("number", "volume", t("音量", "Volume"), {"min": 0, "max": 100, "step": 5, "icon": "mdi:volume-high"}),
        ("number", "screen_bright", t("屏幕亮度", "Screen brightness"), {"min": 10, "max": 100, "step": 5,
                                                                      "icon": "mdi:brightness-6"}),
        ("switch", "bgm", t("背景音乐", "Background music"), {"icon": "mdi:music"}),
        ("switch", "quiet", t("夜间勿扰", "Quiet hours"), {"icon": "mdi:bell-sleep"}),
    ]


def state(st, cfg):
    """What the entities show, from the pet's /api/status and /api/config. Sensors the pet cannot
    read right now (no module, camera off) are None, which Home Assistant shows as unknown."""
    st, cfg = st or {}, cfg or {}
    mod, cam, bat, cc = st.get("mod") or {}, st.get("cam") or {}, st.get("bat") or {}, st.get("cc") or {}
    on = lambda v: None if v is None else ("ON" if v else "OFF")  # noqa: E731
    seeing = cam.get("on") and cam.get("ok") and cam.get("face_on", True)
    return {
        "motion": on(mod.get("motion")) if mod.get("ok") else None,
        "face": on(cam.get("face")) if seeing else None,
        "light": mod.get("light") if mod.get("ok") else None,
        "battery": bat.get("soc") if bat.get("ok") else None,
        "charging": on(bat.get("chg")) if bat.get("ok") else None,
        "claude": cc.get("status") if cc.get("status") in CLAUDE_STATES else None,
        "waiting": on(cc.get("status") == "wait") if cc.get("status") else None,
        "level": st.get("lv"),
        "night": on(st.get("night")) if "night" in st else None,
        "focus": max(0, (st.get("focus") or 0) + 59) // 60 if "focus" in st else None,
        "volume": cfg.get("volume"),
        "screen_bright": cfg.get("screen_bright"),
        "bgm": on(cfg.get("bgm")) if "bgm" in cfg else None,
        "quiet": on(cfg.get("quiet")) if "quiet" in cfg else None,
    }


# a change here goes out at once; the rest (light, battery, ...) at most every SLOW_S
FAST = ("motion", "face", "claude", "waiting", "charging", "night", "focus", "volume", "screen_bright", "bgm", "quiet")
SLOW_S = 30


def post_config(values):
    ip = brain.device_ip()
    if not ip:
        raise OSError("device not found")
    req = urllib.request.Request("http://%s/api/config" % ip, data=json.dumps(values).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=3) as r:
        r.read()


def say_plain(text):
    """Show a line on the pet, and read it aloud when reading aloud is on (no buddy around)."""
    brain.device_cmd("talk happy %s\n" % text, timeout=4)
    cfg = tts.device_config()
    if cfg.get("speak"):
        tts.play(tts.speak_bytes(text, "happy", cfg.get("speak_pitch", 100)))


class Home:
    """Keeps Home Assistant in step with the pet. update() is fed the pet's status (None while it
    is unreachable); commands from Home Assistant go to the pet."""

    def __init__(self, url, node="slime", prefix="homeassistant", say=None, client=None):
        self.node, self.prefix, self.say = node, prefix, say or say_plain
        self.base = "slime/%s" % node
        self.cfg, self.cfg_at = {}, 0
        self.sent, self.sent_at, self.online, self.st, self.polled = None, 0, None, None, False
        self.flushing = threading.Lock()  # update() on the caller's thread, on_connect() on the client's
        self.client = client or Client(
            url, "slime-%s-%d" % (node, os.getpid()), (self.base + "/status", "offline", True),
            [self.base + "/set/#", prefix + "/status"], self.on_message, self.on_connect)

    def discovery(self):
        """(topic, payload) of every entity's discovery config."""
        en = brain.lang() == "en"
        dev = {"identifiers": ["slime_pet_" + self.node], "name": "Slime Pet" if en else "史莱姆桌宠",
               "manufacturer": "slime-pet", "model": "ESP-Mosaico",
               "configuration_url": "http://%s/" % (brain.device_ip() or "slime.local")}
        out = []
        for comp, key, name, extra in entities(en):
            c = {"name": name, "unique_id": "slime_%s_%s" % (self.node, key), "device": dev,
                 "availability_topic": self.base + "/status"}
            if comp in ("sensor", "binary_sensor", "number", "switch"):
                c["state_topic"] = self.base + "/state"
                c["value_template"] = "{{ value_json.%s }}" % key
            if comp in ("notify", "button", "number", "switch"):
                c["command_topic"] = "%s/set/%s" % (self.base, key)
            c.update(extra)
            out.append(("%s/%s/slime_%s/%s/config" % (self.prefix, comp, self.node, key), json.dumps(c, ensure_ascii=False)))
        return out

    def on_connect(self):
        log("connected to %s" % self.client.where())
        for topic, payload in self.discovery():
            self.client.publish(topic, payload, retain=True)
        self.online, self.sent = None, None  # say it all again
        self.flush()

    def on_message(self, topic, payload):
        text = payload.decode("utf-8", "replace").strip()
        if VERBOSE:
            log("<-", topic, text)
        if topic == self.prefix + "/status":
            if text == "online":  # Home Assistant restarted: it wants the discovery again
                self.on_connect()
            return
        key = topic.rsplit("/", 1)[-1]
        if key == "say":
            if text:  # reading aloud takes a while: not on the connection's thread
                threading.Thread(target=self._say, args=(text[:120],), daemon=True).start()
            return
        if key in ("focus_start", "focus_stop"):
            brain.device_cmd("focus %s\n" % key.split("_")[1], timeout=3)
        elif key == "volume":
            v = max(0, min(100, int(float(text))))
            post_config({"volume": v, "sound": v > 0})
        elif key == "screen_bright":
            post_config({"screen_bright": max(10, min(100, int(float(text))))})
        elif key in ("bgm", "quiet"):
            post_config({key: text.upper() == "ON"})
        else:
            return
        self.cfg_at = 0  # read the settings back on the next update

    def _say(self, text):
        try:
            self.say(text)
        except Exception as e:
            log("could not say it:", e)

    def update(self, st, now=None):
        now = time.time() if now is None else now
        self.st, self.polled = st, True
        if st is not None and now - self.cfg_at > 30:
            self.cfg_at = now
            self.cfg = tts.device_config() or self.cfg
        self.flush(now)

    def flush(self, now=None):
        with self.flushing:
            self._flush(time.time() if now is None else now)

    def _flush(self, now):
        if not self.polled:
            return  # nothing to say about the pet before the first poll
        st = self.st
        online = st is not None
        if online != self.online and self.client.publish(self.base + "/status", "online" if online else "offline",
                                                         retain=True):
            self.online = online
        if not online:
            return
        cur = state(st, self.cfg)
        if cur == self.sent:
            return
        fast = self.sent is None or any(cur.get(k) != self.sent.get(k) for k in FAST)
        if fast or now - self.sent_at >= SLOW_S:
            if self.client.publish(self.base + "/state", json.dumps(cur), retain=True):
                if VERBOSE:
                    log("->", cur)
                self.sent, self.sent_at = cur, now

    def start(self):
        self.client.start()
        return self

    def stop(self):
        self.client.publish(self.base + "/status", "offline", retain=True)
        self.client.stop()


def start(say=None):
    """A running Home when SLIME_MQTT_URL is set, else None."""
    url, node, prefix = settings()
    return Home(url, node, prefix, say=say).start() if url else None


def main():
    home = start()
    if not home:
        sys.exit(__doc__)
    log("pet %s, topics slime/%s/..., discovery under %s/" % (brain.device_ip() or "(not found yet)", home.node,
                                                               home.prefix))
    try:
        while True:
            try:
                st = json.loads(brain.device_get("/api/status", timeout=3))
            except Exception:
                st = None
            home.update(st)
            time.sleep(2)
    except KeyboardInterrupt:
        home.stop()


if __name__ == "__main__":
    main()
