idf_component_register(
    SRCS "dashboard_text.cpp" "dashboard.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_netif esp_timer esp_wifi nvs_flash
                  fy_battery fy_detector fy_gps fy_recorder fy_signatures
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
