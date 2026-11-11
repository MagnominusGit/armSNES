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
	src/system.c
)

add_executable(snesrun tests/snesrun.c)
target_link_libraries(snesrun armsnes)
