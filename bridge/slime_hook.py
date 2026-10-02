#!/usr/bin/env python3
"""Claude Code hook -> ESP-Mosaico slime pet, over Wi-Fi (HTTP) with USB CDC as the fallback.

Reads the hook event JSON on stdin and sends one line to the pet:

    cc <sid8> <ts_ms> <event> [detail]

(protocol: firmware/slime/main/cc_track.h). Stdlib only, Python 3.9+.

It must never affect Claude: no stdout (UserPromptSubmit/SessionStart stdout would be
injected into the context), always exit 0, short timeouts, and it stays off the USB port
while tools/backup_and_flash.py holds the lock. The pet's IP is cached in .device_addr and
re-resolved (slime.local, IPv4 only: a dual-stack lookup waits ~5 s for a missing AAAA).

Every event is also appended to the slime's journal (slime_brain.py, bridge/.slime/), which
its personality, diary and voice chat are made from.

After each Claude turn that used tools, a detached child asks a model (LM Studio,
http://localhost:1234 by default) for a one-line comment in the pet's voice and sends it as
"say <mood> <text>". SLIME_AI=0 turns it off; the endpoint, model and API key are set in
slime_brain.py (SLIME_LLM_URL / SLIME_LLM_MODEL / SLIME_LLM_KEY). With a cloud API the turn
summary leaves this computer. Comments follow the pet's language setting (SLIME_LANG=zh|en overrides).

SLIME_HOST=<host>[:port] sends everything there instead (no cache, no USB fallback), e.g. to
the simulator or bridge/fake_device.py: SLIME_HOST=127.0.0.1:8080.

Manual test, without Claude Code:
    python3 slime_hook.py --send ask "Bash: rm -rf build"
    python3 slime_hook.py --comment ~/.claude/projects/<project>/<session>.jsonl
    echo '{"hook_event_name":"UserPromptSubmit","session_id":"0123abcd"}' | python3 slime_hook.py
"""
import glob
import http.client
import json
import os
import re
import socket
import sys
import time
import zlib

WINDOWS = os.name == "nt"
if WINDOWS:
    import msvcrt
    import winreg
else:
    import fcntl
    import termios

TS_S = time.time()
TS = int(TS_S * 1000) & 0xFFFFFFFF  # taken first: async hooks are ordered by this

HERE = os.path.dirname(os.path.abspath(__file__))
PORT_LOCK = os.path.join(HERE, ".port.lock")
ADDR_CACHE = os.path.join(HERE, ".device_addr")  # "<ip>" or "fail <unix time>"
HOSTNAME = "slime.local"
SLIME_HOST = os.environ.get("SLIME_HOST", "")  # testing without the board: simulator or fake_device.py
WIFI_RETRY_S = 60  # after a failed lookup, skip Wi-Fi this long
# TinyUSB's default serial "123456" -> /dev/cu.usbmodem1234561 (macOS),
# /dev/serial/by-id/usb-Espressif_..._123456-if00 (Linux). Matching it also keeps us away
# from the ROM download port and from unrelated USB serial devices. Windows has no stable
# name, so there we look the COM port up by Espressif's USB vendor id (see win_ports).
# SLIME_PORT (e.g. COM5 or /dev/ttyACM0) overrides the search.
PORT_GLOBS = ["/dev/cu.usbmodem123456*", "/dev/serial/by-id/*_123456-if*"]
ESPRESSIF_VID = "VID_303A"
ROM_PID = "PID_1001"  # the chip's built-in USB-Serial/JTAG (download mode), not our CDC
PORT_HINT = "COMx (USB VID 303A)" if WINDOWS else " / ".join(PORT_GLOBS)
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


def _post(ip, data, port=80):
    conn = http.client.HTTPConnection(ip, port, timeout=1.0)
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
    data = line if isinstance(line, bytes) else line.encode("utf-8")  # "say" lines carry Chinese
    if SLIME_HOST:
        host, _, port = SLIME_HOST.rpartition(":") if ":" in SLIME_HOST else (SLIME_HOST, "", "80")
        try:
            _post(host, data, int(port))
        except OSError:
            pass
        return
    if not send_wifi(data):
        send_usb(data)


def _reg_subkeys(key):
    i = 0
    while True:
        try:
            yield winreg.EnumKey(key, i)
        except OSError:
            return
        i += 1


def win_ports():
    """COM ports of present Espressif USB devices, except the ROM download port."""
    present = set()
    try:  # only ports whose device is plugged in right now
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as k:
            i = 0
            while True:
                try:
                    present.add(str(winreg.EnumValue(k, i)[1]).upper())
                except OSError:
                    break
                i += 1
    except OSError:
        return []
    ports = []
    root = r"SYSTEM\CurrentControlSet\Enum\USB"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, root) as usb:
            for dev in _reg_subkeys(usb):  # e.g. VID_303A&PID_4001&MI_00
                ids = dev.upper().split("&")
                if ESPRESSIF_VID not in ids or ROM_PID in ids:
                    continue
                with winreg.OpenKey(usb, dev) as d:
                    for inst in _reg_subkeys(d):
                        try:
                            with winreg.OpenKey(d, inst + r"\Device Parameters") as p:
                                name = str(winreg.QueryValueEx(p, "PortName")[0]).upper()
                        except OSError:
                            continue
                        if name in present and name not in ports:
                            ports.append(name)
    except OSError:
        pass
    return sorted(ports, key=lambda n: int(re.sub(r"\D", "", n) or 0))


def find_ports():
    if os.environ.get("SLIME_PORT"):
        return [os.environ["SLIME_PORT"]]
    if WINDOWS:
        return win_ports()
    for pattern in PORT_GLOBS:
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports
    return []


def try_lock(f):
    """Non-blocking exclusive lock on the shared port lock file; released when f is closed."""
    try:
        if WINDOWS:
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def _write_win(port, data):
    # CDC-ACM ignores baud/line settings and Windows has no tty echo, so a plain unbuffered
    # handle is enough; WriteFile returns once the driver has the bytes.
    name = port if port.startswith("\\\\") else "\\\\.\\" + port
    with open(name, "r+b", buffering=0) as f:
        f.write(b"\n" + data)


def send_usb(line):
    ports = find_ports()
    if not ports:
        return
    with open(PORT_LOCK, "a+") as lock:
        for _ in range(15):  # hooks overlap for a few ms at most; the flasher holds it for minutes
            if try_lock(lock):
                break
            time.sleep(0.02)
        else:
            return
        data = line if isinstance(line, bytes) else line.encode("ascii", "replace")
        if WINDOWS:
            _write_win(ports[0], data)
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
            os.write(fd, b"\n" + data)  # "\n" ends any echoed fragment
            termios.tcdrain(fd)  # bounded by the hook timeout if the pet ever stops reading
        finally:
            os.close(fd)


# ---------------- AI comments (the model call and the memory live in slime_brain.py) ----------------

COMMENT_GAP_S = 30
LAST_COMMENT = os.path.join(HERE, ".last_comment")


def _text_of(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return " ".join(b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text")
    return ""


def turn_summary(path):
    """The last turn of a Claude Code transcript (JSONL), condensed for the model."""
    entries = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f.readlines()[-400:]:
            try:
                entries.append(json.loads(line))
            except ValueError:
                pass
    start = 0
    for i, e in enumerate(entries):  # last real prompt (tool results also come as "user")
        if e.get("type") == "user" and _text_of(e.get("message", {}).get("content")).strip():
            start = i
    prompt, tools, failures, reply = "", [], 0, ""
    for e in entries[start:]:
        content = e.get("message", {}).get("content")
        if e.get("type") == "user":
            if not prompt:
                prompt = _text_of(content).strip()
            for b in content if isinstance(content, list) else []:
                if isinstance(b, dict) and b.get("type") == "tool_result" and b.get("is_error"):
                    failures += 1
        elif e.get("type") == "assistant":
            for b in content if isinstance(content, list) else []:
                if not isinstance(b, dict):
                    continue
                if b.get("type") == "tool_use":
                    tools.append(tool_detail(b.get("name", ""), b.get("input")))
                elif b.get("type") == "text" and b.get("text", "").strip():
                    reply = b["text"].strip()
    if not tools:
        return None  # a chat turn: nothing worth commenting on
    counts = {}
    for t in tools:
        k = t.split(":")[0]
        counts[k] = counts.get(k, 0) + 1
    return (
        "主人的请求：%s\n工具：%s（共 %d 次，失败 %d 次）\n代表性操作：%s\nClaude 最后说：%s"
        % (prompt[:300], "、".join("%s×%d" % kv for kv in counts.items()), len(tools), failures,
           "；".join(tools[-6:]), reply[:600])
    )


def comment(path):
    try:
        if time.time() - os.path.getmtime(LAST_COMMENT) < COMMENT_GAP_S:
            return
    except OSError:
        pass
    summary = turn_summary(path)
    if not summary:
        return
    import slime_brain
    got = slime_brain.comment(summary)
    if not got:
        return
    open(LAST_COMMENT, "w").close()
    send("say %s %s\n" % got)


def spawn_comment(ev):
    """Fork off the slow part: the hook itself must return at once."""
    path = ev.get("transcript_path")
    if os.environ.get("SLIME_AI", "1") == "0" or not path or not os.path.isfile(path):
        return
    import subprocess
    kw = {"start_new_session": True}
    if WINDOWS:  # no console window, and not killed along with the hook's process group
        kw = {"creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
              | getattr(subprocess, "CREATE_NO_WINDOW", 0)}
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "--comment", path],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     close_fds=True, **kw)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--comment":
        comment(sys.argv[2])
        return
    if len(sys.argv) >= 3 and sys.argv[1] == "--send":
        sid, event, detail = "00007e57", sys.argv[2], " ".join(sys.argv[3:])
    else:
        ev = json.loads(sys.stdin.buffer.read().decode("utf-8", "replace"))  # not the ANSI code page
        out = translate(ev)
        if not out:
            return
        event, detail = out
        sid = session_tag(ev.get("session_id", ""))
    if not sys.argv[1:]:  # first: the device may be slow to answer, the journal never is
        import slime_brain
        slime_brain.journal_append(event, clip(detail) if detail else "", sid, TS_S)
    send("cc %s %d %s%s\n" % (sid, TS, event, (" " + clip(detail)) if detail else ""))
    if not sys.argv[1:] and event == "stop":
        spawn_comment(ev)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
