# i386:32 位元保護模式,以 multiboot 由 QEMU -kernel 載入
set(ARCH_SOURCES arch/i386/boot.S arch/i386/trap.S arch/i386/start.c
                 arch/i386/arch.c arch/i386/trap.c arch/i386/timer.c
                 arch/i386/serial.c arch/i386/vga.c)
# -mgeneral-regs-only:kernel 不使用 x87/SSE(尚未初始化 FPU,也避免例外入口要存浮點狀態)
set(ARCH_FLAGS -m32 -mgeneral-regs-only -fno-pic -fno-pie)
set(ARCH_LINK_FLAGS -m32)
