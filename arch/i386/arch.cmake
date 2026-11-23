# i386:32 位元保護模式,以 multiboot 由 QEMU -kernel 載入
set(ARCH_SOURCES arch/i386/boot.S arch/i386/trap.S arch/i386/switch.S arch/i386/start.c
                 arch/i386/arch.c arch/i386/trap.c arch/i386/timer.c arch/i386/mmu.c
                 arch/i386/serial.c arch/i386/vga.c arch/i386/smp.c arch/i386/hpet.c arch/i386/ap_tramp.S
                 arch/i386/ata.c arch/i386/rtc.c arch/i386/ne2k.c)
# -mgeneral-regs-only:kernel 不使用 x87/SSE(尚未初始化 FPU,也避免例外入口要存浮點狀態)
set(ARCH_FLAGS -m32 -mgeneral-regs-only -fno-pic -fno-pie)
set(ARCH_LINK_FLAGS -m32)
set(ARCH_USER_BASE 0x40000000)   # 使用者位址範圍的起點,須與 mmu.h 的 USER_BASE 一致
