#!/usr/bin/env python3
"""End-to-end test of the real firmware in the simulator, no board needed (`make simtest`).

Starts ./slime_sim headless on a free port, drives it the way the world would (Claude Code hook
events through bridge/slime_hook.py, touch, buttons, IMU, battery) and checks what the pet
does through its own /api/status. Screenshots of each step land in out/sim/.
Stdlib only.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

HOST_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.abspath(os.path.join(HOST_DIR, "..", "..", ".."))
HOOK = os.path.join(REPO, "bridge", "slime_hook.py")
SHOTS = os.path.join(HOST_DIR, "out", "sim")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


PORT = free_port()
BASE = "http://127.0.0.1:%d" % PORT
failures = []


def http(path, body=None):
    req = urllib.request.Request(BASE + path, data=body.encode() if isinstance(body, str) else body)
    with urllib.request.urlopen(req, timeout=3) as r:
        return r.read()


def status():
    return json.loads(http("/api/status"))


def sim(*cmds):
    http("/sim/input", "\n".join(cmds))


def hook(event, sid="5e55a0001234", **fields):
    ev = dict(hook_event_name=event, session_id=sid, **fields)
    env = dict(os.environ, SLIME_HOST="127.0.0.1:%d" % PORT, SLIME_AI="0")
    subprocess.run([sys.executable, HOOK], input=json.dumps(ev).encode(), env=env, check=True, timeout=10)


def shot(name):
    os.makedirs(SHOTS, exist_ok=True)
    with open(os.path.join(SHOTS, name + ".bmp"), "wb") as f:
        f.write(http("/sim/screen.bmp"))


def pixel(bmp, x, y):
    """(r, g, b) of a 24-bit BMP at x, y from the top."""
    off, w, h = struct.unpack_from("<I", bmp, 10)[0], *struct.unpack_from("<ii", bmp, 18)
    row = (w * 3 + 3) & ~3
    p = off + (y if h < 0 else abs(h) - 1 - y) * row + x * 3
    return bmp[p + 2], bmp[p + 1], bmp[p]


def expect(what, cond, timeout=4.0):
    """Poll /api/status until cond(status) holds."""
    t0, s = time.time(), None
    while time.time() - t0 < timeout:
        s = status()
        if "state" in s and cond(s):  # "{}" until the firmware publishes its first status
            print("ok    %s" % what)
            return s
        time.sleep(0.1)
    failures.append(what)
    print("FAIL  %s  (state=%s msg=%r)" % (what, s.get("state"), s.get("msg")))
    return s


def state_is(*names):
    return lambda s: s["state"] in names


def main():
    exe = os.path.join(HOST_DIR, "slime_sim")
    proc = subprocess.Popen([exe, "--headless", "--quiet", "--port", str(PORT)], stdin=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                status()
                break
            except OSError:
                time.sleep(0.1)
        run()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("\n%d failed: %s" % (len(failures), ", ".join(failures)) if failures else "\nall passed")
    return 1 if failures else 0


def run():
    expect("boots to idle", state_is("idle"))
    assert b"<html" in http("/").lower(), "web panel not served"
    print("ok    web panel served")
    shot("01_idle")

    # body and buttons
    sim("shake")
    expect("shake -> dizzy", state_is("dizzy"))
    shot("02_dizzy")
    expect("dizzy wears off", state_is("idle"), timeout=6)
    sim("tap 100 240")
    expect("tap on the left -> poke", state_is("pokeL"))
    sim("facedown 1")
    expect("face down -> sleep", state_is("sleep"), timeout=4)
    sim("facedown 0")
    expect("picked up again -> wakes", state_is("greet", "idle"))
    sim("right")
    expect("module button -> volume up", lambda s: "音量" in s["msg"])
    sim("battery 10")
    expect("battery 10% -> melt", state_is("melt"), timeout=12)
    shot("03_melt")
    sim("battery 60 400")
    expect("charging -> charge", state_is("charge"), timeout=12)
    sim("battery 90")

    # settings screen: hold the screen
    sim("hold 240 240")
    time.sleep(1.6)
    shot("04_settings")
    sim("ai")  # any button closes it
    time.sleep(0.3)

    # Claude Code, through the real hook script
    hook("SessionStart", source="startup")
    expect("SessionStart -> greets", lambda s: s["cc"]["sessions"] and s["state"] in ("greet", "idle", "charge"))
    hook("UserPromptSubmit")
    expect("prompt -> think", state_is("think"))
    shot("05_think")
    hook("PreToolUse", tool_name="Bash", tool_input={"command": "idf.py build"})
    s = expect("tool -> work, shows the command", lambda s: s["state"] == "work" and "idf.py build" in s["cc"]["detail"])
    shot("06_work")
    hook("PermissionRequest", tool_name="Bash", tool_input={"command": "rm -rf build"}, sid="0bb0a0005678")
    expect("second session asks -> wait wins", lambda s: s["state"] == "wait" and s["cc"]["busy"] == 2)
    shot("07_wait")
    hook("PermissionDenied", sid="0bb0a0005678")
    hook("SessionEnd", sid="0bb0a0005678")
    expect("other session gone -> work again", lambda s: s["state"] == "work" and len(s["cc"]["sessions"]) == 1)
    hook("SubagentStart", agent_id="agent-a1", agent_type="Explore")
    hook("SubagentStart", agent_id="agent-b2", agent_type="Plan")
    expect("subagents -> helper slimes", lambda s: s["cc"]["subs"] == 2 and "2" in s["msg"] and s["cc"]["sessions"][0]["subs"] == 2)
    time.sleep(1.0)  # let them land
    shot("07b_helpers")
    hook("SubagentStop", agent_id="agent-a1", agent_type="Explore")
    hook("SubagentStop", agent_id="agent-b2", agent_type="Plan")
    expect("subagents done -> helpers gone", lambda s: s["cc"]["subs"] == 0)
    hook("PostToolUseFailure", tool_name="Bash", error="exit status 2")
    expect("tool failed -> hurt", state_is("hurt"))
    shot("08_hurt")
    exp0 = status()["exp"] + status()["lv"] * 1000
    for i in range(12):
        hook("PreToolUse", tool_name="Read", tool_input={"file_path": "/src/f%d.c" % i})
        hook("PostToolUse")
    hook("Stop")
    s = expect("stop -> done or level up", state_is("greet", "levelup"))
    if s["exp"] + s["lv"] * 1000 <= exp0:
        failures.append("stop gives exp")
        print("FAIL  stop gives exp")
    else:
        print("ok    stop gives exp (Lv %d, %d/%d)" % (s["lv"], s["exp"], s["need"]))
    shot("09_done")
    expect("idle again", state_is("idle"), timeout=8)

    # a camera that is plugged in and switched on but never starts: the slime says so, once
    sim("camera_stuck 1")
    expect("camera stuck -> the slime complains", lambda s: "摄像头" in s["msg"], timeout=10)
    shot("09b_camera_stuck")
    sim("camera_stuck 0")
    expect("idle again", state_is("idle"), timeout=30)

    # modules plugged in and pulled out: the slime says what it gained or lost
    sim("module L camera")
    expect("camera plugged in -> the slime is glad", lambda s: "眼睛装上" in s["msg"], timeout=6)
    shot("09c_camera_in")
    sim("module R none")
    expect("interaction module pulled out -> the slime misses it", lambda s: "拔走" in s["msg"] and not s["mod"]["ok"], timeout=15)
    sim("module L none")
    expect("camera pulled out", lambda s: "眼前一黑" in s["msg"], timeout=15)
    hook("UserPromptSubmit", prompt="busy")
    expect("thinking", state_is("think"))
    sim("module R interact")
    expect("interaction module back: said at once, even while Claude is busy", lambda s: "接上" in s["msg"] and s["mod"]["ok"], timeout=4)
    hook("Stop")
    expect("idle again", state_is("idle"), timeout=30)
    # hand gestures are commands
    sim("gesture like")
    expect("thumb up -> the slime is pleased", lambda s: "赞" in s["msg"] and s["state"] == "greet")
    expect("idle again", state_is("idle"), timeout=8)
    sim("gesture five")
    expect("open palm -> muted", lambda s: "静音" in s["msg"] and not json.loads(http("/api/config"))["sound"])
    expect("idle again", state_is("idle"), timeout=8)
    sim("gesture five")
    expect("open palm again -> sound back", lambda s: "声音打开" in s["msg"] and json.loads(http("/api/config"))["sound"])
    expect("idle again", state_is("idle"), timeout=8)
    sim("gesture ok")
    expect("OK -> focus timer starts", lambda s: s["focus"] > 0)
    expect("idle again", state_is("idle"), timeout=8)
    sim("gesture ok")
    expect("OK again -> focus timer stops", lambda s: s["focus"] == 0)
    cfg = json.loads(http("/api/config"))
    if cfg.get("face") is not True or cfg.get("gesture") is not False:
        failures.append("face on, gestures off by default")
        print("FAIL  face on, gestures off by default", cfg.get("face"), cfg.get("gesture"))
    else:
        print("ok    face on, gestures off by default")
    expect("idle again", state_is("idle"), timeout=8)

    # the bridge's AI comment line
    http("/api/cmd", "say happy 测试通过，真棒！")
    expect("say -> comment on the dialog", lambda s: "测试通过" in s["msg"])
    shot("10_comment")

    # settings through the web panel's API
    http("/api/config", json.dumps({"volume": 30}))
    v = json.loads(http("/api/config"))["volume"]
    (print("ok    config round trip") if v == 30 else (failures.append("config round trip"), print("FAIL  config round trip")))
    http("/api/config", json.dumps({"wake": False}))
    w = json.loads(http("/api/config")).get("wake")
    (print("ok    wake word setting round trip") if w is False else (failures.append("wake setting"), print("FAIL  wake setting: %r" % w)))

    # background scenery: each fixed scene paints the sky, "off" is plain black, and asleep it dims with the pet
    skies = {}
    for scene, name in ((2, "dawn"), (3, "day"), (4, "dusk"), (5, "night"), (1, "off")):
        http("/api/config", json.dumps({"scene": scene}))
        time.sleep(0.6)
        shot("11_scene_" + name)
        skies[name] = pixel(http("/sim/screen.bmp"), 240, 24)
    lit = [skies[n] for n in ("dawn", "day", "dusk", "night")]
    ok = skies["off"] == (0, 0, 0) and all(c != (0, 0, 0) for c in lit) and len(set(lit)) == 4
    (print("ok    background scenes %s" % skies) if ok else (failures.append("background scenes"), print("FAIL  background scenes %s" % skies)))
    http("/api/config", json.dumps({"scene": 5}))
    http("/api/cmd", "state sleep")
    time.sleep(0.8)
    shot("12_scene_night_asleep")
    dim, bright = pixel(http("/sim/screen.bmp"), 240, 24), skies["night"]
    ok = sum(dim) < sum(bright)
    (print("ok    background dims while asleep") if ok else (failures.append("background dim"), print("FAIL  background dim %s vs %s" % (dim, bright))))
    http("/api/config", json.dumps({"scene": 0}))

    # the bridge's AI endpoints: a saved key only comes back with the update token ("simulator" here),
    # and changing the address without a key drops the old key
    http("/api/ai", json.dumps({"llm_url": "http://localhost:1234/v1", "llm_model": "m", "llm_key": "sk-test"}))
    plain = json.loads(http("/api/ai"))
    req = urllib.request.Request(BASE + "/api/ai", headers={"X-OTA-Token": "simulator"})
    with urllib.request.urlopen(req, timeout=3) as r:
        trusted = json.load(r)
    http("/api/ai", json.dumps({"llm_url": "http://localhost:9/v1"}))
    moved = json.loads(http("/api/ai"))
    ok = (plain.get("llm_key_set") is True and "llm_key" not in plain and plain.get("llm_model") == "m"
          and trusted.get("llm_key") == "sk-test" and moved.get("llm_key_set") is False)
    (print("ok    AI settings: key hidden without the token") if ok
     else (failures.append("AI settings"), print("FAIL  AI settings: %r %r %r" % (plain, trusted, moved))))

    speech()
    home_assistant()


def home_assistant():
    """Home Assistant over MQTT, from the device itself (main/ha_mqtt.c on the simulator's stand-in
    broker, sim_mqtt.c): discovery, state, commands, and the password staying out of sight."""
    def broker():
        return json.loads(http("/sim/mqtt"))

    def wait(what, cond, timeout=4.0):
        t0, b = time.time(), None
        while time.time() - t0 < timeout:
            b = broker()
            if cond(b):
                print("ok    %s" % what)
                return b
            time.sleep(0.1)
        failures.append(what)
        print("FAIL  %s  (%r)" % (what, {k: v for k, v in b.items() if k != "msgs"}))
        return b

    def check(what, ok, detail=None):
        print("ok    %s" % what) if ok else (failures.append(what), print("FAIL  %s  %r" % (what, detail)))

    def refused(body):
        try:
            http("/api/ai", json.dumps(body))
        except urllib.error.HTTPError as e:
            return e.code == 400
        return False

    def states(b, node="desk"):
        return [json.loads(m["payload"]) for m in b["msgs"] if m["topic"] == "slime/%s/state" % node]

    def send(key, payload=""):
        http("/sim/mqtt", "slime/desk/set/%s\n%s" % (key, payload))

    http("/api/config", json.dumps({"lang": 0, "quiet": False}))
    check("mqtt: off until a broker is set", status()["mqtt"] == "off" and not broker()["client"])
    check("mqtt: address with a password in it, another scheme, or a bad id is refused",
          refused({"mqtt_url": "mqtt://ha:secret@broker.test"}) and refused({"mqtt_url": "http://broker.test"})
          and refused({"mqtt_id": "a/b"}) and status()["mqtt"] == "off")
    http("/api/ai", json.dumps({"mqtt_url": "mqtt://broker.test:1883", "mqtt_user": "ha", "mqtt_pass": "s3cret", "mqtt_id": "desk"}))
    plain = json.loads(http("/api/ai"))
    check("mqtt: password hidden without the token",
          plain.get("mqtt_pass_set") is True and "mqtt_pass" not in plain and plain.get("mqtt_user") == "ha", plain)
    b = wait("mqtt: connected, state published", lambda b: b["connected"] and states(b))
    check("mqtt: the broker got the credentials, the last will and the subscriptions",
          b["uri"] == "mqtt://broker.test:1883" and b["user"] == "ha" and b["pass"] == "s3cret" and b["client_id"] == "slime-desk"
          and b["will"] == {"topic": "slime/desk/status", "payload": "offline", "retain": True}
          and b["subs"] == ["slime/desk/set/#", "homeassistant/status"], {k: v for k, v in b.items() if k != "msgs"})
    conf = {m["topic"]: json.loads(m["payload"]) for m in b["msgs"] if m["topic"].startswith("homeassistant/")}
    vol = conf.get("homeassistant/number/slime_desk/volume/config", {})
    say = conf.get("homeassistant/notify/slime_desk/say/config", {})
    motion = conf.get("homeassistant/binary_sensor/slime_desk/motion/config", {})
    check("mqtt: 17 entities announced, all retained", len(conf) == 17 and all(m["retain"] for m in b["msgs"]), sorted(conf))
    check("mqtt: discovery configs",
          vol == {"name": "音量", "unique_id": "slime_desk_volume", "availability_topic": "slime/desk/status",
                  "state_topic": "slime/desk/state", "value_template": "{{ value_json.volume }}",
                  "command_topic": "slime/desk/set/volume", "min": 0, "max": 100, "step": 5, "icon": "mdi:volume-high",
                  "device": {"identifiers": ["slime_pet_desk"], "name": "史莱姆桌宠", "manufacturer": "slime-pet",
                             "model": "ESP-Mosaico", "configuration_url": "http://127.0.0.1/"}}
          and say.get("command_topic") == "slime/desk/set/say" and "state_topic" not in say
          and motion.get("device_class") == "occupancy" and "command_topic" not in motion, (vol, say, motion))
    st = states(b)[-1]
    check("mqtt: state", st.get("claude") == "idle" and st.get("waiting") == "OFF" and st.get("face") is None
          and st.get("battery") == 90 and st.get("motion") == "OFF" and st.get("quiet") == "OFF" and len(st) == 14, st)
    check("mqtt: online", any(m["topic"] == "slime/desk/status" and m["payload"] == "online" for m in b["msgs"]))
    expect("mqtt: the panel shows it connected", lambda s: s["mqtt"] == "connected")

    # commands from Home Assistant
    send("volume", "30.0")
    send("bgm", "OFF")
    wait("mqtt: volume and music set from Home Assistant, and reported back",
         lambda b: states(b)[-1].get("volume") == 30 and states(b)[-1].get("bgm") == "OFF")
    cfg = json.loads(http("/api/config"))
    check("mqtt: the settings really changed", cfg["volume"] == 30 and cfg["bgm"] is False and cfg["sound"] is True, cfg)
    send("say", "晚饭好了，" + "快来吃饭" * 20)  # longer than one command line: cut between characters
    s = expect("mqtt: say is shown", lambda s: "晚饭好了" in s.get("msg", ""))
    check("mqtt: a long line is cut cleanly", s["msg"].endswith(("快来吃饭", "快", "快来", "快来吃")) and "\ufffd" not in s["msg"], s["msg"])
    send("focus_start")
    expect("mqtt: focus started", lambda s: s["focus"] > 0)
    wait("mqtt: focus minutes reported", lambda b: states(b)[-1].get("focus") == json.loads(http("/api/config"))["focus_min"])
    send("focus_stop")
    expect("mqtt: focus stopped", lambda s: s["focus"] == 0)
    hook("PermissionRequest", tool_name="Bash", tool_input={"command": "make"})
    wait("mqtt: Claude waiting for you goes out at once",
         lambda b: states(b)[-1].get("waiting") == "ON" and states(b)[-1].get("claude") == "wait", timeout=2)
    hook("Stop")

    # light is a slow one: not before HA_SLOW_S (3 s in the simulator) after the last state
    wait("mqtt: no longer waiting", lambda b: states(b)[-1].get("waiting") == "OFF")
    expect("idle again", state_is("idle"), timeout=30)
    send("volume", "35")  # a fresh state right before, so that the wait is the whole HA_SLOW_S
    wait("mqtt: settled", lambda b: states(b)[-1].get("volume") == 35)
    time.sleep(0.3)
    n, t0 = len(states(broker())), time.time()
    sim("light 12")
    time.sleep(1.0)
    held = len(states(broker())) == n
    wait("mqtt: light follows later", lambda b: states(b)[-1].get("light") == 12, timeout=5)
    check("mqtt: light held back, then sent", held and time.time() - t0 > 1.5, (held, time.time() - t0))

    # Home Assistant restarts, the connection drops: everything is announced again
    for what, body in (("Home Assistant's birth message", "homeassistant/status\nonline"), ("a reconnect", "@drop")):
        http("/sim/mqtt", "@clear")
        http("/sim/mqtt", body)
        wait("mqtt: discovery and state again after %s" % what,
             lambda b: sum(m["topic"].startswith("homeassistant/") for m in b["msgs"]) == 17 and states(b))

    # the names follow the pet's language; a new address without the password drops the old one; empty = off
    http("/sim/mqtt", "@clear")
    http("/api/config", json.dumps({"lang": 1}))
    wait("mqtt: English names after the language changes", lambda b: any(
        m["topic"] == "homeassistant/number/slime_desk/volume/config" and json.loads(m["payload"])["name"] == "Volume"
        for m in b["msgs"]))
    http("/api/config", json.dumps({"lang": 0}))
    http("/api/ai", json.dumps({"mqtt_url": "mqtt://other.test"}))
    b = wait("mqtt: reconnects to a new address, without the old password",
             lambda b: b["connected"] and b.get("uri") == "mqtt://other.test" and b["pass"] == "")
    check("mqtt: said offline before leaving the old broker",
          any(m["topic"] == "slime/desk/status" and m["payload"] == "offline" for m in b["msgs"]))
    http("/api/ai", json.dumps({"mqtt_user": "ha", "mqtt_pass": "wrong"}))
    expect("mqtt: a refused password is said so in the panel", lambda s: s["mqtt"] == "refused")
    http("/api/ai", json.dumps({"mqtt_pass": "right"}))
    expect("mqtt: connected once the password is right", lambda s: s["mqtt"] == "connected")
    http("/api/ai", json.dumps({"mqtt_url": "", "mqtt_user": "", "mqtt_id": ""}))
    wait("mqtt: off again when the address is cleared", lambda b: not b["client"] and status()["mqtt"] == "off")


def speech():
    """Reading answers aloud: /api/speak takes 16 kHz PCM while the setting is on, and the bridge's
    talk() gets a line's voice from a text-to-speech service (a stand-in on localhost) to the pet."""
    import http.server as hs
    import threading
    # quiet hours mute speech, and the simulator follows the real clock: the switch turns them off
    http("/api/config", json.dumps({"speak": True, "quiet": False}))
    expect("quiet hours switched off -> not night, whatever the clock says", lambda s: not s["night"])
    time.sleep(0.5)
    got = json.loads(http("/api/speak", bytes(2 * 16000)))  # 1 s of silence
    expect("speech: playing", lambda s: s["mic"].get("speaking"))
    expect("speech: done after its length", lambda s: not s["mic"].get("speaking"), timeout=3)
    http("/api/config", json.dumps({"speak": False}))
    try:
        http("/api/speak", bytes(3200))
        refused = False
    except urllib.error.HTTPError as e:
        refused = e.code == 409
    ok = got == {"ok": True, "ms": 1000} and refused
    (print("ok    speech: 1 s accepted, refused when off") if ok
     else (failures.append("speech endpoint"), print("FAIL  speech endpoint %r refused=%r" % (got, refused))))
    http("/api/ai", json.dumps({"tts_key": "sk-tts"}))
    plain = json.loads(http("/api/ai"))
    ok = plain.get("tts_key_set") is True and "tts_key" not in plain
    (print("ok    speech: key hidden without the token") if ok
     else (failures.append("speech key"), print("FAIL  speech key %r" % plain)))
    http("/api/ai", json.dumps({"tts_key": ""}))

    asked = []

    class TTS(hs.BaseHTTPRequestHandler):
        def do_POST(self):
            asked.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            pcm = bytes(2 * 12000)  # 0.5 s at 24 kHz
            body = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 24000, 48000, 2, 16)
                    + b"data" + struct.pack("<I", len(pcm)) + pcm)
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = hs.ThreadingHTTPServer(("127.0.0.1", 0), TTS)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    os.environ.update(SLIME_HOST="127.0.0.1:%d" % PORT, SLIME_TTS_URL="http://127.0.0.1:%d/v1" % srv.server_address[1],
                      SLIME_TTS_KEY="x", SLIME_HOME=os.path.join(HOST_DIR, "out", "sim", "home"))
    sys.path.insert(0, os.path.join(REPO, "bridge"))
    import slime_buddy as buddy
    http("/api/config", json.dumps({"speak": True, "speak_pitch": 100}))
    seen = []
    t = threading.Thread(target=lambda: buddy.talk("happy", "你好呀，我是史莱姆。"))
    t.start()
    deadline = None  # the last line is still being said for a moment after talk() returns
    while deadline is None or time.time() < deadline:
        seen.append(status()["mic"].get("speaking"))
        if deadline is None and not t.is_alive():
            deadline = time.time() + 0.6
        time.sleep(0.05)
    srv.shutdown()
    ok = asked and asked[0]["input"] == "你好呀，我是史莱姆。" and any(seen) and "史莱姆" in status().get("msg", "")
    (print("ok    speech: the bridge reads an answer aloud") if ok
     else (failures.append("bridge speech"), print("FAIL  bridge speech asked=%r seen=%r" % (asked, seen))))


if __name__ == "__main__":
    sys.exit(main())
