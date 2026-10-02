# Prompts to rebuild this project

[中文](PROMPTS.md) · **English**

This project was built together with an AI coding agent (Claude Code). The author wrote almost no code by hand: they described what they wanted in plain language, tried each step on the device and reported back. This document turns that process into a set of prompts you can give your own agent to build a version of your own, with a different character, a different screen, or only the features you care about.

Each stage lists:
- **Done when**: how to tell the stage is finished;
- **Pitfalls**: problems we actually hit that you cannot see in the code. Telling the agent up front saves several rounds of back and forth.

## How to use it

1. Send the **master prompt** first, so the agent knows the overall goal and how to work.
2. Then send the stages **one at a time**. Verify each one on the real device before moving on. Don't send everything at once: in a hardware project most problems only show up once the code runs on the board.
3. When something is vague ("it doesn't work", "it froze"), have the agent **add logging and measure** before changing anything. The hardest problems in this project were all found that way (see "Working principles" at the end).

---

## Master prompt

```text
I have an ESP-Mosaico board (ESP32-S31, 480x480 AMOLED, touch, BMI270 IMU, ES8311 audio,
vibration motor, fuel gauge), plus the optional Interaction module (buttons, PIR, light
sensor, 6 RGB LEDs) and camera module. The official BSP is at
https://github.com/esp-mosaico/esp-mosaico-bsp and needs ESP-IDF 6.2 or newer (master).

I want a desktop pet slime living on the screen: a retro RPG look, soft and jelly-like, with
lots of expressions and states, and linked to the Claude Code sessions I'm running (different
looks while Claude thinks, works, and waits for my approval).

How to work:
- After every step, build, flash and verify on the real device, then tell me what you need
  from me (look at the screen, press a button, plug a module in or out).
- When a problem is unclear, add logging or a recording endpoint and measure before changing
  code; never tune thresholds by feel.
- Keep each change small; ask before committing.
- Music and art must be original; don't copy any game's melodies or assets.
- Talk to me in English, and use English for the on-device text.
```

---

## Stage 1: rendering core and host preview

```text
Don't touch the hardware yet. Write a rendering core in plain C with no ESP-IDF dependency:
- a small rasterizer (anti-aliased fills, gradients, strokes, alpha blending) drawing into an
  RGB565 framebuffer;
- a slime state machine: idle, greet, poked (left/right), dizzy, sleep, think, wait, level up,
  hurt, charging, melting, metal, working; each with its own face, motion and small effects
  (hearts, stars, sweat drops, Zzz);
- jelly spring physics: squash and stretch, sway, bounce on landing, smooth transitions;
- a retro RPG style status window (name, Lv, HP bar) and dialog box (typewriter effect).
Also write a host program that uses the same code to render a sheet of every state and single
frames, so the look can be checked without flashing.
```

**Done when**: every state is recognizable at a glance on the sheet; on the host, one 480×480 frame renders in well under 40 ms.

**Pitfalls**:
- Make a browser draft first (HTML canvas) to tune shapes and animation parameters, then port to C. That is much faster than tuning in C.
- Redraw only what changed (dirty rectangles); a microcontroller won't reach a smooth frame rate otherwise.

## Stage 2: running on the board

```text
Port the core to the device: framebuffer in PSRAM, dirty rectangles pushed to the screen over
QSPI DMA, rendering split across both cores. Print a stats line (frame rate, memory) every few
seconds. The flash script must reboot the running firmware into download mode by itself (no
buttons), and back up the whole factory flash before the first write.
```

**Done when**: 25 fps or more on the device; it boots on battery with USB unplugged; the free internal RAM in the stats line stays steady.

**Pitfalls**:
- **Won't boot on battery**: the board's GPIO57 (PWR_SW) low means power off. The factory bootloader raises it at once; the standard IDF bootloader does not. Raise it from a `bootloader_components` hook at the very start. After adding a bootloader component, delete `build/bootloader`, or it won't be linked in.
- **USB serial**: TinyUSB CDC runs at high speed; the receive callback must drain everything each time, or reception stops for good and even the download-mode magic string is lost. On macOS a freshly opened serial port echoes, which feeds the device's own log back into it: put the port in raw mode on the computer, and `tcdrain` before closing.
- **Internal RAM is the bottleneck**: by default the BSP bounces screen DMA through internal RAM. Once Wi-Fi and the camera are in, fragmentation breaks screen transfers. Patch the BSP to enable `psram_dma_direct`, turn off Wi-Fi's IRAM optimizations and shrink Wi-Fi's receive buffers.
- Don't casually use a "back up only, don't flash" mode: it leaves the chip in download mode with a black screen.

## Stage 3: Claude Code

```text
On the computer, write a Claude Code hook script (Python standard library only) that turns
session events (start, prompt, tool call, tool success/failure, permission request, stop)
into one line of a simple text protocol and sends it to the device. The device tracks several
sessions, decides what the slime shows, and shows what Claude is doing in the dialog (tool,
file name, the gist of a command). Finishing tasks earns EXP and levels up, persisted.
The hook must be asynchronous, print nothing, always exit 0, and fail silently when the device
is offline.
```

**Done when**: during real Claude Code work, the slime's state and dialog keep up in real time, even with two sessions running.

**Pitfalls**:
- Standard output from `UserPromptSubmit` and `SessionStart` hooks is injected into Claude's context, so the hook must never print.
- Events can arrive out of order: put a timestamp on every line, and have the device drop duplicates and order by timestamp.

## Stage 4: Wi-Fi and web panel

```text
Add Wi-Fi (mDNS name slime.local) and a web panel served by the device: live status, every
setting (applied and saved at once), manual state switching to preview. The hook prefers
Wi-Fi with USB as the fallback. I type the Wi-Fi password myself in a terminal and it goes
only over USB; no interface may ever read it back.
```

**Pitfalls**:
- On macOS, resolving `.local` asks for both IPv4 and IPv6 and waits about 5 s for the missing IPv6 answer. Resolve IPv4 only, and cache the device's IP.
- ESP-IDF 6 dropped the bundled cJSON; use `espressif/cjson` from the component registry.

## Stage 5: sensors, sound, settings menu

```text
- IMU: tilt it and the slime slides, shake it and it gets dizzy, lay it face down and it sleeps.
- Microphone: clap twice to greet, bob to the beat of music; ignore keyboard typing.
- Sound: different chiptune cues for different events, with an off switch.
- An on-device settings menu: hold the screen to open, pages, touch controls.
```

**Pitfalls**:
- The device rests at an angle: measure tilt against a slowly following baseline.
- Claps and typing are both short sharp sounds; the difference is that a clap has silence around it, typing is a train of clicks.
- If page flips don't redraw, check whether a tap marks the menu dirty.

## Stage 6: camera

```text
Add the camera module: face detection on the device (no picture leaves it), eyes that follow
you, peekaboo when the lens is covered, warnings when you sit too close or too long. Add a
camera view mode and a corner thumbnail so I can check that I'm in frame. The camera must
support hot-plugging after boot.
```

**Pitfalls**:
- One of the camera's data lines shares a pin with the on-board USB-JTAG; disable USB-JTAG.
- The lens is narrow: at a desk, a face fills 60–70 % of the frame width. Features based on moving around in the picture (waving, tracking) work poorly; features based on the face itself work better.
- Hot-plugging: releasing the driver can fail when the camera is pulled; keep the handle and retry the delete later, or the slot stays claimed forever.
- Don't merge the thumbnail with the slime's dirty area into one big rectangle, and don't refresh it every frame; both cost a lot of frame rate.

## Stage 7: face interaction (tilt, nod, shake, eye contact)

```text
Use the face for interaction: tilt your head and it tilts, nod and it's happy, shake your head
and it sulks, stare at it and it gets shy. First add a recording endpoint that stores every
detection's raw values, filtered values, thresholds and whether it fired; I'll do a fixed
routine (still, three nods, still, three shakes) and you pick the algorithm and thresholds
from the data.
```

**Pitfalls** (this stage took the most rework):
- The face model's 5 keypoints **barely move when the head turns** (on a small crop they drift toward an "average face"); judging nods and shakes from them mostly reacts to noise.
- Requiring a detected face on every frame doesn't work either: detection comes and goes as soon as the head moves.
- What finally worked: use face detection only to locate the face; on every frame, take row and column brightness projections of that region and measure the head's movement from the shift between consecutive frames.
- A nod is **one** dip and back; a shake is **two** back-and-forths (left-right-left). Detect them separately.
- Give the thresholds both a floor and a ceiling, with the floor above the wobble that follows a big movement.
- Fast-flashing LEDs next to the board change the light on your face and read as motion.

## Stage 8: music and sound effects

```text
Build a three-voice chiptune synthesizer (square, triangle, noise) with scores written as MML
text, including pitch glides and ties. Compose an original boot tune, cues, and two background
tunes it hums now and then while idle (with an off switch). Then port the public-domain sfxr
engine, so the web panel can generate sounds at random, preview them and swap any event's sound.
```

**Pitfalls**:
- A long note written as two notes of the same pitch re-attacks and sounds like an extra hit. The score format needs ties.
- sfxr is designed for 44.1 kHz; if the device outputs 48 kHz, resample, or pitch and length come out wrong.
- The synthesizer compiles on the host too: render every sound there and check lengths and peaks. It's much faster than listening on the board.

## Stage 9: everyday features and a demo

```text
Add a clock (network time, time-of-day greetings), quiet hours (mute, LEDs off, dimmer screen),
a focus timer, a help page, and a two-minute feature demo with captions and a 3-second
countdown, for recording videos; the demo must not change the real level or EXP.
```

**Pitfalls**:
- Mute at the audio layer, in one place, not with a check at every call site; one will always be missed.

## Stage 10: robustness

```text
Make freezes recover by themselves and leave a record: reboot on task watchdog timeout; put
the main loop and the audio task under the watchdog; add a crash-record partition, read the
last crash's task and addresses at boot, and show them in the status API.
```

**Pitfalls**:
- The default task watchdog only warns; after a freeze you have to pull the power.
- When the main loop hangs by waiting forever, the CPU isn't busy and the watchdog never fires. Registering the main loop itself with the watchdog catches that.

## Stage 11: wireless updates

```text
Add wireless firmware updates: two app slots written in turn; a new image confirms itself
after 30 s of running, and a crash before that rolls back to the old one. Updates need a token
that is only available over USB. When changing the partition table, keep the settings area
where it is, and back it up before flashing.
```

**Pitfalls**:
- After changing the partition table, write the new OTA data partition as blank; leftover bytes there can make the device boot into an empty slot.
- Don't make the new image's health check depend on Wi-Fi, the camera or anything external, or it may never confirm, and the next reset quietly rolls it back.

## Stage 12: buttons and AI comments

```text
Use the board's AI and BOOT keys and the Interaction module's keys (mute, focus timer,
settings, volume). Then, after each Claude turn, have a local model on my computer (LM Studio)
write a one-line comment in the slime's voice. The transcript may only go to that local model,
never to the cloud.
```

**Pitfalls**:
- Hooks have a timeout and must not slow Claude down: run the model call in a background process detached from the hook.
- The device can only draw characters that were pre-rendered into its font. If the UI language needs more than ASCII (Chinese, Japanese…), either add a table of common characters, or have the computer read the device's glyph tables and filter out what can't be drawn. This project does both.

---

## Working principles

What worked best in this project:

1. **Measure before changing**: nod detection went through several versions. The first ones tuned thresholds by feel; only after adding a recording endpoint did it become clear that first "the keypoints don't follow the head at all", and later "a nod has only one reversal". Without data, tuning only makes things worse.
2. **Reproduce on the computer**: rendering, the synthesizer and the nod algorithm all run off the hardware; replaying recorded real data shows in seconds whether a set of parameters works.
3. **Compare like with like**: if two performance measurements happen in different states (one during the charging animation, one idle), the numbers mislead.
4. **Let it leave evidence**: rare freezes are hard to catch live; add crash records first, and the next one comes with clues.
5. **Build once in a fresh directory**: before publishing, a clean build exposes settings that only exist in your local configuration.
