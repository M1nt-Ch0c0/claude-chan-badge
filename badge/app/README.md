# 徽章上的小克

这一层是纯 C + LVGL 9 的界面，不认识 ESP-IDF。它需要的东西全部在 `app/cc_platform.h` 里：网络、字体、电量、内存、语音阶段、字幕。固件那边由 `badge/firmware/main/cc_platform_idf.c` 实现。

现在默认跑的是语音模式：屏幕上是立绘、时间、电量，底部一行状态，说话时下面弹出字幕框。字幕是 Mac 排好版发过来的灰度位图，徽章只负责贴上去。

早期还有一套文字对话（视觉小说式，三选一回复），代码还在，`send_user` 目前没人调用。

## 目录

```
core/   不依赖 LVGL 的逻辑
  cc_sse.*      增量 SSE 解析，固定 2 KB 缓冲
  cc_stream.*   Messages API 流事件 → 正文、停止原因、错误
  cc_reply.*    回复格式解析（表情标签、正文、选项）
  cc_chat.*     对话历史（12 条 / 3 KB 上限）和请求 JSON
app/
  claude_chan_app.*  状态机、字幕框、按键、设备事件
  cc_avatar.*        立绘切换和那几像素的动作
  cc_sprites.*       六张立绘（RGB565A8，放在 flash 里，由 tools/make_sprites.py 生成）
  cc_platform.h      平台接口
tests/
  test_core.c        core 的单元测试
  cc_cli.c, live_chat.sh  用同一套 core 做一次真实对话
tools/
  make_sprites.py    立绘 PNG → C 数组
```

## 按键

确定单击：暂停 / 继续（和 Mac 上的暂停按钮是同步的）。
确定双击：摸摸头。

## 立绘的动作

只做 1～3 像素的缓动：慢慢呼吸，说话时换张嘴的图再轻轻起伏，听的时候侧过身来停住。最早的版本是定时器每 140 ms 上下跳 8 像素，看着像在蹦，已经改掉了。

## 测试

```sh
cd badge/app
mkdir -p build
cc -std=c11 -w -g -fsanitize=address,undefined -c tests/third_party/cJSON/cJSON.c -o build/cJSON.o
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Icore -Itests/third_party/cJSON \
   core/*.c tests/test_core.c build/cJSON.o -o build/test_core && ./build/test_core
```

真实对话的冒烟测试要一个 Messages API 兼容的地址和 key（只走环境变量，不打印）：

```sh
CHAT_BASE_URL=https://api.anthropic.com CHAT_API_KEY=... tests/live_chat.sh "早上好"
```
