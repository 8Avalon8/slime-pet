#!/usr/bin/env python3
"""A stand-in for the pet on your computer: the same HTTP API, no board, no build.

    python3 bridge/fake_device.py [--port 8080] [--bind 127.0.0.1]
    SLIME_HOST=127.0.0.1:8080 python3 bridge/slime_hook.py --send ask "Bash: rm -rf build"

Point the Claude Code hook at it with SLIME_HOST (export it before starting Claude Code) and
every line the hook sends is printed here, with a rough idea of what the pet would show.
http://127.0.0.1:8080/ serves the real web panel (firmware/slime/main/web/index.html) against
an in-memory status and settings, so the panel can be worked on without a board too.

This only mimics the protocol. For the real state machine and the real picture, run the
simulator instead (firmware/slime/host, `make sim`), which serves the same API.
Stdlib only, Python 3.9+.
"""
import argparse
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX = os.path.join(HERE, "..", "firmware", "slime", "main", "web", "index.html")

# firmware/slime/main/config.c DEFAULTS
CONFIG = {
    "screen_bright": 100, "sleep_bright": 25, "led_bright": 128, "idle_breath": True, "motor": True,
    "sleep_min": 10, "tilt": True, "mic": True, "clap_sens": 5, "dance": True, "sound": True, "volume": 45,
    "text_blip": False, "show_fps": False, "camera": True, "sit_min": 50, "bgm": True, "cam_pip": False,
    "night_start": 23, "night_end": 7, "focus_min": 25, "ai_comment": True,
}
SFX = ["levelup", "done", "hurt", "ask", "poke", "greet", "dizzy", "startle", "sleep", "wake", "hello", "blip",
       "boot", "sulk", "shy"]
# cc events -> session status (firmware/slime/main/cc_track.c, simplified: no ordering or expiry)
CC_STATUS = {"start": "idle", "prompt": "think", "tool": "work", "tool_ok": "think", "tool_fail": "think",
             "ask": "wait", "denied": "think", "stop": "idle", "fail": "idle", "compact": "think"}
CC_RANK = ["idle", "think", "work", "wait"]
CC_SAY = {"start": "Claude Code 来啦！", "stop": "任务完成！", "tool_fail": "史莱姆受到了伤害！", "fail": "史莱姆受到了伤害！",
          "compact": "脑袋装满了，正在整理记忆……", "ask": "等你批准", "denied": "被拒绝了"}

T0 = time.time()
sessions = {}  # sid -> {"st", "detail", "tools", "last"}
pet = {"state": "idle", "msg": "史莱姆在发呆。", "lv": 1, "exp": 0, "manual": ""}


def log(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


def apply_line(line):
    parts = line.split(" ", 4)
    if parts[0] == "cc" and len(parts) >= 4:
        sid, event = parts[1], parts[3]
        detail = parts[4] if len(parts) > 4 else ""
        if event == "end":
            sessions.pop(sid, None)
        elif event in CC_STATUS:
            s = sessions.setdefault(sid, {"st": "idle", "detail": "", "tools": 0, "last": 0})
            s["tools"] = 0 if event == "prompt" else s["tools"] + (event == "tool")
            if event == "stop":
                pet["exp"] += 2 + min(s["tools"], 30)
            s.update(st=CC_STATUS[event], detail=detail or s["detail"], last=time.time())
        else:
            log("  !! unknown cc event %r: the pet would drop this line" % event)
            return
        st = max((s["st"] for s in sessions.values()), key=CC_RANK.index, default="idle")
        pet["state"] = st
        pet["msg"] = CC_SAY.get(event, "") or detail or st
        busy = sum(s["st"] != "idle" for s in sessions.values())
        log("  -> %s  (%d busy / %d sessions)  %s" % (st.upper(), busy, len(sessions), pet["msg"]))
    elif line.startswith("say "):
        _, mood, text = (line.split(" ", 2) + [""])[:3]
        pet["msg"] = text
        log("  -> AI comment (%s): %s" % (mood, text))
    elif line.startswith("state "):
        pet["state"] = line[6:].strip()
        log("  -> manual state %s" % pet["state"])
    else:
        log("  -> (handled by the firmware: %s)" % line.split(" ")[0])


def status():
    ccs = [{"sid": sid, "st": s["st"], "detail": s["detail"], "tools": s["tools"], "age": round(time.time() - s["last"])}
           for sid, s in sessions.items()]
    top = max((s["st"] for s in sessions.values()), key=CC_RANK.index, default="idle")
    return {
        "state": pet["state"], "msg": pet["msg"], "lv": pet["lv"], "exp": pet["exp"], "need": 8 + pet["lv"] * 4,
        "hp": 100.0, "up": round(time.time() - T0), "perf": {"fps": 60.0, "render": 0.0},
        "bat": {"ok": False, "soc": 0, "chg": False, "mv": 0, "ma": 0},
        "cc": {"status": top, "busy": sum(s["st"] != "idle" for s in sessions.values()), "detail": "", "ok": 0,
               "stale": 0, "sessions": ccs},
        "imu": {"ok": False, "ax": 0, "ay": 0, "az": 0, "tilt": 0, "face": False},
        "mod": {"ok": False, "motion": False, "light": 0},
        "mic": {"ok": False, "db": -90, "floor": -90, "claps": 0},
        "net": {"state": "connected", "ssid": "fake-device", "ip": "127.0.0.1", "rssi": 0, "fails": 0, "reason": 0},
        "heap": {"int": 0, "psram": 0}, "boots": [], "crash": None,
        "fw": {"part": "fake", "state": "valid", "ver": "fake_device.py", "built": ""},
        "clock": time.strftime("%H:%M"), "night": False, "focus": 0, "rest": 0, "demo": False,
        "cam": {"on": False, "ok": False, "tries": 0, "err": "ESP_ERR_NOT_FOUND", "face": False, "n": 0, "fx": 0, "fy": 0,
                "size": 0, "ms": 0, "raw_x": 0, "raw_y": 0, "motion": 0, "mx": 0, "luma": 0, "kp": False, "roll": 0,
                "yaw": 0, "pitch": 0, "sway": 0,
                "slot": {"presence": -1, "desc": -1, "owner": -1, "type": -1, "err": "ESP_OK", "bus_resets": 0}},
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quiet: the panel polls /api/status twice a second
        pass

    def send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            with open(INDEX, "rb") as f:
                self.send(200, f.read(), "text/html; charset=utf-8")
        elif path == "/api/status":
            self.send(200, json.dumps(status(), ensure_ascii=False))
        elif path == "/api/config":
            self.send(200, json.dumps(CONFIG))
        elif path == "/api/sfxr":
            self.send(200, json.dumps({"slots": [{"name": n, "custom": False} for n in SFX]}))
        elif path == "/api/head":
            self.send(200, "[]")
        else:
            self.send(404, "not found", "text/plain")

    def do_POST(self):
        path = self.path.split("?")[0]
        data = self.body()
        if path == "/api/cmd":
            queued = 0
            for line in data.decode("utf-8", "replace").splitlines():
                if not line or line.startswith("wifi"):
                    continue
                log("cmd  " + line)
                apply_line(line)
                queued += 1
            self.send(200, json.dumps({"queued": queued}))
        elif path == "/api/config":
            try:
                o = json.loads(data or b"{}")
            except ValueError:
                return self.send(400, "expected a JSON object", "text/plain")
            for k, v in o.items():  # same rules as web.c: known keys, booleans and 0-255 numbers
                if k not in CONFIG:
                    continue
                if isinstance(CONFIG[k], bool):
                    if isinstance(v, bool):
                        CONFIG[k] = v
                elif isinstance(v, (int, float)) and not isinstance(v, bool):
                    CONFIG[k] = max(0, min(255, int(v)))
            log("config " + json.dumps(o, ensure_ascii=False))
            self.send(200, json.dumps(CONFIG))
        elif path in ("/api/sfxr", "/api/wifi"):
            log("%s (ignored)" % path)
            self.send(200, '{"ok":true}')
        else:
            self.send(404, "not found", "text/plain")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1")
    args = ap.parse_args()
    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    log("fake slime on http://%s:%d/   hook: SLIME_HOST=%s:%d" % (args.bind, args.port, args.bind, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
