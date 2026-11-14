# 徽章上的小克

徽章界面，C + LVGL 9，不依赖 ESP-IDF。对平台的全部依赖（网络、字体、电量、内存、语音阶段、字幕）定义在 `app/cc_platform.h`，固件中的实现是 `badge/firmware/main/cc_platform_idf.c`。

语音模式下屏幕显示立绘、时间、电量和状态行，对话时弹出字幕框。字幕是 Mac 渲染的灰度位图，徽章只负责显示。

`core/` 和 `claude_chan_app.c` 中还保留了一套文字对话（徽章直接请求 Messages API，三选一回复），语音模式下不使用。

## 目录

```
core/   不依赖 LVGL 的逻辑
  cc_sse.*      增量 SSE 解析，固定 2 KB 缓冲
  cc_stream.*   Messages API 流事件 → 正文、停止原因、错误
  cc_reply.*    回复格式解析（表情标签、正文、选项）
  cc_chat.*     对话历史（12 条 / 3 KB 上限）和请求 JSON
app/
  claude_chan_app.*  状态机、字幕框、按键、设备事件
  cc_avatar.*        立绘切换和动作
  cc_sprites.*       六张立绘（RGB565A8，放在 flash 里，由 tools/make_sprites.py 生成）
  cc_platform.h      平台接口
tests/
  test_core.c        core 的单元测试
  cc_cli.c, live_chat.sh  用同一套 core 做一次真实对话
tools/
  make_sprites.py    立绘 PNG → C 数组
```

## 按键

| 操作 | 作用 |
| --- | --- |
| 确定单击 | 暂停 / 继续（与 Mac 同步） |
| 确定双击 | 摸头 |

## 立绘动作

所有位移为 1～3 像素的缓动：待机时缓慢呼吸；说话时切换为张嘴的立绘并轻微起伏；聆听时向一侧平移后停住。不使用定时器驱动的跳变位移。

## 测试

```sh
cd badge/app
mkdir -p build
cc -std=c11 -w -g -fsanitize=address,undefined -c tests/third_party/cJSON/cJSON.c -o build/cJSON.o
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Icore -Itests/third_party/cJSON \
   core/*.c tests/test_core.c build/cJSON.o -o build/test_core && ./build/test_core
```

真实对话的冒烟测试需要一个兼容 Messages API 的地址和密钥，通过环境变量传入：

```sh
CHAT_BASE_URL=https://api.anthropic.com CHAT_API_KEY=... tests/live_chat.sh "早上好"
```
