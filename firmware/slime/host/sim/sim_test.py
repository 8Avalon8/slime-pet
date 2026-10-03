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


if __name__ == "__main__":
    sys.exit(main())
