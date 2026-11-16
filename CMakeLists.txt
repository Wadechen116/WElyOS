cmake_minimum_required(VERSION 3.20)
project(WElyOS C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(ARCH_FLAGS -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector
               -fno-builtin -nostdlib)
add_compile_options(${ARCH_FLAGS} -Wall -Wextra -g)
add_link_options(${ARCH_FLAGS} -static -no-pie -Wl,--no-warn-rwx-segments
                 -T ${CMAKE_SOURCE_DIR}/arch/i386/linker.ld)

add_executable(welyos.elf
    arch/i386/boot.S
    kernel/main.c)
set_target_properties(welyos.elf PROPERTIES LINK_DEPENDS
    ${CMAKE_SOURCE_DIR}/arch/i386/linker.ld)
