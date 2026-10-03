#!/usr/bin/env python3
"""The slime's speaking voice: text to speech on the computer, played by the pet. Stdlib only.

slime_buddy.py reads voice-chat answers aloud with it: each dialog line is synthesized here,
converted to 16 kHz mono PCM (raised or lowered by the pet's "voice pitch" setting, which also
speeds it up or slows it down, like a tape) and POSTed to the pet's /api/speak.

Endpoints, set in the pet's web panel ("AI settings") or by environment variables:
    SLIME_TTS_URL    blank: Xiaomi MiMo (https://api.xiaomimimo.com/v1) when a TTS key is set,
                     else the speech-to-text address and key (e.g. SiliconFlow)
    SLIME_TTS_MODEL  blank: mimo-v2.5-tts on MiMo, FunAudioLLM/CosyVoice2-0.5B on SiliconFlow,
                     tts-1 elsewhere
    SLIME_TTS_KEY
    SLIME_TTS_VOICE  blank: 冰糖 on MiMo, bella on SiliconFlow, alloy elsewhere; with a MiMo
                     voice-design model (…-voicedesign) this is a description of the voice
Two kinds of API: MiMo's (/chat/completions with an "audio" field, the text as the assistant's
message) and the OpenAI-style /audio/speech that SiliconFlow and most others offer.

Try it:
    python3 slime_tts.py "你好呀，我是史莱姆"          # say it on the pet
    python3 slime_tts.py "你好呀" --save hello.wav     # or keep it as a file
    python3 slime_tts.py config                      # what is in use
"""
import array
import base64
import json
import os
import struct
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slime_brain as brain  # noqa: E402

RATE = 16000  # what the pet plays (AUDIO_SPEAK_RATE)
MAX_S = 20  # longest clip the pet takes (AUDIO_SPEAK_MAX_S)
MIMO_URL = "https://api.xiaomimimo.com/v1"
MIMO_MODEL = "mimo-v2.5-tts"
COSY_MODEL = "FunAudioLLM/CosyVoice2-0.5B"
# MiMo takes a style request as the user's message; the answer's mood picks one
MIMO_STYLE = {"happy": "开心、俏皮，语速轻快", "proud": "得意、神气", "worried": "有点委屈、小声",
              "neutral": ""}


def is_mimo(url):
    return "xiaomimimo" in url


def settings():
    """{name: (value, source)} for tts_url, tts_model, tts_key, tts_voice; same sources as
    brain.settings() plus "stt" (reusing the speech-to-text service)."""
    dev, base = brain.device_ai(), brain.settings()

    def pick(name, env):
        if os.environ.get(env):
            return os.environ[env], "env"
        v = dev.get(name)
        return (v, "device") if isinstance(v, str) and v else ("", "default")

    url, key = pick("tts_url", "SLIME_TTS_URL"), pick("tts_key", "SLIME_TTS_KEY")
    if key[1] == "device" and url[1] == "env":
        key = ("", "default")  # the saved key belongs to the saved address
    if not url[0]:
        if key[0]:
            url = (MIMO_URL, "default")
        else:  # nothing set up for speech: try the speech-to-text service, which often has it too
            url, key = (base["stt_url"][0], "stt"), (base["stt_key"][0], "stt")
    url = (url[0].rstrip("/"), url[1])
    model, voice = pick("tts_model", "SLIME_TTS_MODEL"), pick("tts_voice", "SLIME_TTS_VOICE")
    if not model[0]:
        model = (MIMO_MODEL if is_mimo(url[0]) else COSY_MODEL if "siliconflow" in url[0] else "tts-1", "default")
    if not voice[0]:
        voice = ("冰糖" if is_mimo(url[0]) else "bella" if "siliconflow" in url[0] else "alloy", "default")
    return {"tts_url": url, "tts_model": model, "tts_key": key, "tts_voice": voice}


def describe_settings():
    names = {"env": "environment", "device": "web panel", "default": "default", "stt": "same as speech to text"}
    out = []
    for k, (v, src) in settings().items():
        out.append("%-9s %s  [%s]" % (k, ("set" if v else "not set") if k == "tts_key" else v, names[src]))
    return out


def _post(url, body, headers, timeout):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def synthesize(text, mood="", timeout=30):
    """Audio file bytes (WAV) for text, from the configured service. Raises on any failure."""
    cfg = settings()
    url, model, key, voice = (cfg[k][0] for k in ("tts_url", "tts_model", "tts_key", "tts_voice"))
    if is_mimo(url):
        headers = brain._headers(key)
        if key:
            headers["api-key"] = key  # MiMo's own header; it takes the Bearer one too
        messages = [{"role": "assistant", "content": text}]
        design = model.endswith("voicedesign")
        hint = voice if design else MIMO_STYLE.get(mood, "")  # a voice-design model is told the voice instead
        if hint:
            messages.insert(0, {"role": "user", "content": hint})
        audio = {"format": "wav"}
        if not design:
            audio["voice"] = voice
        got = json.loads(_post(url + "/chat/completions", {"model": model, "messages": messages, "audio": audio},
                               headers, timeout))
        return base64.b64decode(got["choices"][0]["message"]["audio"]["data"])
    body = {"model": model, "input": text, "voice": voice, "response_format": "wav"}
    if "siliconflow" in url:
        if ":" not in voice:
            body["voice"] = model + ":" + voice  # its system voices are named after the model
        body["sample_rate"] = RATE
    return _post(url + "/audio/speech", body, brain._headers(key), timeout)


def wav_pcm(data):
    """(samples as array('h'), rate) of a 16-bit PCM WAV, mixed down to mono. Tolerates the
    "unknown length" header (0xFFFFFFFF) that streaming servers write."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt = 12, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, pos + 8)
        elif cid == b"data":
            if not fmt:
                break
            tag, ch, rate, _, _, bits = fmt
            if tag not in (1, 0xFFFE) or bits != 16:
                raise ValueError("WAV is not 16-bit PCM")
            raw = data[pos + 8:] if size in (0, 0xFFFFFFFF) else data[pos + 8:pos + 8 + size]
            pcm = array.array("h")
            pcm.frombytes(raw[:len(raw) // 2 * 2])
            if sys.byteorder == "big":
                pcm.byteswap()
            if ch > 1:
                pcm = array.array("h", (sum(pcm[i:i + ch]) // ch for i in range(0, len(pcm) - ch + 1, ch)))
            return pcm, rate
        pos += 8 + size + (size & 1)
    raise ValueError("WAV without audio")


def to_device(pcm, rate, pitch=100):
    """16 kHz mono s16le bytes for /api/speak. pitch (percent) above 100 plays it higher and
    faster, like a sped-up tape; at most MAX_S seconds are kept."""
    step = rate * pitch / 100.0 / RATE  # source samples per output sample
    n = min(int(len(pcm) / step) if step else 0, RATE * MAX_S)
    last = len(pcm) - 1
    out = array.array("h", bytes(2 * n))
    for i in range(n):
        x = i * step
        j = int(x)
        a = pcm[j]
        out[i] = a if j >= last else int(a + (pcm[j + 1] - a) * (x - j))
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def device_config():
    """The pet's settings (speak on/off, pitch); {} when it cannot be reached."""
    try:
        return json.loads(brain.device_get("/api/config", timeout=2))
    except (OSError, ValueError):
        return {}


def speak_bytes(text, mood="", pitch=100):
    pcm, rate = wav_pcm(synthesize(text, mood))
    return to_device(pcm, rate, pitch)


def play(pcm16k, timeout=6):
    """Send to the pet. Returns the clip's length in seconds, or None when the pet would not play
    it (reading aloud is off, or night mode)."""
    ip = brain.device_ip()
    if not ip:
        raise OSError("device not found")
    req = urllib.request.Request("http://%s/api/speak" % ip, data=pcm16k,
                                 headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            r.read()
    except urllib.error.HTTPError as e:
        if e.code == 409:
            return None
        raise
    return len(pcm16k) / 2 / RATE


def _save_wav(path, pcm16k):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm16k)) + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm16k)))
        f.write(pcm16k)


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        sys.exit(__doc__)
    if args[0] == "config":
        print("\n".join(describe_settings()))
        return
    save = None
    if "--save" in args:
        i = args.index("--save")
        save = args[i + 1] if i + 1 < len(args) else "speech.wav"
        del args[i:i + 2]
    pitch = device_config().get("speak_pitch", 100) if not save else int(os.environ.get("SLIME_TTS_PITCH", "110"))
    pcm = speak_bytes(" ".join(args), "happy", pitch)
    if save:
        _save_wav(save, pcm)
        print("saved %s (%.1f s, pitch %d%%)" % (save, len(pcm) / 2 / RATE, pitch))
    else:
        secs = play(pcm)
        print("said it (%.1f s)" % secs if secs is not None else "the pet would not play it: reading aloud is off, or night mode")


if __name__ == "__main__":
    main()
