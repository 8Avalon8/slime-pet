#!/usr/bin/env python3
"""The owner's usual hours and the moments the buddy picks: python3 -m unittest discover bridge."""
import os
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault("SLIME_HOME", tempfile.mkdtemp())
os.environ["SLIME_LANG"] = "zh"
sys.path.insert(0, HERE)
import slime_buddy as buddy  # noqa: E402
import slime_rhythm as rhythm  # noqa: E402


def at(y, m, d, hh, mm=0):
    return time.mktime((y, m, d, hh, mm, 0, 0, 0, -1))


MONDAY = (2026, 10, 5)


def workdays(n, before=MONDAY, start=9, end=18):
    """Prompts every half hour from start to end on the n workdays before `before`."""
    out, t = [], at(*before, 12)
    while len({e["day"] for e in out}) < n:
        t -= 86400
        if time.localtime(t).tm_wday >= 5:
            continue
        lt = time.localtime(t)
        for h in range(start, end):
            for mm in (0, 30):
                out.append({"t": at(lt.tm_year, lt.tm_mon, lt.tm_mday, h, mm), "ev": "prompt", "sid": "s1",
                            "day": lt.tm_yday})
    return sorted(out, key=lambda e: e["t"])


class Learn(unittest.TestCase):
    def test_usual_workday_hours(self):
        r = rhythm.learn(workdays(10), at(*MONDAY, 12))
        self.assertTrue(r.mature(0))
        self.assertFalse(r.mature(1))  # no weekends seen
        self.assertEqual(r.usual(0), (9 * 4, 17 * 4 + 2))  # 09:00 .. the 17:30 quarter
        self.assertEqual(r.describe(), ["工作日通常 09:00 到 17:45 在电脑前"])
        self.assertGreater(r.at(at(*MONDAY, 10)), 0.9)
        self.assertEqual(r.at(at(*MONDAY, 22)), 0)
        self.assertIsNone(r.at(at(2026, 10, 4, 10)))  # a Sunday: does not know weekends yet

    def test_still_learning(self):
        r = rhythm.learn(workdays(3), at(*MONDAY, 12))
        self.assertIsNone(r.usual(0))
        self.assertEqual(r.describe(), [])

    def test_recent_days_count_more(self):
        old = workdays(10, before=(2026, 9, 21), start=9, end=18)
        new = workdays(5, start=13, end=22)
        r = rhythm.learn(old + new, at(*MONDAY, 12))
        self.assertGreater(r.at(at(*MONDAY, 21)), r.at(at(*MONDAY, 9)))

    def test_gaps_filled_only_when_short(self):
        d = (2026, 10, 2)
        times = [at(*d, 9), at(*d, 9, 40), at(*d, 14)]
        self.assertEqual(rhythm.day_slots(times), {36, 37, 38, 56})

    def test_first_today(self):
        now = at(*MONDAY, 9, 5)
        ev = [{"t": at(2026, 10, 4, 23), "ev": "prompt"}, {"t": at(*MONDAY, 9), "ev": "prompt"},
              {"t": at(*MONDAY, 9, 2), "ev": "tool"}]
        self.assertEqual(rhythm.first_today(ev, now), at(*MONDAY, 9))
        owl = [{"t": at(2026, 10, 4, 23, 50), "ev": "prompt"}, {"t": at(*MONDAY, 0, 30), "ev": "prompt"}]
        self.assertIsNone(rhythm.first_today(owl, at(*MONDAY, 0, 35)))  # still going after midnight
        self.assertIsNone(rhythm.first_today([], now))


class Moments(unittest.TestCase):
    def rules(self, events, now):
        r = buddy.Rules()
        r.rhythm, r.rhythm_at = rhythm.learn(events, now), now
        return r

    def check(self, r, events, now):
        orig = buddy.brain.journal_read
        buddy.brain.journal_read = lambda days=7, n=None: [e for e in events if e["t"] <= now]
        try:
            return r.check({"night": False}, now)
        finally:
            buddy.brain.journal_read = orig

    def test_hello_when_you_first_show_up(self):
        past = workdays(10)
        now = at(*MONDAY, 9, 3)
        today = [{"t": at(*MONDAY, 9), "ev": "prompt", "sid": "s2"}]
        hit = self.check(self.rules(past, now), past + today, now)
        self.assertEqual(hit[1], "hello")
        self.assertEqual(hit[0], "hello:2026-10-05")

    def test_later_than_usual(self):
        past = workdays(10)
        now = at(*MONDAY, 21, 30)
        today = [{"t": at(*MONDAY, h), "ev": "prompt", "sid": "s2"} for h in (9, 12, 15, 18)]
        today += [{"t": now - 120, "ev": "prompt", "sid": "s2"}, {"t": now - 60, "ev": "stop", "sid": "s2"}]
        r = self.rules(past, now)
        r.fired["hello:2026-10-05"] = now
        hit = self.check(r, past + today, now)
        self.assertEqual(hit[1], "late")
        self.assertIn("17:45", hit[2])

    def test_not_late_on_your_usual_evening(self):
        past = workdays(10, start=14, end=23)
        now = at(*MONDAY, 21, 30)
        today = [{"t": now - 120, "ev": "prompt", "sid": "s2"}, {"t": now - 60, "ev": "stop", "sid": "s2"}]
        r = self.rules(past, now)
        r.fired["hello:2026-10-05"] = now
        self.assertIsNone(self.check(r, past + today, now))

    def test_milestone_waits_for_a_pause(self):
        now = at(*MONDAY, 15)
        stops = [{"t": now - 3000 + i * 60, "ev": "stop", "sid": "s1"} for i in range(10)]
        busy = stops + [{"t": now - 30, "ev": "tool", "sid": "s2", "d": "make"}]
        r = self.rules([], now)
        r.fired["hello:2026-10-05"] = now
        self.assertIsNone(self.check(r, busy, now))
        busy.append({"t": now - 5, "ev": "stop", "sid": "s2"})  # that session is done now
        hit = self.check(r, busy, now)
        self.assertEqual(hit[1], "milestone")
        self.assertEqual(hit[0], "milestone:2026-10-05:10")


if __name__ == "__main__":
    unittest.main()
