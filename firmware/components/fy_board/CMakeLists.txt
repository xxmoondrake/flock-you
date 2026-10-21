idf_component_register(
    SRCS "board_xiao_esp32s3.cpp" "board_heltec_v43.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_hw_support
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
