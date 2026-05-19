cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico)

if(NOT PICO_SDK_PATH AND EXISTS "$ENV{HOME}/Developer/pico/pico-sdk")
    set(PICO_SDK_PATH "$ENV{HOME}/Developer/pico/pico-sdk")
endif()

if(NOT PICO_TOOLCHAIN_PATH AND EXISTS "/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin")
    set(PICO_TOOLCHAIN_PATH "/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin")
endif()

include(pico_sdk_import.cmake)

project(picoarc C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(picoarc
    src/main.c
    src/arc.c
    src/cec.c
    src/spdif.c
    src/usb_audio.c
    src/usb_descriptors.c
)

pico_generate_pio_header(picoarc ${CMAKE_CURRENT_LIST_DIR}/src/spdif.pio)
pico_generate_pio_header(picoarc ${CMAKE_CURRENT_LIST_DIR}/src/cec_tx.pio)

target_include_directories(picoarc PRIVATE
    src
)

target_compile_definitions(picoarc PRIVATE
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
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
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_pio
    tinyusb_device
)

pico_enable_stdio_usb(picoarc 1)
pico_enable_stdio_uart(picoarc 0)

pico_add_extra_outputs(picoarc)
