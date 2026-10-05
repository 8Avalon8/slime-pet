# Slime Pet · 桌面史莱姆

**中文** · [English](README.en.md)

一只住在 [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-bsp) 开发板（ESP32-S31，480×480 圆角 AMOLED）上的桌面宠物史莱姆。它会和 Claude Code 联动：Claude 思考时它也在思考，干活时显示正在做什么，需要你批准时会叫你、闪灯、震动。它还能看见你、听见你、感觉到你在摇它。

*A desktop slime pet for the ESP-Mosaico board (ESP32-S31, 480×480 AMOLED). It mirrors your Claude Code sessions (thinking, working, waiting for your approval), reacts to touch, tilt, claps, your face and head gestures, plays its own chiptunes, and updates over Wi-Fi. Firmware in C on ESP-IDF; everything runs on the device.*

![史莱姆的各种状态](docs/images/states.png)

| 屏幕界面：设置菜单 / 等你批准 / 命令出错受伤 / 完成任务 | Claude 派出子代理时，身边蹦出小分身 |
|:---:|:---:|
| <img src="docs/images/screens.png" alt="屏幕界面" width="400"> | <img src="docs/images/helper-slimes.png" alt="子代理小分身" width="600"> |

> 想用 AI 编程助手做一个自己的版本？看 [docs/PROMPTS.md](docs/PROMPTS.md)：按阶段整理的复刻提示词，附带每一步的验收标准和我们踩过的坑。

## 能做什么

| | |
|---|---|
| **Claude Code 联动** | 思考 / 干活（显示工具和命令）/ 等你批准（叫你、闪灯、震动）/ 完成任务攒经验升级 / 命令出错受伤。Claude 派出子代理时，身边会蹦出对应数量的小分身，子代理结束就跳回来。多个会话同时跑也分得清。走 Wi-Fi，USB 兜底 |
| **AI 点评** | Claude 每做完一轮工作，用你电脑上的本地模型（LM Studio）以史莱姆的口吻点评一句，比如"测试全过了，真厉害！"。默认只发给本机的模型，不上传云端；也可以接任意 OpenAI 兼容的云端 API |
| **记忆与性格** | 电脑端把每个事件记进史莱姆的日记本；按最近 7 天的习惯养成性格（夜猫子、自信、爱操心、劳模……），点评的语气跟着变；每天自动写一篇史莱姆日记 |
| **主动陪伴** | 电脑上运行 `slime_buddy.py` 后，它会在 Claude 等你批准太久、命令连续失败、深夜还在干活、连续工作几小时、今天完成很多任务、你离开后回来时主动说一句。它还会从最近几周的记录里学你的作息：每天你第一次坐到电脑前时打个招呼，比你平时收工晚了一个多小时会轻轻提醒；庆祝和劝休息这类不急的话，等 Claude 告一段落再说 |
| **等你时说一声** | `slime_buddy.py` 运行时，Claude 要你批准或回答问题（20 秒后）、做完一轮等你下一句（30 秒后），史莱姆会念一句“Claude 做完啦，等你看看”。每次等待只说一次；开着摄像头时你不在座位它先憋着，回来再说；夜间勿扰时做完的不提醒，要批准的只显示不出声 |
| **语音对话** | 喊一声"小龙小龙"，或者长按 AI 键，然后说话；电脑把录音转成文字，史莱姆结合记忆回答你，比如"Claude 刚才在干嘛？"。唤醒词在设备上识别，平时的声音不出设备 |
| **触摸与按键** | 点一下戳它，长按 1 秒打开设置菜单；AI 键单击静音、长按说话（关掉语音时长按看玩法说明）；BOOT 键单击开始/结束专注、长按打开设置；交互模块左右键调音量 |
| **动作感应**（板载 IMU） | 歪过来会滑，使劲摇会晕，扣过来放就睡觉 |
| **麦克风** | 拍两下手打招呼，跟着音乐节拍晃（会排除打字声） |
| **摄像头**（可选模块） | 眼睛跟着你、歪头它也歪、点头开心、摇头委屈、盯着它会害羞、盖住镜头躲猫猫、坐太久提醒你起来，右上角小窗看它看到的画面。也可以改成认手势：点赞夸它、张开手掌静音、OK 开始专注、打电话手势直接说话（人脸检测和手势识别是两个开关，建议只开一个）。全部在设备上算，画面不出设备 |
| **声音** | 三声部芯片音乐合成器（MML 乐谱 + 滑音），原创开机曲、提示音和两首背景音乐 |
| **交互模块**（可选） | 6 颗彩灯随状态变化，按键、人体感应、光线 |
| **日常** | 中英文界面、时钟和按时段问候、随时间变化的背景（清晨 / 白天 / 黄昏 / 夜晚）、夜间勿扰、番茄钟、久坐提醒、玩法说明页 |
| **Home Assistant** | 设置 `SLIME_MQTT_URL` 后，史莱姆通过 MQTT 自动出现在 Home Assistant 里：人体感应、摄像头看到你、环境光、电量、Claude 状态和“Claude 在等你”可以当自动化的触发条件；HA 也能让它说话、开始专注、调音量和亮度、开关背景音乐和夜间勿扰 |
| **网页面板** | 浏览器打开 `http://slime.local/`：实时状态、全部设置、音乐试听、摄像头画面 |
| **无线更新** | 双程序位 OTA，新固件跑满 30 秒才确认，崩溃会自动退回旧版 |

## 硬件

- **ESP-Mosaico 开发板**（ESP32-S31，480×480 AMOLED，触摸，BMI270 IMU，ES8311 音频，振动马达，电池电量计）。需要 ESP-IDF 6.2 及以上（目前只有 master 分支满足）
- 可选：**交互模块**（任一槽位，可热插拔）、**摄像头模块** SC101IOT（只能插左槽）

Wi-Fi 只支持 2.4 GHz。

## 快速开始

```bash
git clone https://github.com/8Avalon8/slime-pet && cd slime-pet
git clone https://github.com/esp-mosaico/esp-mosaico-bsp third_party/esp-mosaico-bsp

cd firmware/slime
. $IDF_PATH/export.sh                      # ESP-IDF master
idf.py --preview set-target esp32s31
idf.py build > /tmp/slime_build.log && grep "Project build complete" /tmp/slime_build.log
```

**第一次烧录（USB）**：用数据线连上开发板的 Type-C 口。

```bash
python tools/backup_and_flash.py
```

第一次运行会先把整片闪存（出厂固件）备份到仓库根目录的 `backup/`，之后每次烧录前还会备份一次设置区。脚本会自动让设备重启进下载模式，不用按键。

**连 Wi-Fi**：在你自己的终端里运行，密码不回显，只通过 USB 线发给设备，不会落盘。

```bash
python3 ../../bridge/wifi_setup.py
```

**之后都用无线更新**：

```bash
idf.py build > /tmp/slime_build.log
python tools/ota_flash.py
```

第一次无线更新会通过 USB 向设备要一个更新口令，缓存在 `bridge/.ota_token`，以后就不用插线了。

手势识别的两个模型放在单独的闪存分区里，无线更新写不到那里：从没有手势识别的旧版本升级上来时，要再用 USB 烧录一次（`python tools/backup_and_flash.py`，设置和等级会保留）。没烧之前其他功能照常，只是手势开关打开后史莱姆会告诉你模型还没装上。

**接上 Claude Code**（在仓库根目录运行）：

```bash
python3 bridge/install_hooks.py            # 写入 ~/.claude/settings.json，会先备份
python3 bridge/install_hooks.py --uninstall
```

钩子是异步的，不会拖慢 Claude；设备不在线时静默失败。

**Windows / Linux**：`bridge/` 下的脚本同样可用，仍然只用标准库（命令里的 `python3` 换成 `python`）。Windows 按 Espressif 的 USB 厂商号（303A）在注册表里找 COM 口，Linux 用 `/dev/serial/by-id/`；找不到时用环境变量 `SLIME_PORT` 指定，比如 `SLIME_PORT=COM5`。`install_hooks.py` 会把运行它的那个 Python 写进钩子命令。固件工具（`backup_and_flash.py`、`ota_flash.py`）目前仍只支持 macOS。

**AI 点评**（可选）：在 [LM Studio](https://lmstudio.ai/) 里下载 `gemma-4-e4b-it` 并开启本地服务（默认 `http://localhost:1234`）。之后 Claude 每做完一轮用到工具的工作，钩子会在后台生成一句点评发给史莱姆。设备的设置里可以关掉；电脑端用环境变量 `SLIME_AI=0` 关闭，`SLIME_LLM_URL` / `SLIME_LLM_MODEL` 换成别的接口或模型（Ollama 等本地服务直接可用）。点评、主动陪伴和语音回答的语言跟随设备的界面语言（通过 Wi-Fi 询问设备），`SLIME_LANG=zh|en` 可以强制指定。

想用云端 API（OpenAI、DeepSeek、OpenRouter 等 OpenAI 兼容接口）时，再设置 `SLIME_LLM_KEY`，例如：

```bash
export SLIME_LLM_URL=https://api.deepseek.com/v1
export SLIME_LLM_MODEL=deepseek-chat
export SLIME_LLM_KEY=sk-...
```

这三项（以及下面语音转文字的地址、模型和 Key）更方便的填法是网页面板：打开 `http://slime.local/` 的“AI 设置”，保存后电脑端的钩子和 buddy 会自动从设备读取，不用再配环境变量；设置了环境变量时以环境变量为准。API Key 保存后面板不会再显示，电脑端凭 `bridge/.ota_token`（第一次无线更新时取到的更新口令）才能读到；它在局域网里是明文传输的，只在信得过的网络里填。`python3 bridge/slime_brain.py config` 可以看当前每一项的值来自哪里。

注意：这时每轮的精简摘要（你的请求前 300 字、最近几次工具操作、Claude 最后一句回复的前 600 字）会发给该服务商。不含文件内容和工具输出。

**记忆、主动陪伴和语音对话**（可选）：钩子会把每个事件记到 `bridge/.slime/journal/`（只在本机，保留 60 天），点评会参考这些记录养成的性格。主动陪伴、日记和语音对话需要另开一个终端常驻运行（Windows / macOS / Linux 都行，只用标准库，走 Wi-Fi）：

```bash
python3 bridge/slime_buddy.py              # Ctrl-C 结束；-v 打印每次判断
python3 bridge/slime_buddy.py diary        # 立刻写今天的日记（平时过了零点自动写昨天的）
python3 bridge/slime_buddy.py ask "今天干了啥"   # 不用麦克风，试一下语音回答
python3 bridge/slime_brain.py traits       # 看看它现在养成了什么性格
python3 bridge/slime_rhythm.py             # 它学到的作息：平时几点到几点在电脑前
```

环境变量和钩子一样（`SLIME_LLM_URL` / `SLIME_LLM_MODEL` / `SLIME_LLM_KEY`），记得在运行 buddy 的终端里也设置。日记存在 `bridge/.slime/diary/`。

聊天时史莱姆知道：日期、星期和时段，自己的等级和电量，专注计时，摄像头有没有看到你，今天的工作和各个 Claude 会话的状态，今天早些时候聊过的话和昨天的日记。问到天气时会去 [wttr.in](https://wttr.in)（免费、不用 Key）查今明两天的天气，地点按电脑的 IP 猜，可用 `SLIME_CITY=Shanghai` 指定，`SLIME_WEATHER=0` 关闭。

模型支持函数调用时（DeepSeek、OpenAI 等 OpenAI 兼容接口），它还能自己调用工具（`bridge/slime_agent.py`）：查别的城市的天气，翻过去的聊天、工作记录和日记，看某一天干了什么，长期记住或忘掉你告诉它的事（“记住我不吃香菜”），定提醒（“二十分钟后提醒我喝水”，到点在屏幕上说），操作设备本身（开始/结束专注计时、音量、亮度、背景音乐）。每次回答最多两轮工具调用，同一轮的工具并行执行、每轮最多等 6 秒；接口不支持工具时自动退回普通回答。`SLIME_TOOLS=0` 关闭工具，`python3 bridge/slime_agent.py tools` 查看发给模型的工具定义。记住的事和提醒存在 `bridge/.slime/facts.json`、`reminders.json`。

语音对话还需要一个语音转文字接口（OpenAI 兼容的 `/audio/transcriptions`）。默认用 `SLIME_LLM_URL` 和 `SLIME_LLM_KEY`、模型 `whisper-1`，所以直接用 OpenAI 时不用另外配置；LM Studio 不提供语音转文字，本地可以用 [Speaches](https://github.com/speaches-ai/speaches) 这类兼容服务：

```bash
export SLIME_STT_URL=http://localhost:8000/v1
export SLIME_STT_MODEL=Systran/faster-whisper-small
```

用法：长按 AI 键，屏幕显示"我在听"后说话（最长 10 秒），松开等它回答。录音只在你按住按键时进行，buddy 取走一次后设备上就不再提供。语音和对话内容会发给你配置的语音和模型接口。设备设置里的"长按 AI 键说话"可以关掉，关掉后长按 AI 键恢复为玩法说明。

也可以不按键：喊"小龙小龙"，它回一句"我在！你说～"后直接说话，停顿一秒左右自动结束；唤醒后 5 秒没人说话就算了。唤醒词由设备自己识别（乐鑫 esp-sr 的 WakeNet，模型打包在固件里），只有唤醒之后的那一句才会发给电脑。设置里的"喊小龙小龙唤醒"可以关掉。想换唤醒词：在 `sdkconfig.defaults` 里把 `CONFIG_SR_WN_WN9_XIAOLONGXIAOLONG_TTS` 换成 esp-sr 提供的其他现成模型（`idf.py menuconfig` 的 ESP Speech Recognition 里有列表），重新编译；自定义的词要找乐鑫训练。

**把回答念出来**（可选）：语音对话的回答可以由史莱姆自己念出来，嘴巴跟着声音动。在网页面板“AI 设置”里填“语音合成 Key”就行：只填 Key 时默认用[小米 MiMo](https://platform.xiaomimimo.com/)（目前限时免费）的 `mimo-v2.5-tts`、音色“冰糖”；也可以填硅基流动（`https://api.siliconflow.cn/v1`，CosyVoice2）或任何 OpenAI 兼容的 `/audio/speech` 接口。什么都不填时会试着用语音转文字的接口。MiMo 的音色可选冰糖、茉莉、苏打、白桦；模型换成 `mimo-v2.5-tts-voicedesign` 时，“音色”一栏写一句声音描述（比如“奶声奶气的小史莱姆，语速轻快”）就能捏一个专属声音。合成在电脑上做，设备只播放（16 kHz，最长 20 秒一句）。面板里的“朗读音调”把声音整体调高或调低（像磁带快放），设备设置里的“朗读回答”可以关掉，夜间勿扰时不出声。Claude 等你时的提醒也用这个声音（没配语音合成就只显示），延迟可用 `SLIME_NUDGE_ASK_S` / `SLIME_NUDGE_DONE_S` 调（秒）。环境变量 `SLIME_TTS_URL` / `SLIME_TTS_MODEL` / `SLIME_TTS_KEY` / `SLIME_TTS_VOICE` 优先；`python3 bridge/slime_tts.py "你好呀"` 直接让它说一句，`python3 bridge/slime_tts.py config` 看当前配置。

**接入 Home Assistant**（可选）：设置 MQTT 服务器地址后，`slime_buddy.py` 会顺带把史莱姆接进 Home Assistant（HA 的 MQTT 集成会自动发现它，不用写 YAML）：

```bash
export SLIME_MQTT_URL=mqtt://用户名:密码@homeassistant.local:1883   # TLS 用 mqtts://
python3 bridge/slime_buddy.py          # 或者不跑 buddy，单独运行 python3 bridge/slime_mqtt.py
```

传感器：有人（交互模块的人体感应）、看到你（摄像头）、环境光、电量、充电中、Claude 状态、Claude 在等你、等级、夜间勿扰中、专注剩余分钟。可控制：说话（`notify.send_message`，配了语音合成会念出来）、开始/结束专注、音量、屏幕亮度、背景音乐、夜间勿扰。没插模块或没开摄像头时，对应传感器显示“未知”。比如：Claude 等你时让客厅的灯闪一下，人离开房间就自动开始专注，晚饭好了让史莱姆喊你。

设备离线时实体显示不可用。多只史莱姆共用一个服务器时用 `SLIME_MQTT_ID` 区分（默认 `slime`，话题 `slime/<id>/...`）；`SLIME_MQTT_PREFIX` 改 HA 的发现前缀（默认 `homeassistant`）。目前由电脑端转发，电脑关着时 HA 里就是离线。

## 仓库结构

| 目录 | 内容 |
|---|---|
| `firmware/slime/components/slime_core/` | 纯 C 渲染核心，不依赖 ESP-IDF：光栅库 `sg`、状态机和果冻弹簧 `slime_anim`、渲染 `slime_render`、文字 `slime_text`、菜单 `slime_menu` |
| `firmware/slime/main/` | 设备端：主循环与"大脑"（`main.c`）、Claude Code 会话跟踪 `cc_track`、传感器 `sensors`、音频与合成器 `audio`、乐谱 `tunes_original.h`、摄像头与人脸 `vision`、网络 `net`、网页面板 `web`、设置 `config`/`settings_ui`、无线更新 `ota`、崩溃记录 `bootlog` |
| `firmware/slime/host/` | 电脑端测试程序：用同一份渲染代码出图、测速、跑单元测试；固件模拟器 `slime_sim` |
| `firmware/slime/tools/` | USB 烧录、无线更新、字形生成 |
| `firmware/slime/bootloader_components/` | 引导程序钩子：上电立刻保持电源（否则电池供电开不了机） |
| `firmware/slime/patches/` | 对官方 BSP 的补丁（构建时自动打上） |
| `bridge/` | 电脑端：Claude Code 钩子 `slime_hook.py`、记忆与模型调用 `slime_brain.py`、常驻伙伴 `slime_buddy.py`（主动陪伴、日记、语音对话）、语音合成 `slime_tts.py`、作息学习 `slime_rhythm.py`、Home Assistant 接入 `slime_mqtt.py`、Wi-Fi 配置 |
| `design/slime_preview.html` | 浏览器版设计稿：形象、状态、果冻物理参数的来源 |

## 在电脑上出图和测试

```bash
cd firmware/slime/host
make sheet       # out/sheet.png：所有状态对照图
make test        # cc_track 单元测试 + 双核拆分渲染逐像素对比
./host_sim frame levelup 1.5 out/frame.bmp "叮叮叮！升到了 Lv 13！"
./host_sim helpers 3 2 work out/helpers.bmp   # 派出 3 个小分身 2 秒后的画面
```

（出 PNG 用的是 macOS 的 `sips`；其他系统可以直接看 BMP。）

**没有板子**：`make sim` 在电脑上跑真固件（窗口里能戳、摇、按键，Claude Code 钩子设 `SLIME_HOST=127.0.0.1:8080` 就能连上），`make simtest` 是端到端自动测试。详见 [firmware/slime/host/README.md](firmware/slime/host/README.md)。

## 自定义

- **乐曲**：乐谱在 `main/tunes_original.h`，用 MML 写，语法见 `audio.c` 开头。想用自己的曲子，把同样的两张表放进 `main/tunes_local.h`：这个文件不进 git，存在时会替代原创曲目。
- **文案和字体**：代码里出现的字会预先渲染成三种字号；另有一张 3,755 个常用字的扩展表（22 px），用来显示 AI 点评这类动态文字。改了代码里的中文字符串之后，要重新生成字形，需要 Pillow 和 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)：

  ```bash
  python3 tools/gen_glyphs.py path/to/NotoSansSC.ttf
  ```

- **界面语言**：设备设置第一项「语言 / Language」或网页面板右上角的 EN / 中文 按钮切换，两边同步。设备上的文案都写成 `SL_TR("中文", "English")`（见 `slime_text.h`），英文只用 ASCII，不需要额外字形；网页面板的英文在 `main/web/index.html` 的 `EN_TEXT` 和 `T()` 里。

## 出问题时

- **开不了机、串口没反应**：先在电脑上运行 `python tools/backup_and_flash.py`，它会一直等端口出现。然后按住 BOOT 键再按 PWR 开机，进入芯片自带的下载模式（屏幕是黑的，端口名会变，比如 `usbmodem2101`），烧录会自动开始。设置和等级存在单独的区域，不受影响。
- **卡死或者自己重启了**：任务卡住 5 秒会触发看门狗重启，现场会记录下来。打开 `http://slime.local/api/status`，看 `crash` 字段里的任务名和地址，配合编译出来的 ELF 用 `addr2line` 就能定位到代码行。
- **macOS 上 ESP-IDF 下载组件报证书错误**：先 `export SSL_CERT_FILE=/etc/ssl/cert.pem`。

## 安全说明

- 网页面板**没有登录**，只适合在自己家的局域网里用。语音录音也通过它取走（`/api/voice.wav`，只在你按住 AI 键说话后存在，取一次就失效）。
- Wi-Fi 密码通过 USB 线（`wifi_setup.py`）或网页面板设置，任何接口都不会把它读出来，日志里也不会出现。
- 无线更新必须带口令，口令只能通过 USB 线拿到；设备还会检查镜像是不是这个项目的，并校验 SHA-256。

## 社区

这个项目最早分享在 [LINUX DO](https://linux.do) 社区，感谢佬友们的反馈和起名建议。

## 协议

MIT，见 [LICENSE](LICENSE)。用到的第三方内容（Noto Sans SC 字形、BSP 补丁）见 [THIRD_PARTY.md](THIRD_PARTY.md)。

这是一个爱好者项目，史莱姆是一个通用的复古 RPG 风格形象，与任何游戏公司无关，仓库里不含任何游戏的音乐或素材。
