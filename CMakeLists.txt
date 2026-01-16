# ESP32-C6 Zigbee Gateway & Automation Center
# Root CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

# Add int_drivers as extra component directory
set(EXTRA_COMPONENT_DIRS "./int_drivers")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c6_zigbee_gateway)
