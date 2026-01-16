# int_drivers component CMakeLists.txt
# Switch driver with debounce logic

idf_component_register(
    SRCS "switch_driver.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
