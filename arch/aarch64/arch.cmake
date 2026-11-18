# AArch64:MMU 關閉時需嚴格對齊(-mstrict-align),且不使用浮點/SIMD;
# -mno-omit-leaf-frame-pointer:葉函式也建立 frame record,backtrace 才不會少一層
set(ARCH_SOURCES arch/aarch64/boot.S arch/aarch64/trap.S arch/aarch64/switch.S arch/aarch64/start.c
                 arch/aarch64/arch.c arch/aarch64/trap.c arch/aarch64/timer.c arch/aarch64/mmu.c
                 arch/aarch64/smp.c kernel/virtio_blk.c kernel/virtio_net.c)
set(ARCH_FLAGS -mgeneral-regs-only -mstrict-align -mcmodel=small
               -mno-omit-leaf-frame-pointer -mno-outline-atomics
               -fno-pic -fno-pie)
set(ARCH_LINK_FLAGS "")

# QEMU 只在載入「非 ELF」kernel 時才依 Linux 啟動協定把 DTB 位址放進 x0(ELF 則不給),
# 所以建置後以 objcopy 另外產生 welyos.bin 供 QEMU 載入。ELF 仍保留,供 GDB 讀符號。
set(ARCH_RAW_BINARY ON)
set(ARCH_USER_BASE 0x100000000)  # 使用者位址範圍的起點,須與 mmu.h 的 USER_BASE 一致
