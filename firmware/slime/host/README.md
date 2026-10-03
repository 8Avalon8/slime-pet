# 没有板子也能测

[中文](#中文) · [English](#english)

## 中文

三种工具，从轻到重：

| 工具 | 跑的是什么 | 适合 |
|---|---|---|
| `host_sim` / `cc_test`（`make test`） | 渲染核心和 `cc_track` 协议 | 改形象、动画、协议解析 |
| `bridge/fake_device.py` | 只模仿 HTTP 接口的 Python 假设备，不用编译 | 改 bridge 钩子、AI 点评、网页面板 |
| `slime_sim`（`make sim`） | **真固件**：`main.c`、`web.c`、`config.c`、`settings_ui.c` 等原样编译，跑在一层假的 ESP-IDF 上 | 改"大脑"状态机、交互、设置菜单，端到端联调 |

### 模拟器 slime_sim

```bash
# macOS
brew install sdl2 cjson
# Ubuntu / Debian
sudo apt install libsdl2-dev libcjson-dev

cd firmware/slime/host
make sim                      # 编译并打开窗口
make simtest                  # 端到端自动测试，截图在 out/sim/
```

没有 SDL2 时（或加 `--headless`）没有窗口，只能用 HTTP 和终端驱动。cJSON 也可以不装：设置了 `IDF_PATH` 时会用 ESP-IDF 自带的那份。

**接上 Claude Code**：模拟器在 `127.0.0.1:8080` 提供和设备一样的接口。启动 Claude Code 前设好环境变量，钩子就会发给模拟器，而不是 `slime.local`：

```bash
export SLIME_HOST=127.0.0.1:8080
claude
```

网页面板在 http://127.0.0.1:8080/ 。

**窗口操作**：鼠标就是触摸屏（点一下戳，按住 1 秒开设置）。按键：

| 键 | 作用 |
|---|---|
| A / Shift+A | AI 键：单击静音 / 长按帮助页 |
| B / Shift+B | BOOT 键：单击专注计时 / 长按设置 |
| Z / X | 交互模块左 / 右键（音量） |
| S | 摇晃 |
| ← / →（按住） | 倾斜；Shift+← / → 是磕一下 |
| F | 屏幕朝下（睡觉）开关 |
| C / Shift+C | 拍手 / 拍两下 |
| M / L | 人体感应 / 关灯开关 |
| N / H / W | 点头 / 摇头 / 挥手（只有事件，没有摄像头画面） |
| P | 电量：90% → 10% → 充电 → 无电量计 |
| F12 | 截图到当前目录 |

**脚本驱动**：同样的输入也能用文字命令发，适合自动化测试或让 AI agent 来操作：

```bash
curl -d 'shake' http://127.0.0.1:8080/sim/input
curl -d $'tap 100 240\nbattery 10' http://127.0.0.1:8080/sim/input
curl -o screen.bmp http://127.0.0.1:8080/sim/screen.bmp
curl http://127.0.0.1:8080/sim/input       # 命令列表
```

终端里也能输：`sim shake` 是模拟输入；其他行相当于在设备的 USB 串口里敲（比如 `state think`、`say happy 你好`、`cc 0000abcd 1 prompt`）。

参数：`--port`、`--bind`、`--nvs FILE`（设置和等级跨次保留，默认每次重来）、`--scale 2`、`--headless`、`--quiet`、`--exit-after 秒 --screenshot 文件.bmp`。

**和真机的差别**：

- 声音和震动只打印日志，不出声。
- 没有摄像头画面，人脸跟随、坐太久提醒这些不会触发；点头、摇头、挥手可以用按键发事件。
- Wi-Fi 永远"已连接"，OTA 返回 403，没有崩溃记录。
- LED 条只显示每种心情的主色，没有流水灯效果。
- 渲染速度是电脑的，不代表设备上的帧率（看帧率用 `make bench` 或真机）。

**原理**：`sim/idf/` 下是一组同名头文件（`esp_log.h`、`freertos/task.h`、`bsp/esp_mosaico.h`、`esp_http_server.h` 等），实现在 `sim/sim_idf.c`，用 pthread 实现 FreeRTOS 任务和信号量，NVS 放内存，LCD 写进窗口的帧缓冲，HTTP 服务器是一个小的 socket 服务。音频、传感器、摄像头、网络这些硬件模块在 `sim/sim_drivers.c` 里有替身，接口和 `main/` 下的头文件一样。`main/` 的代码一行没改。

固件用了新的 IDF 函数时，模拟器会编译失败，在 `sim/idf/sim_idf.h` 加声明、在 `sim_idf.c` 加实现即可；新的硬件模块就在 `sim_drivers.c` 加替身。

### 假设备 fake_device.py

不用编译，只有 Python 标准库：

```bash
python3 bridge/fake_device.py              # 默认 127.0.0.1:8080
SLIME_HOST=127.0.0.1:8080 python3 bridge/slime_hook.py --send ask "Bash: rm -rf build"
```

每收到一行就打印出来，并粗略显示史莱姆会进入什么状态。它只模仿协议，状态是简化版；要看真实反应用模拟器。

## English

Three tools, lightest first:

| Tool | What runs | Good for |
|---|---|---|
| `host_sim` / `cc_test` (`make test`) | the rendering core and the `cc_track` protocol | looks, animation, protocol parsing |
| `bridge/fake_device.py` | a Python stand-in for the HTTP API, no build | the bridge hook, AI comments, the web panel |
| `slime_sim` (`make sim`) | **the real firmware**: `main.c`, `web.c`, `config.c`, `settings_ui.c` and friends compiled unchanged on a fake ESP-IDF | the "brain", interactions, the settings menu, end-to-end runs |

```bash
brew install sdl2 cjson                     # or: sudo apt install libsdl2-dev libcjson-dev
cd firmware/slime/host
make sim                                    # build and open the window
make simtest                                # scripted end-to-end test, screenshots in out/sim/
export SLIME_HOST=127.0.0.1:8080; claude    # point the Claude Code hook at it
```

The mouse is the touch screen; the keys are printed at start-up (and listed in the table above).
`POST /sim/input` takes the same inputs as text commands (`GET /sim/input` lists them) and
`GET /sim/screen.bmp` returns the screen, for scripts and agents. On stdin, `sim <cmd>` is a
simulated input and any other line goes to the pet as if typed on its USB console.

Differences from the board: sound and vibration are only logged, there is no camera picture
(nod, head-shake and wave can be sent as events), Wi-Fi is always connected, OTA answers 403,
the LED strip shows one colour per mood, and frame rates are the computer's, not the board's.

How it works: `sim/idf/` holds headers named like ESP-IDF's, implemented in `sim/sim_idf.c` on
POSIX threads (tasks, semaphores, queues, timers), an in-memory NVS, an LCD that draws into the
window and a small HTTP server; `sim/sim_drivers.c` stands in for the hardware modules behind
the same headers as `main/`. When the firmware starts using a new IDF call the simulator stops
compiling: declare it in `sim/idf/sim_idf.h` and add a stand-in to `sim_idf.c`.
