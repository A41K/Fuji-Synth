
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)
project(fuji_synth C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()
add_executable(fuji_synth fuji_synth.cpp)
target_link_libraries(fuji_synth pico_stdlib pico_multicore
    hardware_i2c hardware_adc hardware_gpio hardware_sync pico_audio_i2s)
pico_enable_stdio_usb(fuji_synth 1)
pico_enable_stdio_uart(fuji_synth 0)
pico_add_extra_outputs(fuji_synth)