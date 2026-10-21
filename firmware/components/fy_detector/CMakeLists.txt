idf_component_register(
    SRCS "detector.cpp" "wifi_capture.cpp" "ble_capture.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi bt esp_timer fy_signatures fy_board fy_gps fy_status_led
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
