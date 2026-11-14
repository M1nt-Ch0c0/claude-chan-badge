# 开发手册

## 目录

1. [整体结构](#整体结构)
2. [VoiceDesk 内部](#voicedesk-内部)
3. [徽章固件](#徽章固件)
4. [内存](#内存)
5. [调试工具](#调试工具)
6. [测试](#测试)

## 整体结构

```
徽章 (ESP32-C3)                 Mac (VoiceDesk)                      云端 / 本机
麦克风 ──PCM 16k──▶ TCP :19731 ──▶ 重采样 16k→24k ──▶ Realtime WebSocket
喇叭   ◀─PCM 16k── ◀────────────── Pacer ◀── 24k→16k ◀──┘
屏幕   ◀─阶段 / 字幕位图── 字幕渲染 ◀── 转写、回答文字
                                   工具调用 ──▶ claude -p（后台任务）
```

徽章不做 TLS、不保存密钥、不做重采样。原因见[内存](#内存)一节：开着 Wi-Fi 时再做一次 TLS 握手，剩余内存不够放音频缓冲。

帧格式见 [protocol.md](protocol.md)。

## VoiceDesk 内部

### 源文件

| 文件 | 内容 |
| --- | --- |
| `Engine.swift` | 徽章连接、Realtime 会话、工具调用、阶段和字幕 |
| `Link.swift` | `BadgeLink`（加锁的整帧写入）和 `Pacer`（按实时速度发音频） |
| `Subtitle.swift` | 用 CoreText 把字幕渲染成 A4 位图，计算增量 |
| `Agent.swift` | 启动和管理 `claude -p` 进程，解析 stream-json 输出 |
| `State.swift` | 界面状态和快照 |
| `WebUI.swift` | WKWebView 宿主，状态以每秒最多 30 次的频率推给页面 |
| `Audio.swift` | 线性重采样、切帧、RMS |
| `Protocol.swift` | 帧打包和解析 |
| `Resources/ui/index.html` | 全部界面（HTML/CSS/JS 单文件） |

### 线程

| 队列 | 负责 |
| --- | --- |
| `voice.desk` | Realtime 事件、工具调用、阶段判断、字幕（每 50 ms 一次 tick） |
| `voice.desk.tcp` | 徽章套接字的阻塞读循环（`select`） |
| `voice.desk.pacer` | 每 40 ms 发一次音频 |
| `voice.desk.agent` | 后台任务进程的输出解析 |
| 主线程 | 界面状态，WebView 通信 |

TCP 读循环和 Realtime 事件不能放在同一个串行队列：读循环不返回，事件就永远处理不到。

### 音频节奏

模型的音频到达速度远快于播放速度。`Pacer` 只比播放进度多发 160 ms，每 40 ms 把两帧拼成一个 TCP 包。这样带来三个结果：

- Mac 知道徽章大约播到了哪里，字幕可以按这个进度逐字显示；
- 暂停时只要丢掉还没发出的帧，徽章最多再播 160 ms；
- 阶段 0（空闲）在徽章真正播完后才发出。

播放期间和结束后 0.4 秒内，麦克风帧被丢弃，防止喇叭的声音被当作用户输入。

### 字幕

- 用户的话：来自 `conversation.item.input_audio_transcription.delta`，边转写边显示。
- 模型的回答：文字来自 `response.output_audio_transcript.delta`，显示进度由音频播放进度决定（已播时长 ÷ 这段音频总时长 × 字数）。
- 徽章字幕每秒最多更新 4 次。徽章声明支持位图（HELLO 第 6 字节 bit0）时发 `SUBIMG`，否则发 `TEXT`。
- 位图宽 200 px，16 px 字体每行 21 px，最多 3 行；后台任务进度用 14 px 字体，最多 2 行。超出的行从上面截掉。

### Realtime 会话

`session.update` 的要点：

- 输入输出都是 `audio/pcm` 24 kHz；`server_vad`，静音 500 ms 判定说完；
- 开启输入转写，语言 `zh`；
- 工具：`run_agent_task`、`check_agent_tasks`、`cancel_agent_task`、`open_app`、`open_url`。

调用 `run_agent_task` 或 `check_agent_tasks` 之后，下一次 `response.create` 带 `tool_choice: none`，避免模型反复自行查询进度。如果这一轮只调用了 `run_agent_task`，且模型在调用前已经说过话，就不再追加回复。

任务完成时，结果以 `role: system` 的消息加入对话，再请求一次回复，由模型用语音播报。用户主动查询过结果的任务不再重复播报。

### 后台任务

`AgentRunner` 用下面的参数启动 Claude Code：

```
claude -p <任务> --output-format stream-json --verbose --model <模型> --append-system-prompt <说明>
```

权限参数：

| 级别 | 参数 |
| --- | --- |
| 只看 | `--permission-mode default --allowedTools Read,Glob,Grep,WebSearch,WebFetch,TodoWrite` |
| 标准 | `--permission-mode acceptEdits`，允许上面的工具加 `Write,Edit,Bash`，`--disallowedTools` 禁止 `rm`、`sudo`、`git push`、`shutdown`、`reboot`、`diskutil`、`dd`、`mkfs`、`launchctl`、`defaults delete` |
| 完全 | `--dangerously-skip-permissions` |

进程在用户主目录下运行。`continue_previous` 为真时，加上 `--resume <上一个成功任务的 session_id>`。超过 600 秒会被终止。

### 密钥查找顺序

1. 环境变量 `VOICEDESK_API_KEY` / `VOICEDESK_BASE_URL`
2. `~/.config/voicedesk/config.json`
3. `~/.claude/settings.json` 的 `env.ANTHROPIC_API_KEY` / `env.ANTHROPIC_BASE_URL`（适用于 Claude Code 和 Realtime 共用同一个中转的情况）

密钥只用于请求头，不写入日志。

### 界面开发

```sh
VOICEDESK_UI_DIR=Sources/Resources/ui .build/debug/VoiceDesk
```

设置这个环境变量后，页面从源码目录加载，`⌘R` 重新加载。`index.html` 也可以直接用浏览器打开，此时会使用内置的模拟数据，可以加参数切换状态：`?phase=listening`、`?phase=thinking`、`?phase=paused`、`?empty=1`。

## 徽章固件

### 源文件

| 文件 | 内容 |
| --- | --- |
| `main/cc_main.c` | 启动、按键任务、堆监控 |
| `main/cc_lan.c` | 局域网连接、收发帧、字幕位图缓冲 |
| `main/cc_net.c` | Wi-Fi 连接；文字对话模式下的 HTTPS 请求 |
| `main/cc_platform_idf.c` | `cc_platform.h` 的 ESP-IDF 实现 |
| `main/cc_bridge.c` | USB 调试桥（截屏、注入按键、查内存） |
| `main/cc_lv_assert.h` | 强制包含进 LVGL 的断言定义 |
| `components/bsp` | FoloToy 的板级支持包，未修改 |

界面代码在 `badge/app`，由 `main/CMakeLists.txt` 引入。

### 分区表

| 名称 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- |
| nvs | `0x9000` | 24 KB | |
| phy_init | `0xf000` | 4 KB | |
| cardid | `0x356000` | 16 KB | 原厂设备身份，只声明，不写入 |
| factory | `0x360000` | 4.6 MB | 应用（约 4.2 MB，其中字体 2.4 MB、六张立绘约 0.65 MB） |

### 线程栈

| 任务 | 栈 |
| --- | --- |
| LVGL（esp_lvgl_port） | 7 KB |
| `cc_lan` | 8 KB |
| `cc_keys` | 8 KB |
| `cc_bridge_rx` | 6 KB |
| `cc_heap` | 3 KB |
| Wi-Fi 驱动（闭源） | 6.5 KB |

### 字幕位图

`cc_lan` 任务把收到的 A4 数据写进一块 7.5 KB 的静态缓冲。界面线程每 40 ms 用序号锁读一次（写入期间序号为奇数，读前读后序号不一致就放弃，等下一次），展开成 A8 写进 14 KB 的堆缓冲，作为 LVGL 图像显示，颜色通过 `image_recolor` 设置。

LVGL 的软件渲染不能直接绘制 A4，每次绘制都会临时申请一块 A8 缓冲，所以在徽章上展开一次，常驻为 A8。

### LVGL 断言

LVGL 默认的断言处理是 `while(1)`。内存申请失败时，LVGL 任务停在原地，画面冻结，而音频任务继续运行。`main/CMakeLists.txt` 给 LVGL 库加了 `-include cc_lv_assert.h`，把断言改成 `__builtin_trap()`：触发 panic，打印回溯后重启。

## 内存

ESP32-C3 共有约 400 KB SRAM。放在 IRAM 里的代码和堆共用这块内存。

### 实测空闲堆

| 阶段 | 裁剪前 | 裁剪后 |
| --- | ---: | ---: |
| BSP 就绪 | 188 KB | 201 KB |
| Wi-Fi 已连接 | 135 KB | 155 KB |
| 进入界面 | 76 KB | 97 KB |

裁剪后连续对话约一分钟，空闲堆最低为 54 KB。更长时间的运行还没有测量。

更早的测量（文字对话模式，徽章自己发 HTTPS）：TLS 握手后空闲 32 KB、最低 22 KB、最大连续块 17 KB。徽章不做 TLS 就是因为这组数据。

### 协议栈裁剪

`sdkconfig.defaults` 末尾的一组选项：

| 选项 | 值 | 作用 |
| --- | --- | --- |
| `ESP_WIFI_IRAM_OPT` / `ESP_WIFI_EXTRA_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT` | n | Wi-Fi 代码放回 flash |
| `LWIP_IRAM_OPTIMIZATION` | n | lwIP 代码放回 flash |
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 6（原 10） | 静态接收缓冲 |
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` / `DYNAMIC_TX_BUFFER_NUM` | 16（原 32） | 动态缓冲上限 |
| `ESP_WIFI_AMPDU_TX_ENABLED` | n | |
| `ESP_WIFI_SOFTAP_SUPPORT` / `ENTERPRISE_SUPPORT` | n | |
| `LWIP_IPV6` / `LWIP_DHCPS` | n | |
| `LWIP_TCP_WND_DEFAULT` / `SND_BUF_DEFAULT` | 2880（原 5760） | |

静态内存（`idf.py size` 的 DRAM 一项）从 140.8 KB 降到 119.9 KB；加上字幕位图的 7.5 KB 缓冲后为 127.5 KB。

### 处理过的问题

**画面冻结，声音还在**

现象：字幕和立绘停住；声音停顿一下后跳着续上，之后画面不再恢复。

原因：字幕由徽章自己用 TinyTTF 排字，16 px 字体缓存最多 96 个字形，每个约 0.5 KB，装满时约 48 KB。开着 Wi-Fi 时堆被耗尽，LVGL 申请内存失败，进入 `while(1)`。

处理：字形缓存降到 16 px 48 个、14 px 24 个；断言改成 panic；后来字幕改为 Mac 渲染的位图。

**运行几分钟后重启**

现象：空闲堆在约 6 分钟内从 50 KB 降到 3 KB，然后重启。

原因：Wi-Fi 处于省电模式（`WIFI_PS_MIN_MODEM`）时，路由器把 20 ms 一个的小包攒起来，大约每 100 ms 成批送达，每个包各占一块约 1.6 KB 的接收缓冲。接收循环每轮只读 1 KB，读的速度跟不上，未读的包一直占着缓冲。

处理：

- 改为 `WIFI_PS_NONE`；
- 接收循环每轮把套接字读空；
- Mac 每 40 ms 把两帧音频合成一个 TCP 包，徽章字幕每秒最多更新 4 次；
- 收发缓冲上限减半。

**Mac 写了半帧**

徽章接收慢时，Mac 的 `send` 可能在 2 秒超时后只写出半帧，之后的字节流就错位了。现在一旦出现半帧写入，就主动断开连接，由徽章重连。

### 改动时注意

- 每 15 秒会打印一行 `堆 巡检：空闲 … 最低 … 最大块 …`，空闲低于 24 KB 时改为 `堆偏低` 警告。
- 新增常驻缓冲之前，先看「进入界面」那一行的空闲值，并为 Wi-Fi 突发流量留出余量（实测波动 20～30 KB）。
- 不要在徽章上增加按字形缓存的字体用量，新的文字显示优先走 Mac 渲染。

## 调试工具

### USB 调试桥

`badge/firmware/tools/cc_bridge.py` 配合固件里的 `cc_bridge.c`，通过 USB 串口工作：

```sh
sh badge/firmware/tools/bridge_start.sh <日志名> /dev/cu.usbmodemXXXX
echo "shot /tmp/ccb/a.png" >> /tmp/ccb/bridge.cmd   # 截屏
echo "key ok click"        >> /tmp/ccb/bridge.cmd   # 注入按键
echo "mem"                 >> /tmp/ccb/bridge.cmd   # 查内存
```

日志写到 `/tmp/ccb/<日志名>.log`，带时间戳。脚本打开串口时把 DTR/RTS 置低，以避免复位芯片，但实测部分情况下仍会复位一次。`tools/dev.sh` 里有几个 shell 辅助函数（`k`、`shot`、`waitlog`）。

### 假徽章

`tools/fake_badge.py` 见 README。打印的几项含义：

- `CTRL a0xx`：阶段变化，正常顺序是 01 → 02 → 03 → 00；
- `平均 xx 帧/秒`：应接近 50，说明音频是按实时速度发送的；
- `字幕位图提交 n 次，共 m 字节`：字幕占用的带宽。

## 测试

| 内容 | 命令 |
| --- | --- |
| 徽章界面核心逻辑（ASan + UBSan） | 见 `badge/app/README.md` |
| VoiceDesk 编译 | `cd mac/VoiceDesk && swift build` |
| 固件编译 | `cd badge/firmware && idf.py build` |
| 端到端（无硬件） | `tools/fake_badge.py` |

VoiceDesk 目前没有单元测试，音频节奏、字幕、工具调用的行为都是用假徽章验证的。
