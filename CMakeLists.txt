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

# ---- QEMU 目標 ----
set(QEMU_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/qemu.sh)
# run:直接執行,序列埠輸出到終端機(Ctrl+C 結束)
add_custom_target(run
    COMMAND bash ${QEMU_SCRIPT} $<TARGET_FILE:welyos.elf>
    DEPENDS welyos.elf USES_TERMINAL)
# debug:開 GDB stub(tcp::1234)並在第一條指令前暫停,等待 gdb 連線
add_custom_target(debug
    COMMAND bash ${QEMU_SCRIPT} $<TARGET_FILE:welyos.elf> -s -S
    DEPENDS welyos.elf USES_TERMINAL)
