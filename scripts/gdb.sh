#!/usr/bin/env bash
# 用法: gdb.sh <arch> <kernel.elf> [額外 gdb 參數...]
#   先在另一個終端機執行 `cmake --build build/<arch> --target debug`,
#   (QEMU 會暫停並等待連線),再執行本腳本。
set -e
ARCH="$1"; ELF="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"

# 需要能處理多種架構的 gdb
GDB="${GDB:-}"
if [ -z "$GDB" ]; then
  for c in gdb-multiarch gdb; do command -v "$c" >/dev/null && GDB="$c" && break; done
fi
[ -n "$GDB" ] || { echo "找不到 gdb-multiarch(sudo apt install gdb-multiarch)" >&2; exit 1; }

case "$ARCH" in
  i386)    GARCH=i386 ;;
  riscv64) GARCH=riscv:rv64 ;;
  aarch64) GARCH=aarch64 ;;
  *) echo "未知架構: $ARCH" >&2; exit 1 ;;
esac

# QEMU 若是 Windows 版(.exe,WSL 內找不到原生版)且 WSL 為 NAT 模式,
# 要連 Windows 主機的位址,而非 127.0.0.1。mirrored 模式或原生 QEMU 則用 127.0.0.1。
HOST="${GDB_HOST:-127.0.0.1}"
if [ -z "${GDB_HOST:-}" ] && ! command -v "qemu-system-$ARCH" >/dev/null 2>&1 \
   && grep -qi microsoft /proc/version 2>/dev/null; then
  MODE="$(wslinfo --networking-mode 2>/dev/null || echo nat)"
  [ "$MODE" = nat ] && HOST="$(ip route | awk '/default/{print $3; exit}')"
fi
PORT="${GDB_PORT:-1234}"

exec "$GDB" -q -x "$HERE/gdbinit" \
     -ex "set architecture $GARCH" \
     -ex "file $ELF" \
     -ex "target remote $HOST:$PORT" \
     "$@"
