#!/usr/bin/env python3
"""Claude Code hook -> ESP-Mosaico slime pet, over Wi-Fi (HTTP) with USB CDC as the fallback.

Reads the hook event JSON on stdin and sends one line to the pet:

    cc <sid8> <ts_ms> <event> [detail]

(protocol: firmware/slime/main/cc_track.h). Stdlib only, Python 3.9+.

It must never affect Claude: no stdout (UserPromptSubmit/SessionStart stdout would be
injected into the context), always exit 0, short timeouts, and it stays off the USB port
while tools/backup_and_flash.py holds the lock. The pet's IP is cached in .device_addr and
re-resolved (slime.local, IPv4 only: a dual-stack lookup waits ~5 s for a missing AAAA).

Manual test, without Claude Code:
    python3 slime_hook.py --send ask "Bash: rm -rf build"
    echo '{"hook_event_name":"UserPromptSubmit","session_id":"0123abcd"}' | python3 slime_hook.py
"""
import fcntl
import glob
import http.client
import json
import os
import re
import socket
import sys
import termios
import time
import zlib

TS = int(time.time() * 1000) & 0xFFFFFFFF  # taken first: async hooks are ordered by this

HERE = os.path.dirname(os.path.abspath(__file__))
PORT_LOCK = os.path.join(HERE, ".port.lock")
ADDR_CACHE = os.path.join(HERE, ".device_addr")  # "<ip>" or "fail <unix time>"
HOSTNAME = "slime.local"
WIFI_RETRY_S = 60  # after a failed lookup, skip Wi-Fi this long
# TinyUSB's default serial "123456" -> /dev/cu.usbmodem1234561. Matching it also keeps us
# away from the ROM download port and from unrelated USB serial devices.
PORT_GLOB = "/dev/cu.usbmodem123456*"
DETAIL_MAX = 40
NEEDS_YOU = {"permission_prompt", "elicitation_dialog", "elicitation_url_dialog", "agent_needs_input"}


def ascii_only(s):
    out, prev_q = [], False
    for ch in str(s):
        ok = " " <= ch <= "~"
        if ok or not prev_q:
            out.append(ch if ok else "?")
        prev_q = not ok
    return " ".join("".join(out).split())


def clip(s, n=DETAIL_MAX):
    s = ascii_only(s)
    return s if len(s) <= n else s[: n - 3] + "..."


SETUP_WORDS = {"cd", "export", "source", ".", "set", "setopt", "sleep", "mkdir"}


def bash_gist(cmd):
    """The command that matters: 'cd x && FOO=1 ~/env/bin/python tool.py -v' -> 'python tool.py -v'."""
    lines = [l for l in cmd.strip().splitlines() if l.strip() and not l.strip().startswith("#")]
    for seg in re.split(r"&&|\|\||;|\n", "\n".join(lines)):
        words = seg.split()
        while words and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]):  # env prefix / assignment
            words.pop(0)
        if not words or words[0] in SETUP_WORDS:
            continue
        words[0] = os.path.basename(words[0])
        return " ".join(words)
    return lines[0].strip() if lines else ""


def tool_detail(name, inp):
    inp = inp if isinstance(inp, dict) else {}
    arg = ""
    if name == "Bash":
        arg = bash_gist(str(inp.get("command", "")))
    elif "file_path" in inp or "notebook_path" in inp:
        arg = os.path.basename(str(inp.get("file_path") or inp.get("notebook_path")))
    elif "pattern" in inp:
        arg = inp["pattern"]
    elif "url" in inp:
        arg = str(inp["url"]).split("//", 1)[-1].split("/", 1)[0]
    elif "query" in inp:
        arg = inp["query"]
    elif "description" in inp:
        arg = inp["description"]
    elif "skill" in inp:
        arg = inp["skill"]
    if name.startswith("mcp__"):  # mcp__Claude_Browser__navigate -> Browser.navigate
        parts = name.split("__")
        server = parts[1].split("_")[-1] if len(parts) > 2 else ""
        name = (server + "." if server else "") + parts[-1]
    return clip(name + (": " + str(arg) if arg else ""))


def translate(ev):
    """Hook JSON -> (event, detail), or None to stay quiet."""
    name = ev.get("hook_event_name", "")
    tool = ev.get("tool_name", "")
    if name == "SessionStart":
        return None if ev.get("source") == "compact" else ("start", "")
    if name == "UserPromptSubmit":
        return ("prompt", "")
    if name == "PreToolUse":
        return ("tool", tool_detail(tool, ev.get("tool_input")))
    if name == "PostToolUse":
        return ("tool_ok", "")
    if name == "PostToolUseFailure":
        if ev.get("is_interrupt"):
            return ("interrupt", "")
        err = str(ev.get("error") or "failed").strip().splitlines()
        return ("tool_fail", clip(tool + ": " + (err[0] if err else "failed")))
    if name == "PermissionRequest":
        return ("ask", tool_detail(tool, ev.get("tool_input")))
    if name == "Notification":
        if ev.get("notification_type") in NEEDS_YOU:
            return ("ask", "")  # keep the detail from PermissionRequest/PreToolUse
        return None
    if name == "PermissionDenied":
        return ("denied", "")
    if name == "Stop":
        return ("stop", "")
    if name == "StopFailure":
        return ("fail", clip(ev.get("error") or ev.get("reason") or "API error"))
    if name == "PreCompact":
        return ("compact", "")
    if name == "SessionEnd":
        return ("end", "")
    return None


def session_tag(sid):
    hexpart = "".join(c for c in str(sid).lower() if c in "0123456789abcdef")[:8]
    return hexpart if len(hexpart) == 8 else "%08x" % (zlib.crc32(str(sid).encode()) & 0xFFFFFFFF)


def _cache(value):
    tmp = ADDR_CACHE + ".%d" % os.getpid()
    with open(tmp, "w") as f:
        f.write(value + "\n")
    os.replace(tmp, ADDR_CACHE)


def _post(ip, data):
    conn = http.client.HTTPConnection(ip, 80, timeout=1.0)
    try:
        conn.request("POST", "/api/cmd", body=data, headers={"Content-Type": "text/plain"})
        r = conn.getresponse()
        return r.status == 200 and b'"queued":0' not in r.read()
    finally:
        conn.close()


def send_wifi(data):
    try:
        with open(ADDR_CACHE) as f:
            cached = f.read().strip()
    except OSError:
        cached = ""
    if cached.startswith("fail "):
        if time.time() - float(cached.split()[1]) < WIFI_RETRY_S:
            return False
        cached = ""
    if cached:
        try:
            if _post(cached, data):
                return True
        except OSError:
            pass
    try:  # IP changed, or first run
        ip = socket.gethostbyname(HOSTNAME)
        if ip != cached and _post(ip, data):
            _cache(ip)
            return True
    except OSError:
        pass
    _cache("fail %d" % time.time())
    return False


def send(line):
    data = line if isinstance(line, bytes) else line.encode("ascii", "replace")
    if not send_wifi(data):
        send_usb(data)


def send_usb(line):
    ports = sorted(glob.glob(PORT_GLOB))
    if not ports:
        return
    with open(PORT_LOCK, "a") as lock:
        for _ in range(15):  # hooks overlap for a few ms at most; the flasher holds it for minutes
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                time.sleep(0.02)
        else:
            return
        # O_NONBLOCK only for open(); the write must then drain before close(), or macOS
        # discards the unsent bytes when no other process holds the port open.
        fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            # Raw mode first: a fresh tty echoes received bytes, which would bounce the pet's
            # own log output back into its line parser.
            attrs = termios.tcgetattr(fd)
            attrs[1] &= ~termios.OPOST
            attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIOFLUSH)
            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_NONBLOCK)
            data = line if isinstance(line, bytes) else line.encode("ascii", "replace")
            os.write(fd, b"\n" + data)  # "\n" ends any echoed fragment
            termios.tcdrain(fd)  # bounded by the hook timeout if the pet ever stops reading
        finally:
            os.close(fd)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--send":
        sid, event, detail = "00007e57", sys.argv[2], " ".join(sys.argv[3:])
    else:
        ev = json.load(sys.stdin)
        out = translate(ev)
        if not out:
            return
        event, detail = out
        sid = session_tag(ev.get("session_id", ""))
    send("cc %s %d %s%s\n" % (sid, TS, event, (" " + clip(detail)) if detail else ""))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
