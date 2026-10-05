#!/usr/bin/env python3
"""The slime's resident companion on the computer. Stdlib only, Python 3.9+, any OS.

Runs next to the Claude Code hooks and talks to the pet over Wi-Fi:

  * Claude waits for you: when a session asks for a permission or a question, or finishes a
    turn and waits for your next message, the slime says so out loud (after SLIME_NUDGE_ASK_S /
    SLIME_NUDGE_DONE_S seconds, 20 / 30 by default), once per wait; with the camera on it holds
    the line while you are away and says it when you are back.
  * Proactive lines: polls the journal (slime_brain.py) and the device status, and speaks up
    when Claude has waited for your approval too long, commands keep failing, you are still
    working deep in the night or for hours on end, a big day passes a milestone, or (camera)
    you come back after a while. Quiet hours, the focus timer and the demo keep it silent.
  * Picks its moments (slime_rhythm.py learns your usual hours): hello the first time you show
    up in a day, a gentle word when you are over an hour past your usual stop, and lines that
    can wait (milestones, take a break) wait until no Claude session is busy.
  * Reminders the slime was asked to set (slime_agent.py) are said when due.
  * Diary: after midnight it writes yesterday's diary to bridge/.slime/diary/YYYY-MM-DD.md.
  * Voice chat: hold the AI key on the pet and speak; the pet records, this process fetches the
    recording, turns it into text (SLIME_STT_*), answers in the slime's voice with what it
    knows (date and weekday, its own status, today's work, what each Claude session is doing, the last
    few exchanges, yesterday's diary, the weather when asked), can call tools (slime_agent.py), and
    the pet shows the answer.
  * Home Assistant (SLIME_MQTT_URL set): what the pet senses as entities, and commands back
    (slime_mqtt.py), from the same poll of the pet.

Usage:
    python3 slime_buddy.py                 # run (Ctrl-C to stop); -v logs every poll decision
    python3 slime_buddy.py diary [date]    # write (or rewrite) a diary now and print it
    python3 slime_buddy.py ask "今天干了啥"   # a voice question without the microphone
    python3 slime_buddy.py check           # which proactive line would fire right now, and why

Model endpoints and memory location: see slime_brain.py.
"""
import concurrent.futures
import datetime as dt
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slime_agent as agent  # noqa: E402
import slime_brain as brain  # noqa: E402
import slime_mqtt as mqtt  # noqa: E402
import slime_rhythm as rhythm  # noqa: E402
import slime_tts as tts  # noqa: E402

POLL_S = 1.5              # device status (voice needs to feel responsive)
RULES_S = 10
GAP_S = 180               # at least this long between two proactive lines
LOCK_PORT = 47817         # one buddy per computer: a bound localhost port, released on exit
VERBOSE = "-v" in sys.argv
last_status = None        # the pet's latest /api/status, for what the slime knows about itself

FALLBACK = {  # used when the model is unreachable
    "wait": ("worried", "Claude 还在等你点头哦"),
    "fails": ("worried", "别急，慢慢来，会好的"),
    "night": ("worried", "好晚了，早点休息吧"),
    "marathon": ("worried", "干了好久了，起来走走吧"),
    "milestone": ("proud", "今天好厉害，继续加油！"),
    "welcome": ("happy", "你回来啦！"),
    "hello": ("happy", "你来啦，今天也一起加油"),
    "late": ("worried", "比平时晚了，别熬太久哦"),
}
FALLBACK_EN = {
    "wait": ("worried", "Claude is still waiting for your OK"),
    "fails": ("worried", "Easy, one step at a time"),
    "night": ("worried", "It's late, get some sleep"),
    "marathon": ("worried", "Long session! Go stretch a bit"),
    "milestone": ("proud", "Great work today, keep it up!"),
    "welcome": ("happy", "You're back!"),
    "hello": ("happy", "Hi! Let's have a good day"),
    "late": ("worried", "Later than usual, don't stay up"),
}


def tr(zh, en):
    return en if brain.lang() == "en" else zh


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def vlog(*a):
    if VERBOSE:
        log(*a)


def ago(sec):
    sec = int(sec)
    return "%d 秒前" % sec if sec < 60 else ("%d 分钟前" % (sec // 60) if sec < 3600 else "%d 小时前" % (sec // 3600))


# ---------------- what the slime knows ----------------

STATE_WORDS = {"ask": "在等主人批准", "tool": "正在执行", "tool_ok": "正在干活", "tool_fail": "刚失败了一次",
               "prompt": "刚收到新任务，在思考", "stop": "做完了一轮", "fail": "出错停下了", "end": "会话结束了",
               "start": "刚开始", "compact": "在整理记忆", "denied": "被主人拒绝了", "interrupt": "被打断了"}


def sessions(events, now, limit=3):
    """Latest state of the most recently active Claude sessions."""
    last, last_tool = {}, {}
    for e in events:
        sid = e.get("sid")
        if not sid or e.get("ev") not in STATE_WORDS:
            continue
        last[sid] = e
        if e.get("ev") in ("tool", "ask") and e.get("d"):
            last_tool[sid] = e["d"]
    out = []
    for sid, e in sorted(last.items(), key=lambda kv: -kv[1]["t"])[:limit]:
        if now - e["t"] > 6 * 3600:
            continue
        line = "会话 %s：%s %s" % (sid[:4], ago(now - e["t"]), STATE_WORDS[e["ev"]])
        if sid in last_tool:
            line += "（最近的操作：%s）" % last_tool[sid]
        out.append(line)
    return out


WEEKDAYS = "一二三四五六日"


def clock_text(now):
    """'2026年10月3日 星期六（周末） 晚上 19:47'"""
    lt = time.localtime(now)
    h = lt.tm_hour
    part = ("凌晨" if h < 5 else "早上" if h < 9 else "上午" if h < 12 else "中午" if h < 14
            else "下午" if h < 18 else "晚上" if h < 23 else "深夜")
    return "%d年%d月%d日 星期%s%s %s %s" % (lt.tm_year, lt.tm_mon, lt.tm_mday, WEEKDAYS[lt.tm_wday],
                                      "（周末）" if lt.tm_wday >= 5 else "", part, time.strftime("%H:%M", lt))


def device_facts(st):
    """What the pet itself knows right now (its /api/status), as short sentences."""
    if not st:
        return []
    out = []
    if st.get("lv"):
        out.append("你现在 %d 级，经验 %d/%d。" % (st["lv"], st.get("exp", 0), st.get("need", 0)))
    bat = st.get("bat") or {}
    if bat.get("ok"):
        out.append("你的电量 %d%%%s。" % (bat.get("soc", 0), "，正在充电" if bat.get("chg") else ""))
    if st.get("focus", 0) > 0:
        out.append("主人开着专注计时，还剩 %d 分钟。" % max(1, st["focus"] // 60))
    elif st.get("rest", 0) > 0:
        out.append("专注刚结束，主人在休息，还剩 %d 分钟。" % max(1, st["rest"] // 60))
    if st.get("night"):
        out.append("现在是你的免打扰时段。")
    cam = st.get("cam") or {}
    if cam.get("on") and cam.get("ok"):
        out.append("摄像头看到主人在电脑前。" if cam.get("face") else "摄像头现在没看到主人。")
    return out


def memories(events, now):
    """Older things worth remembering: what the owner talked about earlier today and the latest diary."""
    out = []
    talked = [e.get("d", "") for e in events if e.get("ev") == "heard"
              and brain._day(e["t"]) == brain._day(now) and now - e["t"] >= 1800][-4:]
    if talked:
        out.append("主人今天早些时候和你聊过：" + " / ".join(talked))
    for k in (1, 2):
        day = brain._day(now - k * 86400)
        try:
            with open(os.path.join(brain.DIARY_DIR, day + ".md"), encoding="utf-8") as f:
                body = f.read().split("\n---", 1)[0]
        except OSError:
            continue
        body = " ".join(l.strip() for l in body.splitlines() if l.strip() and not l.startswith("#"))
        if body:
            out.append("你%s的日记：%s" % ("昨天" if k == 1 else "前天", body[:160]))
            break
    return out


WEATHER_WORDS = ("天气", "下雨", "下雪", "气温", "温度", "冷不冷", "热不热", "多少度", "带伞", "穿什么", "穿啥",
                 "晴", "刮风", "雾霾", "weather", "rain", "temperature", "umbrella")


def weather():
    """Weather where the computer is (see slime_agent.fetch_weather)."""
    return agent.fetch_weather()


def context(now=None, st=None, question=""):
    """(the last 7 days of events, what the slime knows as text). st: the pet's status, when known."""
    now = time.time() if now is None else now
    week = brain.journal_read(7, now)
    today = [e for e in week if brain._day(e["t"]) == brain._day(now)]
    s = brain.stats(today)
    lines = ["现在是 %s。" % clock_text(now)]
    lines += device_facts(st)
    if any(w in question.lower() for w in WEATHER_WORDS):
        w = weather()
        lines.append("这里的天气：" + w if w else "天气：查不到（没联网或服务不可用），别编天气。")
    lines.append("主人今天和 Claude 的工作：完成 %d 轮任务，工具 %d 次，失败 %d 次，发了 %d 条指令。"
                 % (s["tasks"], s["tools"], s["fails"], s["prompts"]))
    if s["top"]:
        lines.append("今天用得最多的工具：" + "、".join("%s×%d" % kv for kv in s["top"]))
    lines += sessions(week, now)
    lines += memories(week, now)
    usual = rhythm.load(now).describe()
    if usual:
        lines.append("主人的作息（最近几周学到的）：" + "；".join(usual) + "。")
    known = agent.facts()
    if known:
        lines.append("主人让你记住的事：" + "；".join(f["d"] for f in known[-20:]))
    return week, "\n".join(lines)


# ---------------- proactive lines ----------------


class Rules:
    def __init__(self):
        self.fired = {}          # key -> time
        self.face_seen = None    # last time a face was in view (camera working)
        self.away_since = None
        self.rhythm, self.rhythm_at = None, 0

    def learned(self, now):
        """The owner's usual hours, relearned every 10 minutes."""
        if self.rhythm is None or now - self.rhythm_at > 600:
            self.rhythm, self.rhythm_at = rhythm.load(now), now
        return self.rhythm

    def cooled(self, key, sec, now):
        return now - self.fired.get(key, 0) > sec

    def observe(self, st, now):
        cam = (st or {}).get("cam") or {}
        if not (cam.get("on") and cam.get("ok")):
            self.face_seen = self.away_since = None  # no camera: no presence
            return None
        if cam.get("face"):
            back = None
            if self.away_since and now - self.away_since >= 600:
                back = now - self.away_since
            self.face_seen, self.away_since = now, None
            return back
        if self.face_seen and now - self.face_seen > 20 and not self.away_since:
            self.away_since = self.face_seen
        return None

    def check(self, st, now, away=None):
        """(key, rule, situation) for the line to say now, or None."""
        week = brain.journal_read(2, now)
        recent = [e for e in week if now - e["t"] < 600 and e.get("ev") not in ("say", "heard", "reply")]
        # Claude waiting for approval
        last = {}
        for e in week:
            if e.get("sid") and e.get("ev") in STATE_WORDS:
                last[e["sid"]] = e
        for sid, e in last.items():
            if e["ev"] == "ask" and 120 < now - e["t"] < 3600 and self.cooled("wait:" + sid, 600, now):
                return ("wait:" + sid, "wait", "Claude 已经等主人批准 %d 分钟了%s。提醒主人去看看。"
                        % ((now - e["t"]) // 60, "，要做的是：" + e["d"] if e.get("d") else ""))
        if st and (st.get("demo") or st.get("focus", 0) > 0):
            return None  # only the approval reminder gets through the focus timer
        hour = time.localtime(now).tm_hour
        night_key = "night:" + brain._day(now - 6 * 3600)
        if hour < 5 and recent and night_key not in self.fired:
            return (night_key, "night", "现在是凌晨 %d 点，主人还在和 Claude 干活。劝主人休息。" % hour)
        learned = self.learned(now)
        end = learned.end_today(now)
        owner_now = any(now - t < 600 for t in rhythm.owner_times(week))
        if (end and hour >= 20 and now - end >= 3600 and (learned.at(now) or 0) < 0.1 and owner_now
                and night_key not in self.fired):
            return (night_key, "late", "主人平时%s一般 %s 就收工了，现在 %s 还在和 Claude 干活。轻轻提醒主人别太晚。"
                    % ("周末" if rhythm.kind(now) else "工作日", time.strftime("%H:%M", time.localtime(end)),
                       time.strftime("%H:%M", time.localtime(now))))
        if st and st.get("night"):
            return None  # quiet hours: only the reminders above (the pet stays muted anyway)
        hello_key = "hello:" + brain._day(now)
        first = rhythm.first_today(week, now)
        if first and now - first < 600 and hour >= 5 and hello_key not in self.fired:
            usual = learned.usual(rhythm.kind(now))
            early = usual and rhythm.slot(first) < usual[0] - 4
            return (hello_key, "hello", "主人今天第一次来到电脑前，现在是%s%s。和主人打个招呼。"
                    % (clock_text(now), "，比平时早不少" if early else ""))
        # in the middle of something (a Claude session busy in the last 5 minutes): the lines below
        # can wait for a pause, they come up again on the next check
        busy = any(e["ev"] in ("prompt", "tool", "tool_ok", "compact") and now - e["t"] < 300 for e in last.values())
        fails = [e for e in recent if e.get("ev") == "tool_fail"]
        if len(fails) >= 3 and self.cooled("fails", 1200, now):
            return ("fails", "fails", "最近 10 分钟里命令失败了 %d 次，最近一次：%s。鼓励主人。"
                    % (len(fails), fails[-1].get("d") or "未知"))
        active = [e["t"] for e in week if e.get("ev") not in ("say", "heard", "reply")]
        if active and now - active[-1] < 600:
            start = active[-1]
            for t in reversed(active):
                if start - t > 1200:
                    break
                start = t
            if now - start >= 7200 and not busy and self.cooled("marathon", 5400, now):
                return ("marathon", "marathon", "主人已经连续工作 %.1f 小时了，中间没有超过 20 分钟的休息。劝主人起来活动。"
                        % ((now - start) / 3600))
        tasks = sum(1 for e in week if e.get("ev") == "stop" and brain._day(e["t"]) == brain._day(now))
        if tasks >= 10 and not busy:
            key = "milestone:%s:%d" % (brain._day(now), tasks // 10 * 10)
            if key not in self.fired:
                return (key, "milestone", "今天已经完成了 %d 轮任务！为主人庆祝。" % tasks)
        if away and self.cooled("welcome", 1800, now):
            return ("welcome", "welcome", "主人离开了 %d 分钟，刚刚回到电脑前。欢迎主人回来。" % (away // 60))
        return None


# ---------------- Claude waits for you: say so out loud ----------------

NUDGE_ASK_S = float(os.environ.get("SLIME_NUDGE_ASK_S", 20))    # a permission or question: after this long
NUDGE_DONE_S = float(os.environ.get("SLIME_NUDGE_DONE_S", 30))  # a finished turn: after this long
NUDGE_LINES = {
    "ask": ["Claude 等你点头呢", "Claude 需要你确认一下", "Claude 在等你回话"],
    "stop": ["Claude 做完啦，等你看看", "Claude 那边好了，轮到你啦", "Claude 停下来等你了"],
    "many": ["%d 个 Claude 都在等你"],
    "back": ["你回来啦，Claude 在等你"],
}
NUDGE_LINES_EN = {
    "ask": ["Claude needs your OK", "Claude has a question for you"],
    "stop": ["Claude is done, your turn", "Claude finished, take a look"],
    "many": ["%d Claudes are waiting for you"],
    "back": ["Welcome back, Claude is waiting"],
}


def waiting_on_you(events, since=0):
    """sid -> (kind, start) for Claude sessions whose latest state leaves them waiting on you:
    "ask" (a permission or a question) or "stop" (the turn is done, your move). start is when
    that wait began; several ask events in a row (PermissionRequest, then its Notification) are
    one wait."""
    out = {}
    for e in events:
        sid, ev = e.get("sid"), e.get("ev")
        if not sid or ev not in STATE_WORDS:
            continue
        if ev in ("ask", "stop"):
            if out.get(sid, (None,))[0] != ev:
                out[sid] = (ev, e["t"])
        else:
            out.pop(sid, None)
    return {sid: w for sid, w in out.items() if w[1] >= since}


class Nudger:
    """One spoken line per wait, after a short delay (so it stays quiet while you are right there
    and answer at once). With the camera on, it holds the line while you are away and says it when
    you come back. Quiet hours: a finished turn is not worth waking anyone; a question is shown,
    not read aloud."""

    def __init__(self, start):
        self.start = start  # waits from before the buddy started are not ours to announce
        self.done = set()   # (sid, kind, start) already said

    def due(self, events, now, present=True, night=False):
        """(kind, text) to say now, or None."""
        fresh = []
        for sid, (kind, t) in waiting_on_you(events, self.start).items():
            key = (sid, kind, t)
            if key in self.done or now - t > 3600:
                continue
            if night and kind == "stop":
                self.done.add(key)
                continue
            if now - t >= (NUDGE_ASK_S if kind == "ask" else NUDGE_DONE_S):
                fresh.append((key, now - t))
        if not fresh or not present:
            return None
        self.done.update(key for key, _ in fresh)
        lines = NUDGE_LINES_EN if brain.lang() == "en" else NUDGE_LINES
        pick = lambda k: lines[k][int(now) % len(lines[k])]  # noqa: E731
        kind = "ask" if any(k[1] == "ask" for k, _ in fresh) else "stop"
        if len(fresh) > 1:
            return kind, pick("many") % len(fresh)
        if fresh[0][1] > 120 + (NUDGE_ASK_S if kind == "ask" else NUDGE_DONE_S):
            return kind, pick("back")  # it waited for you to come back
        return kind, pick(kind)


def nudge(kind, text):
    """Read the line aloud (when reading aloud is on) and show it: a question goes through the
    pet's "remind" path (it buzzes and keeps the waiting face), a finished turn as a short line."""
    clips = speech("worried" if kind == "ask" else "happy", [text])
    if clips:
        try:
            tts.play(clips[0].result(timeout=TTS_WAIT_S))
        except Exception as e:
            log("nudge: no voice this time (%s)" % (str(e) or type(e).__name__))
    send_line(("say remind %s\n" if kind == "ask" else "talk happy %s\n") % text)
    brain.journal_append("say", text)


def proactive_line(rule, situation):
    try:
        events, ctx = context(st=last_status)
        _, persona = brain.personality(events)
        said = brain.recent_said(events)
        system = (persona + "你要主动对主人说一句不超过 16 个汉字的话。"
                  + ("别和你最近说过的话重复：" + " / ".join(said) + "。" if said else "") + brain.JSON_TAIL)
        got = brain.speak(system, "背景：\n%s\n\n情况：%s" % (ctx, situation))
        if got:
            return got
    except Exception as e:
        vlog("model:", e)
    return (FALLBACK_EN if brain.lang() == "en" else FALLBACK)[rule]


# ---------------- voice ----------------

PUNCT = "，。！？；、,.!?;~～…"


def chunks(text, n=None):
    """Split an answer into dialog lines of at most n characters, at punctuation when possible
    (English: at word boundaries)."""
    if brain.lang() == "en":
        import textwrap
        return textwrap.wrap(text, n or brain.LINE_MAX_EN)
    n = n or brain.LINE_MAX
    out, cur = [], ""
    for ch in text:
        cur += ch
        if ch in PUNCT and len(cur) >= 4:
            out.append(cur)
            cur = ""
        elif len(cur) >= n:
            cut = max((cur.rfind(p) for p in PUNCT), default=-1)
            if 3 <= cut < len(cur) - 1:
                out.append(cur[:cut + 1])
                cur = cur[cut + 1:]
            else:
                out.append(cur)
                cur = ""
    if cur.strip():
        out.append(cur)
    merged = []
    for c in (c.strip() for c in out):  # glue short pieces back together while they fit
        if merged and len(merged[-1]) + len(c) <= n:
            merged[-1] += c
        elif c:
            merged.append(c)
    return merged


CHAT_RULES = (
    "主人在用语音和你聊天。规矩：\n"
    "1. 主人问问题，第一句就直接回答问题本身，用你的常识认真答，答对比卖萌重要；"
    "游戏、电影、书、产品、人名都可以正常说。\n"
    "2. 时间、日期、星期、天气、你的状态以下面的信息为准；不知道就说不知道，别瞎编。\n"
    "3. 问题和写代码无关时，别扯主人的工作，也别夸主人；性格只影响语气，不改变回答的内容。\n"
    "4. 主人闲聊或说心情时，自然接话，可以用上下面的信息和记忆。\n"
    "5. 你有工具：别处的天气、翻记忆和日记、某天的工作、记住/忘掉主人的事、定提醒、操作你自己"
    "（专注计时、音量、亮度、音乐）。下面的信息够用或者是常识问题就直接答，不调工具；"
    "主人让你做事就调工具去做，做完简短说结果。\n"
    "你知道的事：\n")


def answer(question, now=None, st=None):
    """(mood, text) for a spoken question. st: the pet's status (default: the latest one the buddy saw)."""
    now = time.time() if now is None else now
    week, ctx = context(now, last_status if st is None else st, question)
    _, persona = brain.personality(week)
    history = [e for e in week if e.get("ev") in ("heard", "reply") and now - e["t"] < 1800][-6:]
    system = (persona + CHAT_RULES + ctx
              + "\n最后用不超过 60 个字回答，可以是两三句短句。" + brain.JSON_TAIL)
    user = ""
    for e in history:
        user += ("主人：" if e["ev"] == "heard" else "你：") + e.get("d", "") + "\n"
    user += "主人：" + question
    return brain.speak(system, user, max_chars=64, max_tokens=300, temperature=0.6,
                       first=lambda messages: agent.run(messages, max_tokens=300, temperature=0.6, log=log))


def send_line(line, tries=3):
    """A line for the pet's dialog. Its web server answers one request at a time and can be busy
    for a few seconds (a recording being fetched, the camera coming up): wait and try again
    instead of giving up on the first timeout."""
    for attempt in range(tries):
        try:
            return brain.device_cmd(line, timeout=4)
        except OSError as e:  # URLError and timeouts included
            if attempt == tries - 1:
                raise
            log("device busy (%s); sending again" % e)
            time.sleep(1)


TTS_WAIT_S = 20  # longest wait for an answer's voice before showing it silently
VOICE_CHARS = 70  # one clip reads about this much within the pet's limit (tts.MAX_S seconds)


def voice_parts(lines, limit=VOICE_CHARS):
    """Dialog lines grouped into as few parts as fit a clip each: a part is read aloud in one go,
    so the voice does not stop between the lines on the screen."""
    parts = []
    for line in lines:
        if parts and sum(len(x) for x in parts[-1]) + len(line) <= limit:
            parts[-1].append(line)
        else:
            parts.append([line])
    return parts


def speech(mood, texts):
    """Futures for each text's voice (16 kHz PCM for /api/speak), synthesized two at a time so
    the next one is ready while one plays; None when the pet does not want answers read aloud."""
    cfg = tts.device_config()
    if not cfg.get("speak") or (last_status or {}).get("night"):  # night mode: the pet stays quiet
        return None
    pitch = cfg.get("speak_pitch", 100)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)
    clips = [pool.submit(tts.speak_bytes, text, mood, pitch) for text in texts]
    pool.shutdown(wait=False)
    return clips


def talk(mood, text):
    """Show an answer on the pet and read it aloud when that is on. The voice runs through
    without stopping (one clip for the whole answer, or a few for a long one) and the dialog
    lines follow it, each shown when the voice gets to it; without a voice a line stays up for
    its typewriter time."""
    parts = voice_parts(chunks(text))
    clips = speech(mood, ["".join(part) for part in parts])
    next_at = 0
    for i, part in enumerate(parts):
        clip = None
        if clips:
            try:
                clip = clips[i].result(timeout=TTS_WAIT_S)
            except Exception as e:  # no voice this time: the rest is shown silently
                log("speech: synthesis failed (%s); showing the text only" % (str(e) or type(e).__name__))
                for c in clips:
                    c.cancel()
                clips = None
        time.sleep(max(0.0, next_at - time.time()))
        secs = None
        if clip:
            try:
                secs = tts.play(clip)
            except Exception as e:
                log("speech: the pet did not take it:", e)
            if secs is None:  # turned off, night mode, or unreachable: stop synthesizing
                for c in clips or []:
                    c.cancel()
                clips = None
        total = sum(len(line) for line in part) or 1
        start = time.time()
        said = 0
        for line in part:
            if secs:  # the line appears when the voice reaches it
                time.sleep(max(0.0, start + secs * said / total - time.time()))
            else:
                time.sleep(max(0.0, next_at - time.time()))
            send_line("talk %s %s\n" % (mood, line))
            said += len(line)
            # typewriter speed is 24 characters per second, then a moment to read it
            next_at = time.time() + 2.0 + len(line) / 24
        if secs:
            next_at = start + secs + 0.15


def handle_voice(seq):
    try:
        wav = brain.device_get("/api/voice.wav?seq=%d" % seq, timeout=5)
    except Exception as e:
        log("voice: could not fetch recording %d: %s" % (seq, e))
        return
    try:
        text = brain.transcribe(wav)
    except Exception as e:
        log("voice: speech to text failed:", e)
        send_line("talk worried %s\n" % tr("听不懂……语音识别没连上", "Can't understand... no speech-to-text"))
        return
    log("voice: heard", repr(text))
    if not text:
        send_line("talk worried %s\n" % tr("没听清，再说一遍？", "Didn't catch that, say it again?"))
        return
    try:
        got = answer(text)
    except Exception as e:
        log("voice: model failed:", e)
        got = None
    mood, reply = got or ("worried", tr("脑袋转不动了，等会再聊", "My head is stuck, talk later"))
    brain.journal_append("heard", text)
    brain.journal_append("reply", reply)
    log("voice: reply", repr(reply))
    talk(mood, reply)


# ---------------- diary ----------------


def write_diary(day):
    """day: 'YYYY-MM-DD'. Returns the diary text, or None when there was nothing to write about."""
    t = time.mktime(dt.date.fromisoformat(day).timetuple()) + 12 * 3600
    events = [e for e in brain.journal_read(1, t)]
    work = [e for e in events if e.get("ev") not in ("say", "heard", "reply")]
    if not work:
        return None
    s = brain.stats(events)
    first, last = time.strftime("%H:%M", time.localtime(work[0]["t"])), time.strftime("%H:%M", time.localtime(work[-1]["t"]))
    details = []
    for e in work:
        if e.get("ev") in ("tool", "tool_fail") and e.get("d") and e["d"] not in details:
            details.append(e["d"])
    chats = ["%s：%s" % ("主人" if e["ev"] == "heard" else "我", e.get("d", ""))
             for e in events if e.get("ev") in ("heard", "reply")]
    _, persona = brain.personality(brain.journal_read(7, t))
    facts = ("日期：%s\n主人从 %s 忙到 %s，完成 %d 轮任务，调用工具 %d 次，失败 %d 次，等主人批准 %d 次，"
             "平均等了 %d 秒。\n用得最多的工具：%s\n做过的一些事：%s\n我今天说过的话：%s\n和主人的聊天：%s"
             % (day, first, last, s["tasks"], s["tools"], s["fails"], s["asks"], s["wait_avg"],
                "、".join("%s×%d" % kv for kv in s["top"]) or "无", "；".join(details[-25:]) or "无",
                " / ".join(s["said"][-10:]) or "无", " / ".join(chats[-10:]) or "无"))
    text = brain.chat([{"role": "system", "content": persona + "用史莱姆的口吻写今天的日记，150 到 250 字，"
                        "写具体发生的事和你的心情，可以有一点小感想，不要列清单，不要标题。"},
                       {"role": "user", "content": facts}], max_tokens=600, temperature=0.8, timeout=180)
    if not text:
        return None
    os.makedirs(brain.DIARY_DIR, exist_ok=True)
    body = "# %s 史莱姆日记\n\n%s\n\n---\n%s 到 %s · 完成 %d 轮 · 工具 %d 次 · 失败 %d 次\n" % (
        day, text.strip(), first, last, s["tasks"], s["tools"], s["fails"])
    with open(os.path.join(brain.DIARY_DIR, day + ".md"), "w", encoding="utf-8") as f:
        f.write(body)
    return body


def diary_due(now):
    yesterday = brain._day(now - 86400)
    return None if os.path.exists(os.path.join(brain.DIARY_DIR, yesterday + ".md")) else yesterday


# ---------------- main loop ----------------


def single_instance():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", LOCK_PORT))
    except OSError:
        sys.exit("slime_buddy.py 已经在运行了。")
    return s


def say(mood, text):
    brain.device_cmd("say %s %s\n" % (mood, text))
    brain.journal_append("say", text)


def run():
    global last_status
    lock = single_instance()  # noqa: F841  (held until exit)
    log("slime buddy: memory %s" % brain.HOME)
    for line in brain.describe_settings() + tts.describe_settings():
        log("  " + line)
    rules, nudger, st, voice_seen = Rules(), Nudger(time.time()), None, None
    home = mqtt.start(say=lambda text: talk("happy", text))
    if home:
        log("  Home Assistant: MQTT %s, topics slime/%s/..." % (home.client.where(), home.node))
    next_rules, next_nudge, next_diary, last_line, offline_logged = 0, 0, 0, 0, False
    while True:
        now = time.time()
        try:
            st = json.loads(brain.device_get("/api/status"))
            if offline_logged:
                log("device back online")
            offline_logged = False
        except Exception as e:
            if not offline_logged:
                log("device offline (%s); retrying" % e)
            offline_logged, st = True, None
        last_status = st
        if home:
            home.update(st, now)
        voice = (st or {}).get("voice") or {}
        if st and voice_seen is None:
            voice_seen = voice.get("seq", 0)  # recordings made before we started are not ours
        if voice.get("ready") and voice.get("seq", 0) != voice_seen:
            voice_seen = voice["seq"]
            try:
                handle_voice(voice_seen)
            except Exception as e:  # the pet went away mid-answer: this conversation is lost, the buddy is not
                log("voice: gave up on this one:", e)
            last_line = time.time()
        away = rules.observe(st, now)
        if st and now >= next_nudge:
            next_nudge = now + 3
            hit = nudger.due(brain.journal_read(2, now), now, present=rules.away_since is None,
                             night=bool(st.get("night")))
            if hit:
                try:
                    nudge(*hit)
                    last_line = time.time()
                    log("nudge (%s): %s" % hit)
                except Exception as e:
                    log("could not nudge:", e)
        if now >= next_rules or away:
            next_rules = now + RULES_S
            try:
                hook_said = os.path.getmtime(os.path.join(brain.HERE, ".last_comment"))
            except OSError:
                hook_said = 0
            hit = rules.check(st, now, away) if st and now - max(last_line, hook_said) > GAP_S else None
            vlog("rules:", hit)
            if hit and (hit[1] == "wait" or st.get("cc", {}).get("status") != "wait"):
                key, rule, situation = hit
                mood, text = proactive_line(rule, situation)
                try:
                    say("remind" if rule == "wait" else mood, text)
                    rules.fired[key] = now
                    if rule == "hello":
                        rules.fired["welcome"] = now  # one greeting is enough
                    last_line = time.time()
                    log("said (%s): %s" % (rule, text))
                except Exception as e:
                    log("could not say:", e)
        for r in agent.due_reminders(now):
            try:
                send_line("talk happy %s\n" % (tr("提醒：", "Reminder: ") + r["d"]))
                brain.journal_append("say", r["d"])
                last_line = time.time()
                log("reminder:", r["d"])
            except Exception as e:  # offline: say it late rather than never
                log("could not remind (%s); trying again" % e)
                agent.set_reminder(1, r["d"])
        if now >= next_diary:
            next_diary = now + 600
            day = diary_due(now)
            if day and time.localtime(now).tm_hour >= 1:
                try:
                    if write_diary(day):
                        log("diary written:", day)
                        brain.journal_prune(now)
                        if st:
                            say("happy", "昨天的日记写好啦")
                except Exception as e:
                    log("diary failed:", e)
        time.sleep(max(0.2, POLL_S - (time.time() - now)))


def main():
    args = [a for a in sys.argv[1:] if a != "-v"]
    cmd = args[0] if args else "run"
    if cmd == "run":
        try:
            run()
        except KeyboardInterrupt:
            pass
    elif cmd == "diary":
        day = args[1] if len(args) > 1 else brain._day(time.time())
        print(write_diary(day) or "这一天没有可写的事（或者模型没连上）。")
    elif cmd == "ask" and len(args) > 1:
        try:
            st = json.loads(brain.device_get("/api/status"))
        except Exception:
            st = {}
        got = answer(args[1], st=st)
        print(got)
        if got:
            brain.journal_append("heard", args[1])
            brain.journal_append("reply", got[1])
            print(chunks(got[1]))
            try:
                talk(*got)
            except Exception as e:
                print("(没发到设备：%s)" % e)
    elif cmd == "check":
        try:
            st = json.loads(brain.device_get("/api/status"))
        except Exception:
            st = {}
        print(Rules().check(st, time.time()))
        print("\n".join(rhythm.load().describe()) or "作息还在学")
        print(context(st=st)[1])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
