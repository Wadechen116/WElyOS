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
# 最佳化等級。預設 -O2;要用 GDB 逐行追蹤時,未最佳化的程式比較好對照原始碼,
# 可改用:cmake --preset <架構> -DWELYOS_OPT=-O0
set(WELYOS_OPT "-O2" CACHE STRING "編譯器最佳化等級,例如 -O2、-Og、-O0")
add_compile_options(${WELYOS_OPT})

# 執行佇列的實作(見 include/runq.h):
#   ring  每顆 CPU 一組無鎖環狀佇列,加上工作竊取(預設)
#   list  全系統共用一組串列,以一把鎖保護
# 可改用:cmake --preset <架構> -DWELYOS_RUNQ=list
set(WELYOS_RUNQ "ring" CACHE STRING "執行佇列的實作:ring 或 list")
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/kernel/runq_${WELYOS_RUNQ}.c)
    message(FATAL_ERROR "未知的 WELYOS_RUNQ: ${WELYOS_RUNQ}(kernel/runq_${WELYOS_RUNQ}.c 不存在)")
endif()

add_compile_options(${ARCH_FLAGS} -ffreestanding -fno-stack-protector -fno-builtin
                    -fno-tree-loop-distribute-patterns -fno-omit-frame-pointer -nostdlib -Wall -Wextra -g)
# 連結選項分成共用的部分與各自的 linker script:kernel 與使用者程式是兩個獨立的執行檔
set(COMMON_LINK_FLAGS ${ARCH_LINK_FLAGS} -ffreestanding -nostdlib -static -no-pie
                      -Wl,--no-warn-rwx-segments -Wl,--build-id=none)

# ---- 使用者程式 ----
# 獨立連結在 USER_BASE(各架構的使用者位址範圍起點),再轉成原始二進位,
# 由 kernel/userimg.S 以 .incbin 嵌入 kernel。
add_executable(init.elf user/init.c)
target_link_options(init.elf PRIVATE ${COMMON_LINK_FLAGS}
    -T ${CMAKE_SOURCE_DIR}/user/user.ld -Wl,--defsym=USER_BASE=${ARCH_USER_BASE})
set_target_properties(init.elf PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/user/user.ld)
set(USER_IMG ${CMAKE_BINARY_DIR}/init.bin)
add_custom_command(OUTPUT ${USER_IMG}
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:init.elf> ${USER_IMG}
    DEPENDS init.elf
    COMMENT "產生使用者程式映像 init.bin")
add_custom_target(user_image DEPENDS ${USER_IMG})

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
    kernel/runq_${WELYOS_RUNQ}.c
    kernel/spinlock.c
    kernel/smp.c
    kernel/sync.c
    kernel/vm.c
    kernel/proc.c
    kernel/userimg.S
    lib/string.c)
target_link_options(welyos.elf PRIVATE ${COMMON_LINK_FLAGS}
    -T ${CMAKE_SOURCE_DIR}/arch/${ARCH}/linker.ld)
# userimg.S 內嵌 init.bin:映像改變時要重新組譯
add_dependencies(welyos.elf user_image)
set_source_files_properties(kernel/userimg.S PROPERTIES
    COMPILE_DEFINITIONS "USER_IMG_PATH=\"${USER_IMG}\""
    OBJECT_DEPENDS ${USER_IMG})
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
# run:直接執行,序列埠輸出到終端機;autoexit 讓 kernel 結束時關閉 QEMU,
# selftest 執行各子系統的自我測試與示範
add_custom_target(run
    COMMAND bash ${QEMU_SCRIPT} ${ARCH} $<TARGET_FILE:welyos.elf> -append "autoexit selftest"
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
