# Slime Pet · 桌面史莱姆

一只住在 [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-bsp) 开发板（ESP32-S31，480×480 圆角 AMOLED）上的桌面宠物史莱姆。它会和 Claude Code 联动：Claude 思考时它也在思考，干活时显示正在做什么，需要你批准时会叫你、闪灯、震动。它还能看见你、听见你、感觉到你在摇它。

*A desktop slime pet for the ESP-Mosaico board (ESP32-S31, 480×480 AMOLED). It mirrors your Claude Code sessions (thinking, working, waiting for your approval), reacts to touch, tilt, claps, your face and head gestures, plays its own chiptunes, and updates over Wi-Fi. Firmware in C on ESP-IDF; everything runs on the device.*

![史莱姆的各种状态](docs/images/states.png)

## 能做什么

| | |
|---|---|
| **Claude Code 联动** | 思考 / 干活（显示工具和命令）/ 等你批准（叫你、闪灯、震动）/ 完成任务攒经验升级 / 命令出错受伤。多个会话同时跑也分得清。走 Wi-Fi，USB 兜底 |
| **触摸** | 点一下戳它，长按 1 秒打开设置菜单 |
| **动作感应**（板载 IMU） | 歪过来会滑，使劲摇会晕，扣过来放就睡觉 |
| **麦克风** | 拍两下手打招呼，跟着音乐节拍晃（会排除打字声） |
| **摄像头**（可选模块） | 眼睛跟着你、歪头它也歪、点头开心、摇头委屈、盯着它会害羞、盖住镜头躲猫猫、坐太久提醒你起来，右上角小窗看它看到的画面。全部在设备上算，画面不出设备 |
| **声音** | 三声部芯片音乐合成器（MML 乐谱 + 滑音），原创开机曲、提示音和两首背景音乐；内置 sfxr 音效引擎，可以在网页上随机生成、替换任意音效 |
| **交互模块**（可选） | 6 颗彩灯随状态变化，按键、人体感应、光线 |
| **日常** | 时钟和按时段问候、夜间勿扰、番茄钟、久坐提醒、玩法说明页、一键功能演示（录视频用） |
| **网页面板** | 浏览器打开 `http://slime.local/`：实时状态、全部设置、音乐试听、自制音效、摄像头画面 |
| **无线更新** | 双程序位 OTA，新固件跑满 30 秒才确认，崩溃会自动退回旧版 |

## 硬件

- **ESP-Mosaico 开发板**（ESP32-S31，480×480 AMOLED，触摸，BMI270 IMU，ES8311 音频，振动马达，电池电量计）。需要 ESP-IDF 6.2 及以上（目前只有 master 分支满足）
- 可选：**交互模块**（任一槽位，可热插拔）、**摄像头模块** SC101IOT（只能插左槽）

Wi-Fi 只支持 2.4 GHz。

## 快速开始

```bash
git clone <this repo> slime-pet && cd slime-pet
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

**接上 Claude Code**（在仓库根目录运行）：

```bash
python3 bridge/install_hooks.py            # 写入 ~/.claude/settings.json，会先备份
python3 bridge/install_hooks.py --uninstall
```

钩子是异步的，不会拖慢 Claude；设备不在线时静默失败。

## 仓库结构

| 目录 | 内容 |
|---|---|
| `firmware/slime/components/slime_core/` | 纯 C 渲染核心，不依赖 ESP-IDF：光栅库 `sg`、状态机和果冻弹簧 `slime_anim`、渲染 `slime_render`、文字 `slime_text`、菜单 `slime_menu` |
| `firmware/slime/main/` | 设备端：主循环与"大脑"（`main.c`）、Claude Code 会话跟踪 `cc_track`、传感器 `sensors`、音频与合成器 `audio`/`sfxr`、乐谱 `tunes_original.h`、摄像头与人脸 `vision`、网络 `net`、网页面板 `web`、设置 `config`/`settings_ui`、无线更新 `ota`、崩溃记录 `bootlog` |
| `firmware/slime/host/` | 电脑端测试程序：用同一份渲染代码出图、测速、跑单元测试 |
| `firmware/slime/tools/` | USB 烧录、无线更新、字形生成 |
| `firmware/slime/bootloader_components/` | 引导程序钩子：上电立刻保持电源（否则电池供电开不了机） |
| `firmware/slime/patches/` | 对官方 BSP 的补丁（构建时自动打上） |
| `bridge/` | 电脑端：Claude Code 钩子、Wi-Fi 配置 |
| `design/slime_preview.html` | 浏览器版设计稿：形象、状态、果冻物理参数的来源 |

## 在电脑上出图和测试

```bash
cd firmware/slime/host
make sheet       # out/sheet.png：所有状态对照图
make test        # cc_track 单元测试 + 双核拆分渲染逐像素对比
./host_sim frame levelup 1.5 out/frame.bmp "叮叮叮！升到了 Lv 13！"
```

（出 PNG 用的是 macOS 的 `sips`；其他系统可以直接看 BMP。）

## 自定义

- **乐曲**：乐谱在 `main/tunes_original.h`，用 MML 写，语法见 `audio.c` 开头。想用自己的曲子，把同样的两张表放进 `main/tunes_local.h`：这个文件不进 git，存在时会替代原创曲目。
- **音效**：网页面板「自制音效」区可以随机生成 sfxr 音效，或粘贴 [sfxr.me](https://sfxr.me/) 导出的 JSON，替换任意场景的音效。替换存在设备里，不用重新编译。
- **文案和字体**：改了代码里的中文字符串之后，要重新生成字形，需要 Pillow 和 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)：

  ```bash
  python3 tools/gen_glyphs.py path/to/NotoSansSC.ttf
  ```

## 出问题时

- **开不了机、串口没反应**：先在电脑上运行 `python tools/backup_and_flash.py`，它会一直等端口出现。然后按住 BOOT 键再按 PWR 开机，进入芯片自带的下载模式（屏幕是黑的，端口名会变，比如 `usbmodem2101`），烧录会自动开始。设置和等级存在单独的区域，不受影响。
- **卡死或者自己重启了**：任务卡住 5 秒会触发看门狗重启，现场会记录下来。打开 `http://slime.local/api/status`，看 `crash` 字段里的任务名和地址，配合编译出来的 ELF 用 `addr2line` 就能定位到代码行。
- **macOS 上 ESP-IDF 下载组件报证书错误**：先 `export SSL_CERT_FILE=/etc/ssl/cert.pem`。

## 安全说明

- 网页面板**没有登录**，只适合在自己家的局域网里用。
- Wi-Fi 密码通过 USB 线（`wifi_setup.py`）或网页面板设置，任何接口都不会把它读出来，日志里也不会出现。
- 无线更新必须带口令，口令只能通过 USB 线拿到；设备还会检查镜像是不是这个项目的，并校验 SHA-256。

## 协议

MIT，见 [LICENSE](LICENSE)。用到的第三方内容（Noto Sans SC 字形、sfxr、BSP 补丁）见 [THIRD_PARTY.md](THIRD_PARTY.md)。

这是一个爱好者项目，史莱姆是一个通用的复古 RPG 风格形象，与任何游戏公司无关，仓库里不含任何游戏的音乐或素材。
