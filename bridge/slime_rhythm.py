#!/usr/bin/env python3
"""When the owner is usually at the computer, learned from the journal. Stdlib only.

The day is cut into 96 quarter hours, workdays and weekends apart. On a day the owner used
the computer, a quarter hour counts as "there" when they did something in it (opened a Claude
session, sent a prompt, turned a request down, interrupted, talked to the slime), and so do
the quarter hours between two such moments less than 45 minutes apart. A quarter hour's
weight is the share of recent days of that kind on which the owner was there; recent days
count more (half-life 7 days, last 28 days), so a new routine takes over in a couple of weeks.

The buddy uses it to pick its moments: it says hello the first time you show up in a day,
tells you it is later than you usually stop, and the chat knows your usual hours.

Usage:
    python3 slime_rhythm.py          # your usual hours and a 24-hour chart of them
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slime_brain as brain  # noqa: E402

OWNER_EVENTS = ("start", "prompt", "denied", "interrupt", "heard")  # things only the owner does
SLOTS = 96          # quarter hours in a day
DAYS = 28           # how far back it looks
HALF_LIFE = 7.0     # days
FILL_S = 45 * 60    # two moments this close: there in between too
MIN_DAYS = 5        # active days of a kind before it says anything about that kind
USUAL = 0.25        # a quarter hour with at least this weight is "usual"
KINDS = ("workday", "weekend")


def slot(t):
    lt = time.localtime(t)
    return lt.tm_hour * 4 + lt.tm_min // 15


def kind(t):
    return 1 if time.localtime(t).tm_wday >= 5 else 0


def hhmm(s):
    return "%02d:%02d" % (s // 4, s % 4 * 15)


def owner_times(events):
    return [e["t"] for e in events if e.get("ev") in OWNER_EVENTS]


def day_slots(times):
    """Quarter hours of one day the owner was there, from the times they did something."""
    out = set()
    times = sorted(times)
    for i, t in enumerate(times):
        out.add(slot(t))
        if i and t - times[i - 1] <= FILL_S:
            a, b = slot(times[i - 1]), slot(t)
            out.update(range(a, b + 1) if a <= b else ())
    return out


class Rhythm:
    def __init__(self, weights, days):
        self.w = weights    # [kind][slot] -> 0..1
        self.days = days    # [kind] -> active days seen

    def mature(self, k):
        return self.days[k] >= MIN_DAYS

    def at(self, t):
        """Weight of the quarter hour t falls in, or None while it does not know that kind of day."""
        k = kind(t)
        return self.w[k][slot(t)] if self.mature(k) else None

    def usual(self, k):
        """(first, last) usual quarter hour of that kind of day, or None."""
        if not self.mature(k):
            return None
        hot = [s for s in range(SLOTS) if self.w[k][s] >= USUAL]
        return (hot[0], hot[-1]) if hot else None

    def end_today(self, t):
        """Time on t's day the usual stretch ends (end of its last quarter hour), or None."""
        u = self.usual(kind(t))
        if not u:
            return None
        lt = time.localtime(t)
        return time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1)) + (u[1] + 1) * 900

    def describe(self):
        """'工作日通常 09:30 到 19:00 在电脑前' sentences, empty while it is still learning."""
        out = []
        for k, name in enumerate(("工作日", "周末")):
            u = self.usual(k)
            if u:
                out.append("%s通常 %s 到 %s 在电脑前" % (name, hhmm(u[0]), hhmm((u[1] + 1) % SLOTS)))
        return out

    def chart(self, k):
        """24 characters, one per hour: how likely the owner is there."""
        bars = " ▁▂▃▄▅▆▇█"
        return "".join(bars[min(8, int(round(max(self.w[k][h * 4:h * 4 + 4]) * 8)))] for h in range(24))


def learn(events, now=None):
    """Rhythm from journal events (the last DAYS days; today is not counted, it is not over)."""
    now = time.time() if now is None else now
    today = brain._day(now)
    by_day = {}
    for t in owner_times(events):
        d = brain._day(t)
        if d != today and now - t < DAYS * 86400:
            by_day.setdefault(d, []).append(t)
    sums = [[0.0] * SLOTS for _ in KINDS]
    total, days = [0.0, 0.0], [0, 0]
    for d, times in by_day.items():
        k = kind(times[0])
        w = 0.5 ** ((now - times[0]) / 86400 / HALF_LIFE)
        total[k] += w
        days[k] += 1
        for s in day_slots(times):
            sums[k][s] += w
    weights = [[sums[k][s] / total[k] if total[k] else 0.0 for s in range(SLOTS)] for k in range(2)]
    return Rhythm(weights, days)


def load(now=None):
    return learn(brain.journal_read(DAYS + 1, now), now)


def first_today(events, now, quiet_s=6 * 3600):
    """Time the owner first showed up today, when that came after a long quiet (so a night owl
    still going after midnight is not "first"); None when they have not shown up yet."""
    today = brain._day(now)
    prev = None
    for t in owner_times(events):
        if brain._day(t) == today:
            return t if prev is None or t - prev >= quiet_s else None
        prev = t
    return None


def main():
    r = load()
    lines = r.describe()
    print("\n".join(lines) if lines else "还在学：每种日子（工作日 / 周末）要有 %d 天的记录。" % MIN_DAYS)
    print("最近 %d 天：工作日 %d 天，周末 %d 天有记录。" % (DAYS, r.days[0], r.days[1]))
    print("        0     6     12    18    24")
    for k, name in enumerate(("工作日", "周末  ")):
        print("%s  |%s|" % (name, r.chart(k)))


if __name__ == "__main__":
    main()
