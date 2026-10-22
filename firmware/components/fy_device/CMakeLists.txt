idf_component_register(
    SRCS "device_mode.cpp" "device.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer fy_board fy_dashboard fy_detector fy_status_led fy_usb
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
