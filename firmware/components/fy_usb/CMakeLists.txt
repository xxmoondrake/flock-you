idf_component_register(
    SRCS "usb.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_hal_usb esp_netif esp_system espressif__esp_tinyusb fy_dashboard
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
