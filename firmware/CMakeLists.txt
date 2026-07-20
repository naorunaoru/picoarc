cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico)

set(PICOARC_PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk")
if(NOT EXISTS "${PICOARC_PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "Pico SDK submodule is missing; run 'git submodule update --init --recursive' from the repository root"
    )
endif()
set(PICO_SDK_PATH "${PICOARC_PICO_SDK_PATH}" CACHE PATH "Pico SDK source used by PicoARC" FORCE)

set(PICOARC_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/tinyusb")
if(NOT EXISTS "${PICOARC_TINYUSB_PATH}/hw/bsp/rp2040")
    message(FATAL_ERROR
        "TinyUSB submodule is missing; run 'git submodule update --init --recursive' from the repository root"
    )
endif()
set(PICO_TINYUSB_PATH "${PICOARC_TINYUSB_PATH}" CACHE PATH "TinyUSB source used by PicoARC" FORCE)

if(NOT PICO_TOOLCHAIN_PATH AND EXISTS "/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin")
    set(PICO_TOOLCHAIN_PATH "/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin")
endif()

include(pico_sdk_import.cmake)

project(picoarc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICOARC_VARIANT "debug" CACHE STRING "Firmware variant to build: debug or release")
set_property(CACHE PICOARC_VARIANT PROPERTY STRINGS debug release)

if(PICOARC_VARIANT STREQUAL "debug")
    set(PICOARC_LOGGING 1)
    set(PICOARC_DEBUG_USB 1)
elseif(PICOARC_VARIANT STREQUAL "release")
    set(PICOARC_LOGGING 0)
    set(PICOARC_DEBUG_USB 0)
else()
    message(FATAL_ERROR "Unsupported PICOARC_VARIANT '${PICOARC_VARIANT}'. Use 'debug' or 'release'.")
endif()

pico_sdk_init()

add_executable(picoarc
    src/main.c
    src/arc.c
    src/cec.c
    src/ddc_edid.c
    src/picoarc_log.c
    src/realtime.c
    src/spdif.c
    src/usb_audio.c
    src/usb_descriptors.c
)

pico_generate_pio_header(picoarc ${CMAKE_CURRENT_LIST_DIR}/src/spdif.pio)
pico_generate_pio_header(picoarc ${CMAKE_CURRENT_LIST_DIR}/src/cec_rx.pio)
pico_generate_pio_header(picoarc ${CMAKE_CURRENT_LIST_DIR}/src/cec_tx.pio)

target_include_directories(picoarc PRIVATE
    src
)

target_compile_definitions(picoarc PRIVATE
    PICOARC_VARIANT="${PICOARC_VARIANT}"
    PICOARC_LOGGING=${PICOARC_LOGGING}
    PICOARC_DEBUG_USB=${PICOARC_DEBUG_USB}
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=0
    # TinyUSB runs on core 1, but the RP2040 ROM BOOTSEL entry must be made
    # from core 0. realtime.c supplies a mailbox-backed 1200-baud callback.
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
    PICO_STDIO_USB_RESET_MAGIC_BAUD_RATE=1200
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK=0
    PICO_STDIO_USB_RESET_RESET_TO_FLASH_DELAY_MS=100
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_BOOTSEL=1
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_FLASH_BOOT=1
    PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR=0
)

target_link_libraries(picoarc
    pico_stdlib
    pico_i2c_slave
    pico_multicore
    pico_unique_id
    hardware_clocks
    hardware_dma
    hardware_i2c
    hardware_pio
    hardware_pwm
    hardware_watchdog
    tinyusb_device
)

set_target_properties(picoarc PROPERTIES
    OUTPUT_NAME "picoarc-${PICOARC_VARIANT}"
)

# Settings writes will temporarily make XIP unavailable. Copy the complete
# firmware image into SRAM at boot so core 1 can continue servicing USB and
# S/PDIF while core 0 performs a flash erase/program operation.
pico_set_binary_type(picoarc copy_to_ram)

pico_enable_stdio_usb(picoarc ${PICOARC_DEBUG_USB})
pico_enable_stdio_uart(picoarc 0)

pico_add_extra_outputs(picoarc)
