idf_component_register(
    SRCS "main.cpp"
    REQUIRES nvs_flash esp_event esp_app_format fy_alerts fy_battery fy_board fy_detector fy_device fy_gps fy_recorder fy_status_led fy_ui fy_usb
)
