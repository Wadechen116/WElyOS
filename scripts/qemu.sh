#!/usr/bin/env bash
# 用法: qemu.sh <kernel.elf> [額外 QEMU 參數...]
# 自動尋找 qemu-system-i386;在 WSL 下使用 Windows 版並轉換路徑。
set -e
ELF="$1"; shift
QEMU="${QEMU:-}"
if [ -z "$QEMU" ]; then
  if command -v qemu-system-i386 >/dev/null; then QEMU=qemu-system-i386
  elif [ -x "/mnt/c/Program Files/qemu/qemu-system-i386.exe" ]; then
    QEMU="/mnt/c/Program Files/qemu/qemu-system-i386.exe"
  else echo "找不到 qemu-system-i386,請設定環境變數 QEMU" >&2; exit 1; fi
fi
case "$QEMU" in *.exe) ELF="$(wslpath -w "$ELF")";; esac
set +e
"$QEMU" -kernel "$ELF" -serial stdio -display none -no-reboot \
     -device isa-debug-exit,iobase=0xf4,iosize=0x04 "$@"
rc=$?
# isa-debug-exit 寫入 0 會讓 QEMU 回傳 1,視為成功
[ "$rc" -eq 1 ] && exit 0
exit $rc
