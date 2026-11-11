cmake_minimum_required(VERSION 3.13)
project(armSNES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -fwrapv)

add_library(armsnes STATIC
	src/bus.c
	src/cart.c
	src/cpu.c
	src/dsp.c
	src/input.c
	src/ppu.c
	src/smp.c
	src/state.c
	src/system.c
	src/chips/necdsp.c
	src/chips/sa1.c
	src/chips/gsu.c
)

add_executable(snesrun tests/snesrun.c)
target_link_libraries(snesrun armsnes)

# snesrun-dot: the PPU drawing one dot at a time, the reference the spans are checked against.
add_library(armsnes-dot STATIC EXCLUDE_FROM_ALL src/bus.c src/cart.c src/cpu.c src/dsp.c src/input.c src/ppu.c src/smp.c src/state.c src/system.c src/chips/necdsp.c src/chips/sa1.c src/chips/gsu.c)
target_compile_definitions(armsnes-dot PRIVATE ARMSNES_DOT)
add_executable(snesrun-dot EXCLUDE_FROM_ALL tests/snesrun.c)
target_link_libraries(snesrun-dot armsnes-dot)

# The libretro core, with the Armulators pieces it uses (libretro.h) copied into shared/ so it builds
# without the submodule.
add_library(armsnes_libretro SHARED libretro/libretro.c)
target_include_directories(armsnes_libretro PRIVATE src shared/libretro)
target_link_libraries(armsnes_libretro armsnes)
set_target_properties(armsnes_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
if(NOT APPLE AND NOT WIN32)
	target_link_options(armsnes_libretro PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/shared/libretro/link.T -Wl,--no-undefined)
endif()

add_executable(coretest tests/coretest.c)
target_include_directories(coretest PRIVATE shared/libretro)
target_link_libraries(coretest ${CMAKE_DL_LIBS})
