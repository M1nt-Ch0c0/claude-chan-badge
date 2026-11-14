# 小克的界面代码在 badge/app（和平台无关，模拟器也用同一份），字体在 badge/assets/fonts。
get_filename_component(CLAUDE_CHAN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../app" ABSOLUTE)
get_filename_component(CLAUDE_CHAN_FONT "${CMAKE_CURRENT_LIST_DIR}/../../assets/fonts/NotoSansSC-GB2312.ttf" ABSOLUTE)

idf_component_register(
    SRCS "cc_main.c"
         "cc_bridge.c"
         "cc_net.c"
         "cc_lan.c"
         "cc_platform_idf.c"
         "${CLAUDE_CHAN_DIR}/app/claude_chan_app.c"
         "${CLAUDE_CHAN_DIR}/app/cc_avatar.c"
         "${CLAUDE_CHAN_DIR}/app/cc_sprites.c"
         "${CLAUDE_CHAN_DIR}/core/cc_chat.c"
         "${CLAUDE_CHAN_DIR}/core/cc_reply.c"
         "${CLAUDE_CHAN_DIR}/core/cc_sse.c"
         "${CLAUDE_CHAN_DIR}/core/cc_stream.c"
    INCLUDE_DIRS "." "${CLAUDE_CHAN_DIR}/app" "${CLAUDE_CHAN_DIR}/core"
    REQUIRES bsp esp_timer json mbedtls esp_driver_usb_serial_jtag esp_wifi esp_netif esp_event nvs_flash esp_http_client esp-tls
    EMBED_FILES "${CLAUDE_CHAN_FONT}"
)

# LVGL 断言改为 panic 重启，不再 while(1) 卡死画面。
idf_component_get_property(cc_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${cc_lvgl_lib} PRIVATE "-include" "${CMAKE_CURRENT_LIST_DIR}/cc_lv_assert.h")
