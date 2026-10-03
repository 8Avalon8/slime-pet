#!/usr/bin/env python3
"""What the slime knows when it chats: python3 -m unittest discover bridge (no model, no device, no network)."""
import os
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["SLIME_HOME"] = tempfile.mkdtemp()
os.environ["SLIME_LANG"] = "zh"
os.environ["SLIME_WEATHER"] = "1"
sys.path.insert(0, HERE)
import slime_brain as brain  # noqa: E402
import slime_buddy as buddy  # noqa: E402

NOW = time.mktime((2026, 10, 3, 19, 47, 0, 0, 0, -1))  # a Saturday evening

STATUS = {"lv": 7, "exp": 30, "need": 120, "bat": {"ok": True, "soc": 64, "chg": False},
          "focus": 900, "rest": 0, "night": False, "cam": {"on": True, "ok": True, "face": True}}

WTTR = {
    "current_condition": [{"temp_C": "21", "FeelsLikeC": "20", "humidity": "70",
                           "weatherDesc": [{"value": "Partly cloudy"}], "lang_zh": [{"value": "局部多云"}]}],
    "nearest_area": [{"areaName": [{"value": "Shanghai"}]}],
    "weather": [
        {"mintempC": "18", "maxtempC": "24", "hourly": [{"chanceofrain": "10", "lang_zh": [{"value": "晴"}]}] * 8},
        {"mintempC": "17", "maxtempC": "22", "hourly": [{"chanceofrain": "80", "weatherDesc": [{"value": "Light rain"}]}] * 8},
    ],
}


class Context(unittest.TestCase):
    def setUp(self):
        buddy._weather = (0, None)
        os.makedirs(brain.DIARY_DIR, exist_ok=True)
        with open(os.path.join(brain.DIARY_DIR, "2026-10-02.md"), "w", encoding="utf-8") as f:
            f.write("# 2026-10-02 史莱姆日记\n\n今天主人修好了语音唤醒，我好开心。\n\n---\n09:00 到 18:00\n")
        brain.journal_append("heard", "勇者斗恶龙好玩吗", t=NOW - 3 * 3600)

    def test_clock_has_date_and_weekday(self):
        self.assertEqual(buddy.clock_text(NOW), "2026年10月3日 星期六（周末） 晚上 19:47")

    def test_context_knows_device_memory_and_time(self):
        _, ctx = buddy.context(NOW, STATUS)
        for want in ("2026年10月3日", "星期六", "7 级", "电量 64%", "还剩 15 分钟", "主人在电脑前",
                     "修好了语音唤醒", "勇者斗恶龙好玩吗"):
            self.assertIn(want, ctx)
        self.assertNotIn("天气", ctx)  # only fetched when asked

    def test_weather_only_when_asked(self):
        with mock.patch.object(buddy, "weather", return_value="晴") as w:
            buddy.context(NOW, None, "今天几号")
            w.assert_not_called()
            _, ctx = buddy.context(NOW, None, "明天会下雨吗")
            w.assert_called_once()
        self.assertIn("天气：晴", ctx)

    def test_weather_unavailable_says_so(self):
        with mock.patch.object(buddy, "weather", return_value=None):
            _, ctx = buddy.context(NOW, None, "外面天气怎么样")
        self.assertIn("别编天气", ctx)

    def test_weather_text(self):
        got = buddy.weather_text(WTTR)
        self.assertIn("Shanghai：现在局部多云，21°C", got)
        self.assertIn("明天Light rain，17~22°C，降雨概率最高 80%", got)

    def test_answer_prompt(self):
        seen = {}

        def fake_speak(system, user, **kw):
            seen.update(system=system, user=user, **kw)
            return ("happy", "是一款经典日式角色扮演游戏！")

        with mock.patch.object(brain, "speak", fake_speak):
            got = buddy.answer("勇者斗恶龙是什么", now=NOW, st=STATUS)
        self.assertEqual(got[0], "happy")
        self.assertIn("直接回答问题本身", seen["system"])
        self.assertIn("别夸主人", seen["system"])
        self.assertIn("星期六", seen["system"])
        self.assertTrue(seen["user"].endswith("主人：勇者斗恶龙是什么"))
        self.assertLess(seen["temperature"], 0.9)


if __name__ == "__main__":
    unittest.main()
