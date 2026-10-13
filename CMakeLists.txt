# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

# 强制默认构建类型为 Release（确保启用高级优化）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(hid_usb C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(hid_usb hid_usb.c hid_engine.c
    tusb_config.h
    usb_descriptors.c
    usb_descriptors.h )

pico_set_program_name(hid_usb "hid_usb")
pico_set_program_version(hid_usb "0.3.0-autofish-v2")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(hid_usb 0)
pico_enable_stdio_usb(hid_usb 0)

# Add the standard library to the build
target_link_libraries(hid_usb
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(hid_usb PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/wifi_config.h")
    target_compile_definitions(hid_usb PRIVATE HID_HAS_WIFI_CONFIG=1)
else()
    message(STATUS "Wi-Fi uses CHANGE_ME placeholders; copy wifi_config.h.example to wifi_config.h and configure before deployment")
endif()
# Protocol validation must retain NaN/Inf checks, even with old cache flags.
target_compile_options(hid_usb PRIVATE -Wall -Wextra -Werror -fno-fast-math)

# Add any user requested libraries
target_link_libraries(hid_usb 
        tinyusb_device 
        tinyusb_board
        pico_cyw43_arch_lwip_poll
        )

# Preserve the static IPv4 settings from the existing CircuitPython firmware.
# With LWIP_DHCP=0, the CYW43 driver applies these on every STA initialization,
# including a leave/rejoin after Wi-Fi loss.
set(PICO_STATIC_IP "192.168.11.7" CACHE STRING "Pico static IPv4 address")
set(PICO_STATIC_NETMASK "255.255.255.0" CACHE STRING "Pico IPv4 netmask")
set(PICO_STATIC_GATEWAY "192.168.11.1" CACHE STRING "Pico IPv4 gateway")
set(PICO_STATIC_DNS "8.8.8.8" CACHE STRING "Pico IPv4 DNS server")
pico_configure_ip4_address(hid_usb PRIVATE CYW43_DEFAULT_IP_STA_ADDRESS "${PICO_STATIC_IP}")
pico_configure_ip4_address(hid_usb PRIVATE CYW43_DEFAULT_IP_MASK "${PICO_STATIC_NETMASK}")
pico_configure_ip4_address(hid_usb PRIVATE CYW43_DEFAULT_IP_STA_GATEWAY "${PICO_STATIC_GATEWAY}")
pico_configure_ip4_address(hid_usb PRIVATE CYW43_DEFAULT_IP_DNS "${PICO_STATIC_DNS}")

pico_add_extra_outputs(hid_usb)

