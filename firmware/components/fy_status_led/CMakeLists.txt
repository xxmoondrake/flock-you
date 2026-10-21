idf_component_register(
    SRCS "status_pattern.cpp" "status_led.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer fy_board
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
