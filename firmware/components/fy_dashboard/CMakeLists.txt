idf_component_register(
    SRCS "dashboard_text.cpp" "auth.cpp" "export.cpp" "notes.cpp" "portal.cpp" "dashboard.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif fy_store
    PRIV_REQUIRES esp_http_server esp_timer esp_wifi lwip mbedtls nvs_flash
                  fy_battery fy_board fy_detector fy_gps fy_recorder fy_signatures
    EMBED_FILES "web/index.html" "web/app.js" "web/app.css"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
