idf_component_register(
    SRCS "battery_curve.cpp" "battery.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_adc esp_driver_gpio fy_board
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
