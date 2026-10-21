idf_component_register(
    SRCS "recorder.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_partition esp_timer nvs_flash fy_battery fy_detector fy_gps fy_signatures
                  fy_store
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
