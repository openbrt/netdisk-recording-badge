idf_component_register(
    SRCS "wifi_manager.cc" "wifi_station.cc" "ssid_manager.cc"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_wifi nvs_flash
)
