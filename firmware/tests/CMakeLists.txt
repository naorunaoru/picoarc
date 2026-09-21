cmake_minimum_required(VERSION 3.13)

project(picoarc_host_tests C)

enable_testing()

function(picoarc_add_sanitized_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ../src)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
    target_link_options(${target} PRIVATE
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -fsanitize=address,undefined
    )
    add_test(NAME ${target} COMMAND ${target})
endfunction()

picoarc_add_sanitized_test(audio_samples_test
    audio_samples_test.c
    ../src/audio_samples.c
)
picoarc_add_sanitized_test(core1_safety_test core1_safety_test.c)
