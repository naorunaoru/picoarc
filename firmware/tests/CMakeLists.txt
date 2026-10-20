cmake_minimum_required(VERSION 3.13)

project(picoarc_host_tests C)

enable_testing()

add_executable(audio_samples_test
    audio_samples_test.c
    ../src/audio_samples.c
)

target_include_directories(audio_samples_test PRIVATE ../src)
set_target_properties(audio_samples_test PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
target_compile_options(audio_samples_test PRIVATE
    -Wall
    -Wextra
    -Werror
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all
    -fsanitize=address,undefined
)
target_link_options(audio_samples_test PRIVATE
    -fno-omit-frame-pointer
    -fno-sanitize-recover=all
    -fsanitize=address,undefined
)

add_test(NAME audio_samples COMMAND audio_samples_test)

add_executable(diagnostics_trace_test diagnostics_trace_test.c ../src/diagnostics_trace.c)
target_include_directories(diagnostics_trace_test PRIVATE ../src)
target_compile_definitions(diagnostics_trace_test PRIVATE PICOARC_USB_DIAGNOSTICS=1)
set_target_properties(diagnostics_trace_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(diagnostics_trace_test PRIVATE
    -Wall -Wextra -Werror -fno-omit-frame-pointer -fno-sanitize-recover=all -fsanitize=address,undefined)
target_link_options(diagnostics_trace_test PRIVATE
    -fno-omit-frame-pointer -fno-sanitize-recover=all -fsanitize=address,undefined)
add_test(NAME diagnostics_trace COMMAND diagnostics_trace_test)

# Compile the real descriptors and USB request callbacks with only hardware and
# transport calls stubbed. Exercise both audio classes and CDC/debug layouts.
# Use TinyUSB's strict-alignment path, as on RP2040, even on ARM64 hosts.
foreach(uac RANGE 1 2)
    foreach(debug RANGE 0 1)
        set(target audio_rates_uac${uac}_debug${debug}_test)
        add_executable(${target} audio_rates_test.c
            ../src/audio_rates.c ../src/usb_descriptors.c ../src/usb_audio.c
            ../src/audio_samples.c)
        target_include_directories(${target} PRIVATE stubs ../src ../third_party/tinyusb/src)
        target_compile_definitions(${target} PRIVATE
            CFG_TUSB_MCU=OPT_MCU_NONE CFG_TUSB_OS=OPT_OS_NONE TUP_DCD_ENDPOINT_MAX=8
            PICOARC_UAC_VERSION=${uac} PICOARC_DEBUG_USB=${debug}
            PICOARC_LOGGING=1 PICOARC_USB_DIAGNOSTICS=0)
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
        target_compile_options(${target} PRIVATE
            -UNDEBUG -U__ARM_FEATURE_UNALIGNED -Wall -Wextra -Werror -Wno-unused-parameter
            -fno-omit-frame-pointer -fno-sanitize-recover=all -fsanitize=address,undefined)
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer -fno-sanitize-recover=all -fsanitize=address,undefined)
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
endforeach()
