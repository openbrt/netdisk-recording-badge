idf_component_register(
    SRCS "main.c"
         "kuku_ui.c"
         "kuku_rec.c"
         "kuku_rec_progress.c"
         "kuku_endurance.c"
         "kuku_endurance_core.c"
         "kuku_rec_filename.c"
         "kuku_cloud_path.c"
         "kuku_wav.c"
        "kuku_test.c"
        "kuku_console_line.c"
         "kuku_wifi.cc"
         "kuku_baidu.c"
         "kuku_image.c"
         "kuku_baidu_auth_link.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_wifi fatfs wear_levelling
             esp-wifi-connect esp_http_client json mbedtls
    PRIV_REQUIRES esp_netif esp_event esp_timer esp-tls lwip lvgl esp_driver_usb_serial_jtag esp_app_format
)
if(KUKU_BAIDU_KEYS_HEADER)
    if(NOT IS_ABSOLUTE "${KUKU_BAIDU_KEYS_HEADER}" OR NOT EXISTS "${KUKU_BAIDU_KEYS_HEADER}")
        message(FATAL_ERROR "KUKU_BAIDU_KEYS_HEADER must name an existing absolute private header")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KUKU_BAIDU_KEYS_HEADER="${KUKU_BAIDU_KEYS_HEADER}")
endif()
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/kuku_font_14.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/kuku_font_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/kuku_font_28.c"
)
# 注:基线 demo_*.c / ui_pixel*.c 保留在磁盘上供主机测试(tools/validate.sh
# 直接编译它们),但不参与固件构建 —— 本应用为全新 UI(passport-develop
# 的 mandatory UI redesign 规则)。
