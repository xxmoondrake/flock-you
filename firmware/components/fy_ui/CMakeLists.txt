idf_component_register(
    SRCS "ui_layout.cpp" "ui_controller.cpp" "ui_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fy_display
    PRIV_REQUIRES esp_timer fy_battery fy_board fy_dashboard fy_detector fy_device fy_gps fy_recorder fy_signatures fy_store fy_usb
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
