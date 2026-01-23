# main component CMakeLists.txt
# ESP32-C6 Zigbee Gateway main application

idf_component_register(
    SRCS "ble_handlers.c" "ble_service.c" "ble_task.c" "main.c"
         "wifi_task.c"
         "zigbee_task.c"
         "scheduler_task.c"
         "button_task.c"
         "led_task.c"
         "nvs_manager.c"
         "device_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
             esp_wifi
             esp_http_server
             esp_netif
             esp_event
             esp_timer
             spiffs
             json
             int_drivers
             bt
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart ieee802154 espressif__esp-zigbee-lib
)

# Embed SPIFFS partition
spiffs_create_partition_image(storage ../web FLASH_IN_PROJECT)
