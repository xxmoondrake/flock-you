idf_component_register(
    SRCS "ui_layout.cpp" "ui_controller.cpp" "ui_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fy_display
    PRIV_REQUIRES esp_timer fy_battery fy_board fy_detector fy_gps fy_recorder fy_signatures
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
