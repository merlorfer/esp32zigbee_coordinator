idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio
                  ieee802154
                  espressif__esp-zigbee-lib
                  nvs_flash
)
