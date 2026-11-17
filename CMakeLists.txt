cmake_minimum_required(VERSION 3.20)

# 以 -DARCH=i386|riscv64|aarch64 選擇目標架構(建議使用 CMakePresets.json)。
# toolchain 檔必須在 project() 之前決定,所以未指定時依 ARCH 自動帶入。
set(ARCH i386 CACHE STRING "目標架構: i386 | riscv64 | aarch64")
set_property(CACHE ARCH PROPERTY STRINGS i386 riscv64 aarch64)
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-${ARCH}.cmake)
endif()

project(WElyOS C ASM)

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/arch/${ARCH}/arch.cmake)
    message(FATAL_ERROR "不支援的架構: ${ARCH}")
endif()
include(arch/${ARCH}/arch.cmake)   # 設定 ARCH_SOURCES / ARCH_FLAGS / ARCH_LINK_FLAGS

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# freestanding:不假設有 OS 或 libc。
# -fno-tree-loop-distribute-patterns:避免 lib/string.c 的迴圈被改寫成自我呼叫
# -fno-omit-frame-pointer:保留 frame pointer 鏈,backtrace 才能回溯
add_compile_options(${ARCH_FLAGS} -ffreestanding -fno-stack-protector -fno-builtin
                    -fno-tree-loop-distribute-patterns -fno-omit-frame-pointer -nostdlib -Wall -Wextra -g)
add_link_options(${ARCH_LINK_FLAGS} -ffreestanding -nostdlib -static -no-pie
                 -Wl,--no-warn-rwx-segments
                 -T ${CMAKE_SOURCE_DIR}/arch/${ARCH}/linker.ld)

add_executable(welyos.elf
    ${ARCH_SOURCES}
    kernel/main.c
    kernel/kprintf.c
    kernel/fdt.c
    kernel/panic.c
    kernel/timer.c
    kernel/pmm.c
    kernel/kmalloc.c
    kernel/sched.c
    kernel/sync.c
    kernel/vm.c
    lib/string.c)
target_include_directories(welyos.elf PRIVATE include arch/${ARCH})
set_target_properties(welyos.elf PROPERTIES LINK_DEPENDS
    ${CMAKE_SOURCE_DIR}/arch/${ARCH}/linker.ld)

# 需要原始二進位映像的架構(見 arch/<名稱>/arch.cmake)
if(ARCH_RAW_BINARY)
    add_custom_command(TARGET welyos.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:welyos.elf>
                $<TARGET_FILE_DIR:welyos.elf>/welyos.bin
        COMMENT "產生原始二進位 welyos.bin")
endif()

# ---- QEMU 目標 ----
set(QEMU_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/qemu.sh)
# run:直接執行,序列埠輸出到終端機;帶 autoexit 讓 kernel 結束時關閉 QEMU
add_custom_target(run
    COMMAND bash ${QEMU_SCRIPT} ${ARCH} $<TARGET_FILE:welyos.elf> -append autoexit
    DEPENDS welyos.elf USES_TERMINAL)
# debug:開 GDB stub(tcp::1234)並在第一條指令前暫停,等待 gdb 連線
add_custom_target(debug
    COMMAND bash ${QEMU_SCRIPT} ${ARCH} $<TARGET_FILE:welyos.elf> -s -S
    DEPENDS welyos.elf USES_TERMINAL)
# run-gui:開啟 QEMU 視窗顯示 VGA 畫面,kernel 持續運行(僅 i386 有 VGA)
if(ARCH STREQUAL "i386")
    add_custom_target(run-gui
        COMMAND ${CMAKE_COMMAND} -E env QEMU_DISPLAY=default
                bash ${QEMU_SCRIPT} ${ARCH} $<TARGET_FILE:welyos.elf>
        DEPENDS welyos.elf USES_TERMINAL)
endif()
# gdb:連到 debug target 啟動的 QEMU(需另一個終端機先執行 debug)
add_custom_target(gdb
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/gdb.sh ${ARCH} $<TARGET_FILE:welyos.elf>
    DEPENDS welyos.elf USES_TERMINAL)
