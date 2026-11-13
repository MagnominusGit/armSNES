cmake_minimum_required(VERSION 3.13)
project(armSNES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -fwrapv)

# The Super Game Boy's Game Boy: Gambatte-Speedrun's core (gambatte/, GPL-2.0), as armGBA has it.
file(GLOB_RECURSE GAMBATTE_SOURCES gambatte/libgambatte/src/*.cpp)
list(FILTER GAMBATTE_SOURCES EXCLUDE REGEX "/(file_zip|cinterface)\\.cpp$|/unzip/")
add_library(gambatte STATIC ${GAMBATTE_SOURCES})
target_include_directories(gambatte PUBLIC gambatte/libgambatte/include PRIVATE gambatte/libgambatte/src gambatte/common)
target_compile_definitions(gambatte PUBLIC HAVE_STDINT_H)
target_compile_options(gambatte PRIVATE -w -O2 -fno-exceptions -fno-rtti)
set_target_properties(gambatte PROPERTIES CXX_STANDARD 11)

add_library(armsnes STATIC
	src/bus.c
	src/cart.c
	src/cpu.c
	src/dsp.c
	src/input.c
	src/mix.c
	src/sha256.c
	src/ppu.c
	src/smp.c
	src/state.c
	src/system.c
	src/chips/necdsp.c
	src/chips/sa1.c
	src/chips/gsu.c
	src/chips/cx4.c
	src/chips/spc7110.c
	src/chips/rtc.c
	src/chips/st018.c
	src/chips/bsx.c
	src/chips/sufami.c
	src/chips/icd.c
	src/chips/msu1.c
	src/chips/competition.c
	src/chips/pirate.c
	src/chips/gb.cpp
	src/chips/obc1.c
	src/chips/sdd1.c
)

target_link_libraries(armsnes gambatte)

add_executable(snesrun tests/snesrun.c)
target_link_libraries(snesrun armsnes)

# snesrun-dot: the PPU drawing one dot at a time, the reference the spans are checked against.
add_library(armsnes-dot STATIC EXCLUDE_FROM_ALL src/bus.c src/cart.c src/cpu.c src/dsp.c src/input.c src/mix.c src/sha256.c src/ppu.c src/smp.c src/state.c src/system.c src/chips/necdsp.c src/chips/sa1.c src/chips/gsu.c src/chips/cx4.c src/chips/spc7110.c src/chips/rtc.c src/chips/st018.c src/chips/bsx.c src/chips/sufami.c src/chips/icd.c src/chips/msu1.c src/chips/competition.c src/chips/pirate.c src/chips/gb.cpp src/chips/obc1.c src/chips/sdd1.c)
target_compile_definitions(armsnes-dot PRIVATE ARMSNES_DOT)
target_link_libraries(armsnes-dot gambatte)
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
