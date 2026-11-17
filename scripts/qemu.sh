#!/usr/bin/env bash
# 用法: qemu.sh <arch> <kernel.elf> [額外 QEMU 參數...]
#   arch: i386 | riscv64 | aarch64
# 自動尋找 QEMU;在 WSL 下使用 Windows 版並轉換路徑。
# 環境變數: QEMU(指定執行檔)、QEMU_DISPLAY(預設 none;i386 的 GUI 用 default)
#           QEMU_SMP(CPU 數,預設 4;kernel 最多使用 8 顆)、QEMU_CPU(CPU 型號)
set -e
ARCH="$1"; ELF="$2"; shift 2

case "$ARCH" in
  i386)
    BIN=qemu-system-i386
    # isa-debug-exit:kernel 可寫 port 0xF4 結束 QEMU(結束碼 (n<<1)|1)
    # -cpu max:提供 QEMU 能模擬的所有功能。預設的 qemu32 沒有 NX、SMEP、SMAP,
    # kernel 會偵測並退回沒有這些保護的模式
    MACHINE=(-cpu "${QEMU_CPU:-max}" -smp "${QEMU_SMP:-4}" -device isa-debug-exit,iobase=0xf4,iosize=0x04) ;;
  riscv64)
    BIN=qemu-system-riscv64
    # virt 機器內建 OpenSBI。結束由 SiFive finisher 裝置處理
    MACHINE=(-machine virt -smp "${QEMU_SMP:-4}" -m 128M -bios default) ;;
  aarch64)
    BIN=qemu-system-aarch64
    # 成功結束由 PSCI SYSTEM_OFF 處理;失敗碼靠 semihosting 回報(需 -semihosting)。
    # 載入原始二進位(.bin)而非 ELF:QEMU 才會把 DTB 位址放進 x0
    ELF="${ELF%.elf}.bin"
    # -cpu max:提供 QEMU 能模擬的所有功能(PAN 需要 ARMv8.1;cortex-a57 沒有)
    MACHINE=(-machine virt -cpu "${QEMU_CPU:-max}" -smp "${QEMU_SMP:-4}" -m 128M -semihosting) ;;
  *) echo "未知架構: $ARCH" >&2; exit 1 ;;
esac

QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
  if command -v "$BIN" >/dev/null; then QEMU="$BIN"
  elif [ -x "/mnt/c/Program Files/qemu/$BIN.exe" ]; then
    QEMU="/mnt/c/Program Files/qemu/$BIN.exe"
  else echo "找不到 $BIN,請設定環境變數 QEMU" >&2; exit 1; fi
fi
case "$QEMU" in *.exe) ELF="$(wslpath -w "$ELF")";; esac

set +e
"$QEMU" "${MACHINE[@]}" -kernel "$ELF" -serial stdio \
        -display "${QEMU_DISPLAY:-none}" -no-reboot "$@"
rc=$?
# i386 的 isa-debug-exit 寫入 0 時,QEMU 回傳 1,視為成功
[ "$ARCH" = i386 ] && [ "$rc" -eq 1 ] && exit 0
exit $rc
