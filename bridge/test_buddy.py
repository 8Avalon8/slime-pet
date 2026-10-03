#!/usr/bin/env python3
"""What the slime knows when it chats: python3 -m unittest discover bridge (no model, no device, no network)."""
import json
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
import slime_agent as agent  # noqa: E402
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
        got = agent.weather_text(WTTR)
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



def tool_call(cid, name, args):
    return {"id": cid, "type": "function", "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)}}


class Tools(unittest.TestCase):
    def setUp(self):
        for f in (agent.FACTS_FILE, agent.REMINDERS_FILE):
            if os.path.exists(f):
                os.remove(f)
        agent._no_tools.clear()

    def test_loop_runs_tools_in_parallel_then_answers(self):
        replies = [
            {"role": "assistant", "content": "", "tool_calls": [
                tool_call("a", "weather", {"city": "Tokyo"}), tool_call("b", "remember", {"fact": "主人不吃香菜"})]},
            {"role": "assistant", "content": '{"text":"东京明天下雨，带伞哦","mood":"neutral"}'},
        ]
        offered = []

        def fake(messages, max_tokens, temperature, timeout, tools=None):
            offered.append(bool(tools))
            return replies.pop(0)

        msgs = [{"role": "user", "content": "东京明天天气"}]
        with mock.patch.object(brain, "chat_message", fake), \
                mock.patch.object(agent, "fetch_weather", return_value="东京：明天小雨"):
            out = agent.run(msgs)
        self.assertIn("带伞", out)
        self.assertEqual(offered, [True, True])
        tool_msgs = [m for m in msgs if m["role"] == "tool"]
        self.assertEqual([m["tool_call_id"] for m in tool_msgs], ["a", "b"])
        self.assertEqual(tool_msgs[0]["content"], "东京：明天小雨")
        self.assertEqual(agent.facts()[-1]["d"], "主人不吃香菜")
        _, ctx = buddy.context(NOW)
        self.assertIn("主人让你记住的事：主人不吃香菜", ctx)

    def test_rounds_are_capped(self):
        calls = []

        def fake(messages, max_tokens, temperature, timeout, tools=None):
            calls.append(bool(tools))
            if tools:
                return {"role": "assistant", "content": "", "tool_calls": [tool_call("x", "list_reminders", {})]}
            return {"role": "assistant", "content": "好"}

        with mock.patch.object(brain, "chat_message", fake):
            self.assertEqual(agent.run([{"role": "user", "content": "hi"}]), "好")
        self.assertEqual(calls, [True] * agent.MAX_ROUNDS + [False])

    def test_endpoint_without_tools_falls_back(self):
        calls = []

        def fake(messages, max_tokens, temperature, timeout, tools=None):
            calls.append(bool(tools))
            if tools:
                raise agent.urllib.error.HTTPError("u", 400, "no tools", {}, None)
            return {"role": "assistant", "content": "好"}

        with mock.patch.object(brain, "chat_message", fake):
            self.assertEqual(agent.run([{"role": "user", "content": "hi"}]), "好")
            self.assertEqual(agent.run([{"role": "user", "content": "hi"}]), "好")
        self.assertEqual(calls, [True, False, False])  # the refusal is remembered

    def test_reminders(self):
        self.assertIn("分钟后", agent.call("set_reminder", {"minutes": 5, "text": "喝水"}))
        self.assertIn("只能", agent.call("set_reminder", {"minutes": 0, "text": "x"}))
        self.assertEqual(agent.due_reminders(), [])
        due = agent.due_reminders(time.time() + 301)
        self.assertEqual([r["d"] for r in due], ["喝水"])
        self.assertEqual(agent.reminders(), [])

    def test_memory_tools(self):
        brain.journal_append("heard", "我想去京都旅游", t=time.time() - 86400)
        self.assertIn("京都", agent.call("search_memory", {"keyword": "京都"}))
        self.assertIn("没有", agent.call("search_memory", {"keyword": "火星"}))
        self.assertIn("没有日记", agent.call("read_diary", {"date": "2020-01-01"}))
        self.assertIn("参数不对", agent.call("read_diary", {}))
        self.assertIn("出错了", agent.call("read_diary", {"date": "不是日期"}))

    def test_device_tool(self):
        with mock.patch.object(brain, "device_cmd") as cmd, mock.patch.object(agent, "_post_config") as cfg:
            agent.call("device", {"action": "focus_start", "value": 40})
            cfg.assert_called_with({"focus_min": 40})
            cmd.assert_called_with("focus start\n", timeout=3)
            agent.call("device", {"action": "volume", "value": 150})
            cfg.assert_called_with({"volume": 100, "sound": True})

    def test_definitions_are_valid(self):
        for d in agent.definitions():
            f = d["function"]
            self.assertTrue(set(f["parameters"]["required"]) <= set(f["parameters"]["properties"]), f["name"])


if __name__ == "__main__":
    unittest.main()
