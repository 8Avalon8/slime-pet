#!/usr/bin/env python3
"""Home Assistant over MQTT, against a tiny broker in this process: python3 -m unittest discover bridge."""
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault("SLIME_HOME", tempfile.mkdtemp())
os.environ["SLIME_LANG"] = "zh"
sys.path.insert(0, HERE)
import slime_mqtt as mqtt  # noqa: E402

STATUS = {"lv": 7, "night": False, "focus": 600, "bat": {"ok": True, "soc": 64, "chg": True},
          "cc": {"status": "wait"}, "mod": {"ok": True, "motion": True, "light": 321},
          "cam": {"on": False, "ok": False, "face": False}}
CONFIG = {"volume": 60, "screen_bright": 80, "bgm": False, "quiet": True, "speak": False}


class Broker:
    """Accepts one client: answers CONNECT and SUBSCRIBE, records what it publishes, and can
    publish to it."""

    def __init__(self, password="secret"):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.password = password
        self.got, self.connect, self.subs, self.conn = [], None, [], None
        self.cv = threading.Condition()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        conn, _ = self.srv.accept()
        self.conn = conn
        try:
            while True:
                kind, flags, body = mqtt.read_packet(conn)
                with self.cv:
                    if kind == mqtt.CONNECT:
                        self.connect = body
                        ok = self.password.encode() in body
                        conn.sendall(mqtt._packet(mqtt.CONNACK, 0, bytes([0, 0 if ok else 5])))
                    elif kind == mqtt.SUBSCRIBE:
                        pid, rest, topics = body[:2], body[2:], []
                        while rest:
                            n = int.from_bytes(rest[:2], "big")
                            topics.append(rest[2:2 + n].decode())
                            rest = rest[3 + n:]
                        self.subs = topics
                        conn.sendall(mqtt._packet(mqtt.SUBACK, 0, pid + b"\x00" * len(topics)))
                    elif kind == mqtt.PUBLISH:
                        topic, payload = mqtt.parse_publish(flags, body)
                        self.got.append((topic, payload.decode(), bool(flags & 1)))
                    self.cv.notify_all()
        except OSError:
            pass

    def send(self, topic, payload):
        self.conn.sendall(mqtt.publish_packet(topic, payload))

    def wait(self, pred, timeout=3):
        """Polls: some conditions change on the client's threads, not on a message here."""
        end = time.time() + timeout
        while time.time() < end:
            with self.cv:
                if pred(self):
                    return True
                self.cv.wait(0.02)
        return False

    def last(self, topic):
        for t, p, r in reversed(self.got):
            if t == topic:
                return p, r
        return None


class Packets(unittest.TestCase):
    def test_remaining_length_takes_two_bytes_past_127(self):
        p = mqtt.publish_packet("a/b", "x" * 200)
        self.assertEqual(p[0], 0x30)
        self.assertEqual(p[1:3], bytes([(205 % 128) | 0x80, 205 // 128]))

    def test_connect_carries_will_and_login(self):
        p = mqtt.connect_packet("id", "u", "pw", ("slime/s/status", "offline", True))
        flags = p[2 + 2 + 4 + 1]
        self.assertEqual(flags, 0x80 | 0x40 | 0x20 | 0x04 | 0x02)
        self.assertTrue(p.endswith(b"\x00\x01u\x00\x02pw"))


class State(unittest.TestCase):
    def test_what_home_assistant_sees(self):
        s = mqtt.state(STATUS, CONFIG)
        self.assertEqual(s["motion"], "ON")
        self.assertIsNone(s["face"])  # camera off: unknown, not "nobody"
        self.assertEqual((s["light"], s["battery"], s["charging"]), (321, 64, "ON"))
        self.assertEqual((s["claude"], s["waiting"], s["level"], s["focus"]), ("wait", "ON", 7, 10))
        self.assertEqual((s["volume"], s["bgm"], s["quiet"]), (60, "OFF", "ON"))

    def test_no_module_no_battery(self):
        s = mqtt.state({"mod": {"ok": False}, "bat": {"ok": False}}, {})
        self.assertIsNone(s["motion"])
        self.assertIsNone(s["battery"])
        self.assertIsNone(s["volume"])


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.broker = Broker()
        self.said, self.posted = [], []
        patches = [mock.patch.object(mqtt.brain, "device_ip", return_value="slime.test"),
                   mock.patch.object(mqtt.tts, "device_config", return_value=dict(CONFIG)),
                   mock.patch.object(mqtt, "post_config", side_effect=self.posted.append),
                   mock.patch.object(mqtt.brain, "device_cmd")]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.cmd = mqtt.brain.device_cmd
        url = "mqtt://ha:secret@127.0.0.1:%d" % self.broker.port
        self.home = mqtt.Home(url, "desk", say=self.said.append).start()
        self.addCleanup(self.home.stop)
        n = len(mqtt.entities())
        self.assertTrue(self.broker.wait(lambda b: len([g for g in b.got if g[0].endswith("/config")]) == n))
        self.assertIsNone(self.broker.last("slime/desk/status"))  # nothing about the pet before the first poll

    def test_discovery_state_and_commands(self):
        b, home = self.broker, self.home
        self.assertEqual(b.subs, ["slime/desk/set/#", "homeassistant/status"])
        self.assertEqual(len([g for g in b.got if g[0].endswith("/config")]), len(mqtt.entities()))
        cfg = json.loads(b.last("homeassistant/sensor/slime_desk/battery/config")[0])
        self.assertEqual(cfg["device_class"], "battery")
        self.assertEqual(cfg["state_topic"], "slime/desk/state")
        self.assertEqual(cfg["name"], "电量")
        say = json.loads(b.last("homeassistant/notify/slime_desk/say/config")[0])
        self.assertEqual(say["command_topic"], "slime/desk/set/say")

        home.update(STATUS)
        self.assertTrue(b.wait(lambda b: b.last("slime/desk/state")))
        self.assertEqual(b.last("slime/desk/status"), ("online", True))
        state, retained = b.last("slime/desk/state")
        self.assertTrue(retained)
        self.assertEqual(json.loads(state)["claude"], "wait")

        b.send("slime/desk/set/volume", "40")
        b.send("slime/desk/set/bgm", "ON")
        b.send("slime/desk/set/focus_start", "PRESS")
        b.send("slime/desk/set/say", "晚饭好了")
        self.assertTrue(b.wait(lambda _: len(self.posted) == 2 and self.said))
        self.assertEqual(self.posted, [{"volume": 40, "sound": True}, {"bgm": True}])
        self.cmd.assert_called_with("focus start\n", timeout=3)
        self.assertEqual(self.said, ["晚饭好了"])

    def test_small_changes_wait_big_ones_do_not(self):
        b, home = self.broker, self.home
        home.update(STATUS, now=1000)
        self.assertTrue(b.wait(lambda b: b.last("slime/desk/state")))
        n = len(b.got)
        dimmer = json.loads(json.dumps(STATUS))
        dimmer["mod"]["light"] = 300
        home.update(dimmer, now=1005)
        time.sleep(0.2)
        self.assertEqual(len(b.got), n)  # the light alone waits
        idle = json.loads(json.dumps(dimmer))
        idle["cc"]["status"] = "idle"
        home.update(idle, now=1006)
        self.assertTrue(b.wait(lambda b: json.loads(b.last("slime/desk/state")[0])["claude"] == "idle"))

    def test_offline_pet_and_home_assistant_restart(self):
        b, home = self.broker, self.home
        home.update(STATUS)
        self.assertTrue(b.wait(lambda b: b.last("slime/desk/status") == ("online", True)))
        home.update(None)
        self.assertTrue(b.wait(lambda b: b.last("slime/desk/status") == ("offline", True)))
        n = len([g for g in b.got if g[0].endswith("/config")])
        b.send("homeassistant/status", "online")
        self.assertTrue(b.wait(lambda b: len([g for g in b.got if g[0].endswith("/config")]) == 2 * n))


class Login(unittest.TestCase):
    def test_wrong_password_is_reported_not_retried_fast(self):
        broker = Broker(password="right")
        c = mqtt.Client("mqtt://ha:wrong@127.0.0.1:%d" % broker.port, "t", None, ["x"], None, lambda: None)
        with self.assertRaises(PermissionError):
            c._session()


if __name__ == "__main__":
    unittest.main()
