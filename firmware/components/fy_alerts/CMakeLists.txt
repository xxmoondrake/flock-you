idf_component_register(
    SRCS "alerts_logic.cpp" "alerts.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer nvs_flash fy_board fy_detector fy_signatures
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
