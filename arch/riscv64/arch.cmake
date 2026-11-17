# RISC-V 64:rv64imac,不使用浮點;medany 允許程式放在高位址(0x80200000);
# -mno-relax 避免連結器使用未設定的 gp 暫存器做最佳化
set(ARCH_SOURCES arch/riscv64/boot.S arch/riscv64/trap.S arch/riscv64/switch.S arch/riscv64/start.c
                 arch/riscv64/arch.c arch/riscv64/trap.c arch/riscv64/timer.c arch/riscv64/mmu.c)
set(ARCH_FLAGS -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -mno-relax
               -fno-pic -fno-pie)
set(ARCH_LINK_FLAGS -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -Wl,--no-relax)
