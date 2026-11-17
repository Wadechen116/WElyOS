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
    # 開機韌體 OpenSBI:使用專案內自行建置、開機標誌為 WElyOS 的版本
    # (firmware/opensbi-riscv64-welyos.bin,由 scripts/build-opensbi.sh 產生)。
    # 找不到該檔案,或設定 QEMU_BIOS=default 時,改用 QEMU 內附的版本。
    # 結束由 SiFive finisher 裝置處理
    BIOS="${QEMU_BIOS:-$(dirname "$0")/../firmware/opensbi-riscv64-welyos.bin}"
    [ "$BIOS" != default ] && [ ! -f "$BIOS" ] && BIOS=default
    MACHINE=(-machine virt -smp "${QEMU_SMP:-4}" -m 128M -bios "$BIOS") ;;
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
case "$QEMU" in *.exe)
  ELF="$(wslpath -w "$ELF")"
  # Windows 版的 QEMU 看不懂 WSL 的路徑:韌體檔的路徑也要轉換
  for i in "${!MACHINE[@]}"; do
    if [ "${MACHINE[$i]}" = -bios ] && [ "${MACHINE[$((i+1))]}" != default ]; then
      MACHINE[$((i+1))]="$(wslpath -w "${MACHINE[$((i+1))]}")"
    fi
  done ;;
esac

# 從終端機直接執行時(標準輸入是終端機):關掉終端機自己的「整行緩衝」與「回顯」。
# 否則打的字要等按下 Enter 才一次送進去(一長串同時到達,客體來不及收會掉字),
# 而且每個字會顯示兩次(終端機顯示一次、WElyOS 自己再回顯一次)。
# 保留 Ctrl-C 的作用(結束 QEMU)。結束時還原終端機的設定。
if [ -t 0 ]; then
  STTY_SAVED="$(stty -g)"
  trap 'stty "$STTY_SAVED" 2>/dev/null' EXIT
  stty -icanon -echo
fi

set +e
"$QEMU" "${MACHINE[@]}" -kernel "$ELF" -serial stdio \
        -display "${QEMU_DISPLAY:-none}" -no-reboot "$@"
rc=$?
# i386 的 isa-debug-exit 寫入 0 時,QEMU 回傳 1,視為成功
[ "$ARCH" = i386 ] && [ "$rc" -eq 1 ] && exit 0
exit $rc
