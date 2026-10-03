#!/usr/bin/env python3
"""Reading answers aloud: python3 -m unittest discover bridge (no service, no device, no network)."""
import array
import base64
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault("SLIME_HOME", tempfile.mkdtemp())
os.environ["SLIME_LANG"] = "zh"
sys.path.insert(0, HERE)
import slime_brain as brain  # noqa: E402
import slime_buddy as buddy  # noqa: E402
import slime_tts as tts  # noqa: E402

ENV = {k: "" for k in ("SLIME_TTS_URL", "SLIME_TTS_MODEL", "SLIME_TTS_KEY", "SLIME_TTS_VOICE",
                       "SLIME_STT_URL", "SLIME_STT_KEY", "SLIME_LLM_URL", "SLIME_LLM_KEY")}


def wav(samples, rate=24000, ch=1, size=None):
    data = struct.pack("<%dh" % len(samples), *samples)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, ch, rate, rate * ch * 2, ch * 2, 16)
            + b"data" + struct.pack("<I", len(data) if size is None else size) + data)


def device(ai):
    return mock.patch.object(brain, "device_ai", return_value=ai)


class Settings(unittest.TestCase):
    def setUp(self):
        p = mock.patch.dict(os.environ, ENV)
        p.start()
        self.addCleanup(p.stop)

    def test_a_key_alone_means_mimo_with_bingtang(self):
        with device({"tts_key": "k"}):
            cfg = tts.settings()
        self.assertEqual(cfg["tts_url"][0], tts.MIMO_URL)
        self.assertEqual(cfg["tts_model"][0], "mimo-v2.5-tts")
        self.assertEqual(cfg["tts_voice"][0], "冰糖")
        self.assertEqual(cfg["tts_key"], ("k", "device"))

    def test_nothing_set_reuses_speech_to_text(self):
        with device({"stt_url": "https://api.siliconflow.cn/v1", "stt_key": "s"}):
            cfg = tts.settings()
        self.assertEqual(cfg["tts_url"], ("https://api.siliconflow.cn/v1", "stt"))
        self.assertEqual(cfg["tts_key"], ("s", "stt"))
        self.assertEqual(cfg["tts_model"][0], tts.COSY_MODEL)
        self.assertEqual(cfg["tts_voice"][0], "bella")

    def test_saved_key_never_goes_to_an_address_from_the_environment(self):
        with device({"tts_key": "k"}), mock.patch.dict(os.environ, {"SLIME_TTS_URL": "https://elsewhere/v1"}):
            cfg = tts.settings()
        self.assertEqual(cfg["tts_url"][0], "https://elsewhere/v1")
        self.assertEqual(cfg["tts_key"][0], "")


class Synthesize(unittest.TestCase):
    def setUp(self):
        p = mock.patch.dict(os.environ, ENV)
        p.start()
        self.addCleanup(p.stop)

    def call(self, ai, mood="happy"):
        sent = {}

        def post(url, body, headers, timeout):
            sent.update(url=url, body=body, headers=headers)
            if "chat/completions" in url:
                return json.dumps({"choices": [{"message": {"audio": {"data": base64.b64encode(b"RIFF").decode()}}}]}).encode()
            return b"RIFF"

        with device(ai), mock.patch.object(tts, "_post", side_effect=post):
            self.assertEqual(tts.synthesize("你好呀", mood), b"RIFF")
        return sent

    def test_mimo_preset_voice_with_the_mood_as_style(self):
        sent = self.call({"tts_key": "k"})
        self.assertEqual(sent["url"], tts.MIMO_URL + "/chat/completions")
        self.assertEqual(sent["headers"]["api-key"], "k")
        self.assertEqual(sent["body"]["audio"], {"format": "wav", "voice": "冰糖"})
        self.assertEqual(sent["body"]["messages"][-1], {"role": "assistant", "content": "你好呀"})
        self.assertEqual(sent["body"]["messages"][0]["role"], "user")  # "开心、俏皮…"

    def test_mimo_neutral_mood_sends_no_style(self):
        sent = self.call({"tts_key": "k"}, mood="neutral")
        self.assertEqual(sent["body"]["messages"], [{"role": "assistant", "content": "你好呀"}])

    def test_mimo_voice_design_describes_the_voice(self):
        sent = self.call({"tts_key": "k", "tts_model": "mimo-v2.5-tts-voicedesign", "tts_voice": "奶声奶气的小史莱姆"})
        self.assertEqual(sent["body"]["messages"][0], {"role": "user", "content": "奶声奶气的小史莱姆"})
        self.assertNotIn("voice", sent["body"]["audio"])

    def test_siliconflow_names_voices_after_the_model(self):
        sent = self.call({"tts_url": "https://api.siliconflow.cn/v1", "tts_key": "k"})
        self.assertEqual(sent["url"], "https://api.siliconflow.cn/v1/audio/speech")
        self.assertEqual(sent["body"]["voice"], tts.COSY_MODEL + ":bella")
        self.assertEqual(sent["body"]["sample_rate"], 16000)
        self.assertEqual(sent["headers"]["Authorization"], "Bearer k")


class Audio(unittest.TestCase):
    def test_stereo_is_mixed_down(self):
        pcm, rate = tts.wav_pcm(wav([100, 300, -100, -300], rate=16000, ch=2))
        self.assertEqual((list(pcm), rate), ([200, -200], 16000))

    def test_streaming_header_without_length(self):
        pcm, _ = tts.wav_pcm(wav([1, 2, 3], size=0xFFFFFFFF))
        self.assertEqual(list(pcm), [1, 2, 3])

    def test_resampled_to_16k_and_pitch_shortens_it(self):
        pcm = array.array("h", range(0, 24000))  # 1 s at 24 kHz
        same = tts.to_device(pcm, 24000, 100)
        self.assertEqual(len(same), 16000 * 2)
        out = array.array("h")
        out.frombytes(same[:6])
        self.assertEqual(list(out), [0, 1, 3])  # 1.5 source samples per output sample
        self.assertEqual(len(tts.to_device(pcm, 24000, 125)), 12800 * 2)

    def test_capped_at_what_the_pet_takes(self):
        pcm = array.array("h", bytes(2 * 16000 * 30))
        self.assertEqual(len(tts.to_device(pcm, 16000)), 16000 * tts.MAX_S * 2)


class Talk(unittest.TestCase):
    def run_talk(self, cfg, synth, play_secs=1.5):
        sent, played, slept = [], [], []
        with mock.patch.object(tts, "device_config", return_value=cfg), \
                mock.patch.object(tts, "speak_bytes", side_effect=synth), \
                mock.patch.object(tts, "play", side_effect=lambda clip: played.append(clip) or play_secs), \
                mock.patch.object(buddy, "send_line", side_effect=sent.append), \
                mock.patch.object(buddy.time, "sleep", side_effect=slept.append), \
                mock.patch.object(buddy, "log"):
            buddy.talk("happy", "今天写了好多代码呢。要不要休息一下，陪我玩一会儿？")
        return sent, played, slept

    def test_each_line_is_read_aloud_and_the_next_waits_for_it(self):
        sent, played, slept = self.run_talk({"speak": True, "speak_pitch": 120},
                                            lambda line, mood, pitch: ("%s|%d" % (line, pitch)).encode())
        self.assertEqual(len(sent), 2)
        self.assertEqual(played, [s[len("talk happy "):-1].encode() + b"|120" for s in sent])
        self.assertGreater(slept[1], 1.5)  # the second line waited for the first one's voice

    def test_off_means_text_only(self):
        sent, played, _ = self.run_talk({"speak": False}, lambda *a: self.fail("synthesized"))
        self.assertEqual((len(sent), played), (2, []))

    def test_a_failing_service_still_shows_the_answer(self):
        def boom(*a):
            raise OSError("no route")
        sent, played, _ = self.run_talk({"speak": True}, boom)
        self.assertEqual((len(sent), played), (2, []))


if __name__ == "__main__":
    unittest.main()
