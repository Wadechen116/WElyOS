# i386:32 位元保護模式,以 multiboot 由 QEMU -kernel 載入
set(ARCH_SOURCES arch/i386/boot.S arch/i386/start.c arch/i386/arch.c
                 arch/i386/serial.c arch/i386/vga.c)
set(ARCH_FLAGS -m32 -fno-pic -fno-pie)
set(ARCH_LINK_FLAGS -m32)
