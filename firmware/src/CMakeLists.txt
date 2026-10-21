idf_component_register(
    SRCS "main.cpp"
    REQUIRES nvs_flash esp_event fy_battery fy_board fy_detector fy_gps fy_recorder fy_status_led fy_ui
)
