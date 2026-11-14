// cc_lv_assert.h — 强制包含进 LVGL 库的编译单元（见 main/CMakeLists.txt）。
//
// LVGL 默认的断言处理是 while(1)：内存一旦申请失败，LVGL 任务就原地空转，
// 画面永久停住，而音频任务还在跑（2026-11-14 实测的「字幕卡死、声音跳着续上」）。
// 改成陷阱指令：panic 处理打出回溯，然后重启，十几秒后自己恢复。
#pragma once
#define LV_ASSERT_HANDLER __builtin_trap();
