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
import subprocess
import sys
import time
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


def expect(what, cond, timeout=4.0):
    """Poll /api/status until cond(status) holds."""
    t0, s = time.time(), None
    while time.time() - t0 < timeout:
        s = status()
        if cond(s):
            print("ok    %s" % what)
            return s
        time.sleep(0.1)
    failures.append(what)
    print("FAIL  %s  (state=%s msg=%r)" % (what, s and s["state"], s and s["msg"]))
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

    # the bridge's AI comment line
    http("/api/cmd", "say happy 测试通过，真棒！")
    expect("say -> comment on the dialog", lambda s: "测试通过" in s["msg"])
    shot("10_comment")

    # settings through the web panel's API
    http("/api/config", json.dumps({"volume": 30}))
    v = json.loads(http("/api/config"))["volume"]
    (print("ok    config round trip") if v == 30 else (failures.append("config round trip"), print("FAIL  config round trip")))


if __name__ == "__main__":
    sys.exit(main())
