idf_component_register(
    SRCS "nmea.cpp" "gps.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_uart esp_timer fy_board
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
