#!/usr/bin/env python3
"""Tools the slime can use while it answers a voice question. Stdlib only, Python 3.9+.

A small tool-calling loop over the OpenAI-compatible function-calling API (DeepSeek, OpenAI,
recent LM Studio models): the model may call tools for at most MAX_ROUNDS rounds, the calls of a
round run in parallel with a time limit each, then it has to answer. Endpoints that do not take
`tools` fall back to a plain answer (remembered per address). SLIME_TOOLS=0 turns tools off.

Tools: weather anywhere (wttr.in, free, no key), the journal and diaries (search, a given day's
work, a diary), long-term facts the owner asks the slime to remember, reminders, and the pet
itself (focus timer, volume, brightness, music).

Try it:
    python3 slime_agent.py tools                 # the tool definitions sent to the model
    python3 slime_agent.py call weather '{"city":"Tokyo"}'
"""
import concurrent.futures as cf
import datetime as dt
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slime_brain as brain  # noqa: E402

MAX_ROUNDS = 2          # tool rounds before the model must answer
TOOL_TIMEOUT_S = 6      # per tool call; the round waits for the slowest
CHAT_TIMEOUT_S = 25     # per model call while answering out loud
FACTS_FILE = os.path.join(brain.HOME, "facts.json")
REMINDERS_FILE = os.path.join(brain.HOME, "reminders.json")
MAX_FACTS = 60

TOOLS = {}  # name -> (definition, function)
_no_tools = set()  # model addresses that refused `tools`
_lock = threading.Lock()  # facts and reminders files


def tool(description, **params):
    """Register fn as a tool. params: name=(json type, description[, enum]); a name ending in '_'
    is optional (the underscore is dropped)."""
    def wrap(fn):
        props, required = {}, []
        for name, spec in params.items():
            key = name.rstrip("_")
            props[key] = {"type": spec[0], "description": spec[1]}
            if len(spec) > 2:
                props[key]["enum"] = list(spec[2])
            if not name.endswith("_"):
                required.append(key)
        TOOLS[fn.__name__] = ({"type": "function", "function": {
            "name": fn.__name__, "description": description,
            "parameters": {"type": "object", "properties": props, "required": required}}}, fn)
        return fn
    return wrap


def definitions():
    return [d for d, _ in TOOLS.values()]


def call(name, args):
    """Run one tool; always a string for the model (errors included)."""
    if name not in TOOLS:
        return "没有这个工具：%s" % name
    try:
        got = TOOLS[name][1](**(args or {}))
    except TypeError as e:
        return "参数不对：%s" % e
    except Exception as e:  # a tool failing must not end the conversation
        return "出错了：%s" % e
    return got if isinstance(got, str) else json.dumps(got, ensure_ascii=False)


# ---------------- weather ----------------

_weather = {}  # city -> (time, text)


def weather_text(data):
    def desc(d):
        for k in ("lang_zh", "weatherDesc"):
            if d.get(k):
                return d[k][0].get("value", "").strip()
        return ""

    cur = data["current_condition"][0]
    area = (data.get("nearest_area") or [{}])[0]
    place = (area.get("areaName") or [{}])[0].get("value", "") if area else ""
    parts = ["%s现在%s，%s°C，体感 %s°C，湿度 %s%%" % (place + "：" if place else "", desc(cur), cur["temp_C"],
                                                cur["FeelsLikeC"], cur["humidity"])]
    for name, day in zip(("今天", "明天", "后天"), data.get("weather", [])):
        hourly = day.get("hourly") or []
        rain = max((int(h.get("chanceofrain", 0)) for h in hourly), default=0)
        mid = desc(hourly[len(hourly) // 2]) if hourly else ""
        parts.append("%s%s，%s~%s°C，降雨概率最高 %d%%" % (name, mid, day["mintempC"], day["maxtempC"], rain))
    return "；".join(parts) + "。"


def fetch_weather(city="", timeout=4):
    """Text for today to the day after tomorrow, cached 30 minutes; None when unavailable.
    No city: SLIME_CITY, else wttr.in guesses from the IP. SLIME_WEATHER=0 turns it off."""
    if os.environ.get("SLIME_WEATHER", "1") == "0":
        return None
    city = (city or os.environ.get("SLIME_CITY", "")).strip()
    hit = _weather.get(city)
    if hit and time.time() - hit[0] < 1800:
        return hit[1]
    got = None
    try:
        req = urllib.request.Request("https://wttr.in/%s?format=j1&lang=zh" % urllib.parse.quote(city),
                                     headers={"User-Agent": "curl/8"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            got = weather_text(json.load(r))
    except Exception:  # offline, blocked, or the format changed
        pass
    _weather[city] = (time.time(), got)
    return got


@tool("查天气：现在、今天、明天、后天的天气和气温。", city_=("string", "城市名，拼音或英文更准，如 Beijing；不填就是主人所在地"))
def weather(city=""):
    return fetch_weather(city) or "天气服务现在连不上。"


# ---------------- memory ----------------


def _diary(day):
    try:
        with open(os.path.join(brain.DIARY_DIR, day + ".md"), encoding="utf-8") as f:
            body = f.read().split("\n---", 1)[0]
    except OSError:
        return None
    return " ".join(l.strip() for l in body.splitlines() if l.strip() and not l.startswith("#"))


def _parse_day(date):
    date = (date or "").strip()
    today = dt.date.today()
    rel = {"今天": 0, "today": 0, "昨天": 1, "yesterday": 1, "前天": 2}
    if date in rel:
        return (today - dt.timedelta(days=rel[date])).isoformat()
    return dt.date.fromisoformat(date).isoformat()


@tool("读你某一天写的日记。", date=("string", "日期 YYYY-MM-DD，或 今天/昨天/前天"))
def read_diary(date):
    day = _parse_day(date)
    return _diary(day) or "%s 没有日记。" % day


@tool("在记忆里搜关键词：过去的聊天、Claude 做过的操作、日记。用于“我上次说过……”“之前做过……”这类问题。",
      keyword=("string", "要找的词，越短越好"), days_=("integer", "往前找几天，默认 30"))
def search_memory(keyword, days=30):
    kw = keyword.strip().lower()
    if not kw:
        return "关键词是空的。"
    days = max(1, min(int(days), brain.KEEP_DAYS))
    now = time.time()
    hits = []
    words = {"heard": "主人说", "reply": "你答", "say": "你说", "tool": "Claude 操作", "tool_fail": "Claude 失败",
             "prompt": "主人给 Claude 的指令", "ask": "Claude 请求批准"}
    for e in brain.journal_read(days, now):
        d = str(e.get("d", ""))
        if kw in d.lower():
            hits.append("%s %s：%s" % (time.strftime("%m-%d %H:%M", time.localtime(e["t"])),
                                      words.get(e.get("ev"), e.get("ev")), d[:80]))
    for k in range(days):
        day = brain._day(now - k * 86400)
        text = _diary(day)
        if text and kw in text.lower():
            i = text.lower().index(kw)
            hits.append("%s 日记：…%s…" % (day, text[max(0, i - 30):i + 50]))
    if not hits:
        return "最近 %d 天的记忆里没有“%s”。" % (days, keyword)
    return "\n".join(hits[-10:])


@tool("某一天主人和 Claude 干了什么：任务数、工具次数、失败、忙碌时段和做过的操作。",
      date=("string", "日期 YYYY-MM-DD，或 今天/昨天/前天"))
def work_day(date):
    day = _parse_day(date)
    t = time.mktime(dt.date.fromisoformat(day).timetuple()) + 12 * 3600
    events = [e for e in brain.journal_read(1, t) if e.get("ev") not in ("say", "heard", "reply")]
    if not events:
        return "%s 没有工作记录。" % day
    s = brain.stats(events)
    details = []
    for e in events:
        if e.get("ev") in ("prompt", "tool", "tool_fail") and e.get("d") and e["d"] not in details:
            details.append(e["d"][:60])
    return ("%s：%s 到 %s，完成 %d 轮任务，工具 %d 次，失败 %d 次，等批准 %d 次。常用工具：%s。做过的事：%s"
            % (day, time.strftime("%H:%M", time.localtime(events[0]["t"])),
               time.strftime("%H:%M", time.localtime(events[-1]["t"])), s["tasks"], s["tools"], s["fails"],
               s["asks"], "、".join("%s×%d" % kv for kv in s["top"]) or "无", "；".join(details[-15:]) or "无"))


def _load(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def _save(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def facts():
    """What the owner asked the slime to remember: [{"t", "d"}], oldest first."""
    got = _load(FACTS_FILE, [])
    return got if isinstance(got, list) else []


@tool("长期记住关于主人的一件事（喜好、习惯、重要日子、正在做的项目）。主人说“记住……”时用。",
      fact=("string", "要记住的事，一句话，用第三人称写主人，如“主人不吃香菜”"))
def remember(fact):
    fact = " ".join(fact.split())[:100]
    if not fact:
        return "没有要记的内容。"
    with _lock:
        got = [f for f in facts() if f.get("d") != fact]
        got.append({"t": round(time.time()), "d": fact})
        _save(FACTS_FILE, got[-MAX_FACTS:])
    return "记住了：" + fact


@tool("忘掉之前记住的一件事。", keyword=("string", "那件事里的词"))
def forget(keyword):
    with _lock:
        got = facts()
        keep = [f for f in got if keyword not in f.get("d", "")]
        if len(keep) == len(got):
            return "没有记过含“%s”的事。" % keyword
        _save(FACTS_FILE, keep)
    return "忘掉了 %d 件事。" % (len(got) - len(keep))


# ---------------- reminders ----------------


def reminders():
    got = _load(REMINDERS_FILE, [])
    return got if isinstance(got, list) else []


@tool("定个提醒：到点时你会在屏幕上提醒主人。",
      minutes=("integer", "多少分钟后提醒，1 到 1440"), text=("string", "到时候要说的话，16 个字以内"))
def set_reminder(minutes, text):
    minutes = int(minutes)
    if not 1 <= minutes <= 1440:
        return "只能定 1 分钟到 24 小时之内的提醒。"
    due = time.time() + minutes * 60
    with _lock:
        got = reminders()
        got.append({"due": round(due), "d": " ".join(text.split())[:40]})
        _save(REMINDERS_FILE, sorted(got, key=lambda r: r["due"]))
    return "好了，%s 提醒（%d 分钟后）。" % (time.strftime("%H:%M", time.localtime(due)), minutes)


@tool("看还没到点的提醒，或者取消提醒。", cancel_=("string", "要取消的提醒里的词；不填就只是列出来"))
def list_reminders(cancel=""):
    with _lock:
        got = reminders()
        if cancel:
            keep = [r for r in got if cancel not in r.get("d", "")]
            _save(REMINDERS_FILE, keep)
            return "取消了 %d 个提醒。" % (len(got) - len(keep))
    if not got:
        return "没有提醒。"
    return "；".join("%s %s" % (time.strftime("%m-%d %H:%M", time.localtime(r["due"])), r["d"]) for r in got)


def due_reminders(now=None):
    """Pop the reminders whose time has come."""
    now = time.time() if now is None else now
    with _lock:
        got = reminders()
        due = [r for r in got if r.get("due", 0) <= now]
        if due:
            _save(REMINDERS_FILE, [r for r in got if r.get("due", 0) > now])
    return due


# ---------------- the pet ----------------


def _post_config(values):
    ip = brain.device_ip()
    if not ip:
        raise OSError("找不到设备")
    req = urllib.request.Request("http://%s/api/config" % ip, data=json.dumps(values).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=3) as r:
        r.read()


@tool("操作你自己（桌宠设备）：专注计时、音量、屏幕亮度、背景音乐。",
      action=("string", "要做的事", ("focus_start", "focus_stop", "volume", "brightness", "music_on", "music_off")),
      value_=("integer", "focus_start: 分钟数 5~90（不填用上次的）；volume: 0~100；brightness: 10~100"))
def device(action, value=None):
    if action == "focus_start":
        if value:
            _post_config({"focus_min": max(5, min(90, int(value)))})
        brain.device_cmd("focus start\n", timeout=3)
        return "专注计时开始了%s。" % ("，%d 分钟" % int(value) if value else "")
    if action == "focus_stop":
        brain.device_cmd("focus stop\n", timeout=3)
        return "专注计时关了。"
    if action == "volume":
        if value is None:
            return "要给音量 0~100。"
        value = max(0, min(100, int(value)))
        _post_config({"volume": value, "sound": value > 0})
        return "音量调到 %d。" % value
    if action == "brightness":
        if value is None:
            return "要给亮度 10~100。"
        value = max(10, min(100, int(value)))
        _post_config({"screen_bright": value})
        return "亮度调到 %d。" % value
    if action in ("music_on", "music_off"):
        _post_config({"bgm": action == "music_on"})
        return "背景音乐%s了。" % ("开" if action == "music_on" else "关")
    return "不会做：%s" % action


# ---------------- the loop ----------------


def tools_on():
    return os.environ.get("SLIME_TOOLS", "1") != "0" and brain.settings()["llm_url"][0] not in _no_tools


def run(messages, max_tokens=300, temperature=0.6, log=None):
    """Let the model call tools, then return its final text (appending the tool turns to messages so
    later retries see them). None when no model is set."""
    url = brain.settings()["llm_url"][0]
    for rnd in range(MAX_ROUNDS + 1):
        offer = definitions() if rnd < MAX_ROUNDS and tools_on() else None
        try:
            msg = brain.chat_message(messages, max_tokens, temperature, CHAT_TIMEOUT_S, tools=offer)
        except urllib.error.HTTPError as e:
            if offer and e.code in (400, 404, 422) and rnd == 0:  # this endpoint does not take tools
                _no_tools.add(url)
                if log:
                    log("tools: %s refused them (%s); answering without" % (url, e.code))
                continue
            raise
        if msg is None:
            return None
        calls = msg.get("tool_calls") or []
        if not calls or not offer:
            return msg.get("content") or ""
        messages.append({"role": "assistant", "content": msg.get("content") or "", "tool_calls": calls})
        pool = cf.ThreadPoolExecutor(len(calls))
        futs = []
        for c in calls:
            fn = c.get("function") or {}
            try:
                args = json.loads(fn.get("arguments") or "{}")
            except ValueError:
                args = {}
            futs.append(pool.submit(call, fn.get("name", ""), args if isinstance(args, dict) else {}))
        deadline = time.time() + TOOL_TIMEOUT_S
        for c, f in zip(calls, futs):
            try:
                out = f.result(timeout=max(0.0, deadline - time.time()))
            except cf.TimeoutError:
                out = "超时了，没拿到结果。"
            if log:
                log("tool %s(%s) -> %s" % ((c.get("function") or {}).get("name"),
                                           (c.get("function") or {}).get("arguments"), out[:120]))
            messages.append({"role": "tool", "tool_call_id": c.get("id", ""), "content": out[:1500]})
        pool.shutdown(wait=False)  # a stuck call finishes on its own; the answer does not wait for it
    return ""


def main():
    args = sys.argv[1:]
    if args[:1] == ["tools"]:
        print(json.dumps(definitions(), ensure_ascii=False, indent=1))
    elif args[:1] == ["call"] and len(args) >= 2:
        print(call(args[1], json.loads(args[2]) if len(args) > 2 else {}))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
