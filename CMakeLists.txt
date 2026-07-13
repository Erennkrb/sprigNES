cmake_minimum_required(VERSION 3.13)

# Point the build at the vendored SDKs unless the user has their own.
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/tools/pico-sdk)
endif()
if(NOT DEFINED ENV{PICO_EXTRAS_PATH} AND NOT PICO_EXTRAS_PATH)
    set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/tools/pico-extras)
endif()

set(PICO_BOARD pico)

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(sprignes C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The ROM is NOT compiled in — the flasher tool appends it to the UF2 at
# flash offset 512 KB (see ROM_FLASH_OFFSET in src/main.cpp).

add_executable(sprignes
    src/main.cpp
    vendor/infones/InfoNES.cpp
    vendor/infones/InfoNES_Mapper.cpp
    vendor/infones/InfoNES_pAPU.cpp
    vendor/infones/K6502.cpp
)

target_include_directories(sprignes PRIVATE
    vendor/infones
)

target_compile_definitions(sprignes PRIVATE
    # Sprig MAX98357A wiring, same values as the stock firmware
    PICO_AUDIO_I2S_MONO_INPUT=1
    PICO_AUDIO_I2S_DATA_PIN=9
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=10
    # 2 = 22050 Hz APU output (see InfoNES_pAPU.h quality table)
    pAPU_QUALITY=2
    SPRIG_ROTATE_180=0
)

target_link_libraries(sprignes PRIVATE
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_vreg
    pico_audio_i2s
)

pico_enable_stdio_usb(sprignes 1)
pico_enable_stdio_uart(sprignes 0)

pico_add_extra_outputs(sprignes)
