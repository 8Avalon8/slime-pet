#!/usr/bin/env python3
"""The slime's memory and voice on the computer side. Stdlib only, Python 3.9+, any OS.

Shared by slime_hook.py (one-line comments after a Claude turn) and slime_buddy.py (the
resident companion: proactive lines, the daily diary, voice chat).

Memory lives in bridge/.slime/ (override with SLIME_HOME):
    journal/YYYY-MM-DD.jsonl   one line per event: {"t", "sid", "ev", "d"}; the hook writes the
                               Claude Code events, this module adds what the slime said ("say"),
                               heard ("heard") and answered ("reply")
    diary/YYYY-MM-DD.md        the slime's diary, written by slime_buddy.py
Nothing here leaves the computer except the prompts sent to the configured model endpoint.

Personality: the last 7 days of the journal decide a few traits (night owl, confident,
worrier, ...) that are added to every prompt, so the slime's tone drifts with your habits.

Model endpoints (any OpenAI-compatible API):
    SLIME_LLM_URL   default http://localhost:1234/v1 (LM Studio)
    SLIME_LLM_MODEL default: gemma-4-e4b-it if the server lists it
    SLIME_LLM_KEY   sent as "Authorization: Bearer <key>" when set
    SLIME_STT_URL / SLIME_STT_MODEL / SLIME_STT_KEY   speech to text (/audio/transcriptions);
                    default to the LLM URL and key, and model whisper-1

Try it:
    python3 slime_brain.py traits      # the current personality and the stats behind it
    python3 slime_brain.py stats [days]
"""
import datetime as dt
import json
import os
import re
import socket
import sys
import time
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.environ.get("SLIME_HOME") or os.path.join(HERE, ".slime")
JOURNAL_DIR = os.path.join(HOME, "journal")
DIARY_DIR = os.path.join(HOME, "diary")
KEEP_DAYS = 60

LLM_URL = os.environ.get("SLIME_LLM_URL", "http://localhost:1234/v1").rstrip("/")
LLM_KEY = os.environ.get("SLIME_LLM_KEY", "")
LLM_PREFER = ["gemma-4-e4b-it"]  # small and quick; anything else must be named in SLIME_LLM_MODEL
STT_URL = os.environ.get("SLIME_STT_URL", LLM_URL).rstrip("/")
STT_KEY = os.environ.get("SLIME_STT_KEY", LLM_KEY)
STT_MODEL = os.environ.get("SLIME_STT_MODEL", "whisper-1")

LINE_MAX = 17  # one 22 px dialog line
LINE_MAX_EN = 36  # the same line in ASCII
MOODS = ("happy", "proud", "worried", "neutral")

BASE_PERSONA = (
    "你是一只住在桌面上的史莱姆宠物，正在看主人和 Claude Code 一起写代码。"
    "说话是可爱的中文口语，具体、不说空话，不提 AI、模型或 Claude 以外的产品名。"
)

# ---------------- journal ----------------


def _day(t):
    return dt.date.fromtimestamp(t).isoformat()


def journal_append(ev, detail="", sid="", t=None):
    """One event; never raises (the hook must not fail because of the memory)."""
    t = time.time() if t is None else t
    try:
        os.makedirs(JOURNAL_DIR, exist_ok=True)
        line = json.dumps({"t": round(t, 3), "sid": sid, "ev": ev, "d": detail}, ensure_ascii=False)
        # one write() of a short line in append mode: concurrent hooks do not interleave
        with open(os.path.join(JOURNAL_DIR, _day(t) + ".jsonl"), "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def journal_read(days=7, now=None):
    """Events of the last `days` calendar days (today included), oldest first."""
    now = time.time() if now is None else now
    out = []
    for k in range(days - 1, -1, -1):
        path = os.path.join(JOURNAL_DIR, _day(now - k * 86400) + ".jsonl")
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    try:
                        e = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(e, dict) and "t" in e:
                        out.append(e)
        except OSError:
            pass
    out.sort(key=lambda e: e["t"])
    return out


def journal_prune(now=None):
    now = time.time() if now is None else now
    cutoff = _day(now - KEEP_DAYS * 86400)
    try:
        for name in os.listdir(JOURNAL_DIR):
            if name.endswith(".jsonl") and name[:10] < cutoff:
                os.remove(os.path.join(JOURNAL_DIR, name))
    except OSError:
        pass


# ---------------- stats and personality ----------------


def stats(events):
    """Numbers the personality and the diary are made of."""
    s = {"tasks": 0, "tools": 0, "fails": 0, "asks": 0, "denied": 0, "prompts": 0,
         "night": 0, "morning": 0, "events": 0, "minutes": 0, "days": 0, "sessions": 0,
         "wait_avg": 0, "top": [], "said": []}
    minutes, days, sids, pending, waits, tops = set(), set(), set(), {}, [], {}
    for e in events:
        ev, sid, t = e.get("ev"), e.get("sid", ""), e["t"]
        if ev in ("say", "heard", "reply"):
            if ev == "say":
                s["said"].append(e.get("d", ""))
            continue
        s["events"] += 1
        h = dt.datetime.fromtimestamp(t).hour
        s["night"] += h < 5
        s["morning"] += 6 <= h < 9
        minutes.add(int(t // 60))
        days.add(_day(t))
        if sid:
            sids.add(sid)
        if ev == "stop":
            s["tasks"] += 1
        elif ev == "tool":
            s["tools"] += 1
            name = (e.get("d") or "").split(":")[0]
            if name:
                tops[name] = tops.get(name, 0) + 1
        elif ev == "tool_fail":
            s["fails"] += 1
        elif ev == "prompt":
            s["prompts"] += 1
        elif ev == "denied":
            s["denied"] += 1
        if ev == "ask":
            s["asks"] += 1
            pending.setdefault(sid, t)
        elif sid in pending and ev in ("tool", "tool_ok", "tool_fail", "denied", "stop", "prompt"):
            waits.append(t - pending.pop(sid))
    s["minutes"], s["days"], s["sessions"] = len(minutes), len(days), len(sids)
    s["wait_avg"] = round(sum(waits) / len(waits)) if waits else 0
    s["top"] = sorted(tops.items(), key=lambda kv: -kv[1])[:5]
    return s


TRAITS = [
    # (name, test(stats), what it adds to the persona)
    ("夜猫子", lambda s: s["events"] >= 40 and s["night"] / s["events"] > 0.15,
     "你常陪主人熬夜，成了夜猫子，说话带点困意，会心疼主人。"),
    ("早起鸟", lambda s: s["events"] >= 40 and s["morning"] / s["events"] > 0.25,
     "主人常一大早就开工，你也养成了早起的习惯，元气满满。"),
    ("自信", lambda s: s["tools"] >= 50 and s["fails"] / s["tools"] < 0.05,
     "主人很少出错，你变得很自信，爱夸人，偶尔小骄傲。"),
    ("爱操心", lambda s: s["tools"] >= 20 and s["fails"] / s["tools"] > 0.15,
     "你常看到命令失败，变得有点爱操心，但总会鼓励主人别灰心。"),
    ("劳模", lambda s: s["days"] and s["tasks"] / s["days"] >= 20,
     "你们每天都很拼，你是个热血的劳模史莱姆。"),
    ("悠闲", lambda s: 0 < s["events"] and s["days"] and s["tasks"] / s["days"] < 3,
     "最近活不多，你变得慵懒悠闲，说话慢悠悠的。"),
    ("耐心", lambda s: s["asks"] >= 5 and s["wait_avg"] > 120,
     "你常常要等主人批准，学会了耐心，但会小声抱怨等太久。"),
]


def personality(events=None):
    """(trait names, persona text) from the last 7 days."""
    s = stats(journal_read(7) if events is None else events)
    traits = [(n, txt) for n, test, txt in TRAITS if test(s)][:3]
    return [n for n, _ in traits], BASE_PERSONA + "".join(txt for _, txt in traits)


def recent_said(events, n=6):
    return [e.get("d", "") for e in events if e.get("ev") == "say"][-n:]


# ---------------- model ----------------


def _headers(key, content_type="application/json"):
    h = {"Content-Type": content_type}
    if key:
        h["Authorization"] = "Bearer " + key
    return h


_model = None


def pick_model():
    global _model
    if os.environ.get("SLIME_LLM_MODEL"):
        return os.environ["SLIME_LLM_MODEL"]
    if _model is None:
        req = urllib.request.Request(LLM_URL + "/models", headers=_headers(LLM_KEY))
        with urllib.request.urlopen(req, timeout=3) as r:
            ids = [m["id"] for m in json.load(r).get("data", [])]
        _model = next((m for m in LLM_PREFER if m in ids), "")
    return _model or None


def chat(messages, max_tokens=120, temperature=0.9, timeout=90):
    model = pick_model()
    if not model:
        return None
    body = {"model": model, "max_tokens": max_tokens, "temperature": temperature, "messages": messages}
    req = urllib.request.Request(LLM_URL + "/chat/completions", data=json.dumps(body).encode(),
                                 headers=_headers(LLM_KEY))
    with urllib.request.urlopen(req, timeout=timeout) as r:  # the first call may load the model
        return json.load(r)["choices"][0]["message"]["content"]


GLYPH_FILES = [os.path.join(HERE, "..", "firmware", "slime", "components", "slime_core", f)
               for f in ("slime_glyphs.c", "slime_glyphs_ext.c")]


def drawable():
    """Code points the pet can draw on its main dialog line (22 px), from its glyph tables."""
    cps = set(range(0x20, 0x7F))
    for path in GLYPH_FILES:
        try:
            with open(path, encoding="utf-8") as f:
                cps.update(int(m, 16) for m in re.findall(r"\{0x([0-9a-f]+), 2,", f.read()))
        except OSError:
            pass
    return cps


_lang = (0, "zh")


def lang():
    """The pet's UI language, "zh" or "en": SLIME_LANG, else its setting over Wi-Fi (cached a minute), else Chinese."""
    global _lang
    env = os.environ.get("SLIME_LANG", "").lower()
    if env in ("zh", "en"):
        return env
    if time.time() - _lang[0] > 60:
        try:
            got = "en" if json.loads(device_get("/api/config", timeout=1.5)).get("lang") == 1 else "zh"
        except (OSError, ValueError):
            got = _lang[1]
        _lang = (time.time(), got)
    return _lang[1]


def clip(text, max_chars, en):
    if len(text) <= max_chars:
        return text
    text = text[:max_chars]
    return text.rsplit(" ", 1)[0] if en and " " in text else text  # English: don't cut a word in half


def _parse(out, max_chars, en=False):
    m = re.search(r"\{.*\}", out, re.S)
    try:
        data = json.loads(m.group(0)) if m else {"text": out}
    except ValueError:
        data = {"text": out}
    text = clip(" ".join(str(data.get("text", "")).split()).strip("\"'“”"), max_chars, en)
    mood = data.get("mood") if data.get("mood") in MOODS else "neutral"
    return mood, text


def speak(system, user, max_chars=LINE_MAX, max_tokens=120):
    """Ask for {"text","mood"} and keep only what the pet can draw. (mood, text) or None.
    When the pet is set to English, the reply is English, plain ASCII, about twice as many characters."""
    en = lang() == "en"
    if en:
        max_chars = max_chars * LINE_MAX_EN // LINE_MAX
        system += ("\nWhatever the instructions above say about language or length, write the text in casual English, "
                   "plain ASCII only, at most %d characters." % max_chars)
    ok = set(range(0x20, 0x7F)) if en else drawable()
    messages = [{"role": "system", "content": system}, {"role": "user", "content": user}]
    mood, text, fewest = "neutral", "", None
    for attempt in range(3):
        out = chat(messages, max_tokens=max_tokens)
        if out is None:
            if fewest is None:
                return None
            break
        m, t = _parse(out, max_chars, en)
        bad = [ch for ch in t if ord(ch) not in ok]
        if fewest is None or len(bad) < fewest:
            mood, text, fewest = m, t, len(bad)
        if not bad:
            break
        # the pet only has the common characters: ask again, then keep the try that loses the
        # fewest (a sentence with characters dropped out of it reads as nonsense)
        retry = "Say it another way, plain ASCII only, without: " if en else "换一种说法，不要用这些字："
        messages += [{"role": "assistant", "content": out}, {"role": "user", "content": retry + "".join(bad)}]
    text = "".join(ch for ch in text if ord(ch) in ok).strip()
    return (mood, text) if text else None


JSON_TAIL = '只输出 JSON：{"text":"……","mood":"happy|proud|worried|neutral"}。'


def comment(summary):
    """The one-line comment after a Claude turn (slime_hook.py). Remembered in the journal."""
    events = journal_read(7)
    _, persona = personality(events)
    said = recent_said(events)
    system = (persona + "根据下面这一轮工作的摘要，用一句不超过 16 个汉字的点评，可以夸奖、鼓励、调侃或担心，"
              "不要复述命令。" + ("别和你最近说过的话重复：" + " / ".join(said) + "。" if said else "") + JSON_TAIL)
    got = speak(system, summary)
    if got:
        journal_append("say", got[1])
    return got


# ---------------- speech to text ----------------


def transcribe(wav, language=None):
    """WAV bytes -> text, through an OpenAI-compatible /audio/transcriptions endpoint."""
    language = language or lang()
    boundary = uuid.uuid4().hex
    parts = []
    for k, v in (("model", STT_MODEL), ("language", language), ("response_format", "json")):
        parts.append(('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, k, v)).encode())
    parts.append(('--%s\r\nContent-Disposition: form-data; name="file"; filename="voice.wav"\r\n'
                  "Content-Type: audio/wav\r\n\r\n" % boundary).encode() + wav + b"\r\n")
    parts.append(("--%s--\r\n" % boundary).encode())
    req = urllib.request.Request(STT_URL + "/audio/transcriptions", data=b"".join(parts),
                                 headers=_headers(STT_KEY, "multipart/form-data; boundary=" + boundary))
    with urllib.request.urlopen(req, timeout=60) as r:
        return str(json.load(r).get("text", "")).strip()


# ---------------- the device, over Wi-Fi ----------------

ADDR_CACHE = os.path.join(HERE, ".device_addr")  # same file as slime_hook.py: "<ip>" or "fail <time>"
HOSTNAME = "slime.local"


def device_ip():
    ip = os.environ.get("SLIME_HOST") or os.environ.get("SLIME_ADDR", "")  # host[:port], e.g. the simulator
    if ip:
        return ip
    try:
        with open(ADDR_CACHE) as f:
            cached = f.read().strip()
        if cached and not cached.startswith("fail"):
            return cached
    except OSError:
        pass
    try:
        ip = socket.gethostbyname(HOSTNAME)
    except OSError:
        return None
    try:
        tmp = ADDR_CACHE + ".%d" % os.getpid()
        with open(tmp, "w") as f:
            f.write(ip + "\n")
        os.replace(tmp, ADDR_CACHE)
    except OSError:
        pass
    return ip


def device_get(path, timeout=2.0):
    ip = device_ip()
    if not ip:
        raise OSError("device not found")
    with urllib.request.urlopen("http://%s%s" % (ip, path), timeout=timeout) as r:
        return r.read()


def device_cmd(line, timeout=2.0):
    ip = device_ip()
    if not ip:
        raise OSError("device not found")
    req = urllib.request.Request("http://%s/api/cmd" % ip, data=line.encode("utf-8"),
                                 headers={"Content-Type": "text/plain"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "traits"
    if cmd == "traits":
        names, persona = personality()
        print("性格：" + ("、".join(names) if names else "（还没养成，多陪它几天）"))
        print(persona)
    elif cmd == "stats":
        s = stats(journal_read(int(sys.argv[2]) if len(sys.argv) > 2 else 7))
        print(json.dumps(s, ensure_ascii=False, indent=1))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
