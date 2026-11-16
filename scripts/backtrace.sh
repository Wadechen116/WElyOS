#!/usr/bin/env bash
# 用法: backtrace.sh <arch> <kernel.elf> < 含 backtrace 的輸出
# 把 "#N ra=0x..." / "#N pc=0x..." 的位址轉成「函式 at 檔案:行號」。
#   pc  是出錯的指令位址,直接查詢。
#   ra  是返回位址(呼叫指令的下一條),要減 1 才會落在呼叫者那一行;
#       若呼叫的是不會返回的函式,不減 1 會誤判成下一個函式。
set -e
case "$1" in
  i386)    A2L="${ADDR2LINE:-addr2line}" ;;
  riscv64) A2L="${ADDR2LINE:-riscv64-unknown-elf-addr2line}" ;;
  aarch64) A2L="${ADDR2LINE:-aarch64-linux-gnu-addr2line}" ;;
  *) echo "未知架構: $1" >&2; exit 1 ;;
esac
ELF="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sed -n -E 's/.*#([0-9]+) (pc|ra)=(0x[0-9a-fA-F]+).*/\1 \2 \3/p' | tr -d '\r' |
while read -r n kind addr; do
  [ "$kind" = ra ] && addr=$(printf '0x%x' $((addr - 1)))
  loc=$("$A2L" -f -p -e "$ELF" "$addr" | sed "s|$ROOT/||")
  printf '  #%s %s\n' "$n" "$loc"
done
