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
