#!/bin/bash
# bench.sh - 效能基準(docs/ISSUES.md 的 N-004):每個架構以 1 顆與 4 顆 CPU 各開機一次,執行 /bin/bench
#
# 用法:scripts/bench.sh [-a "架構..."]
#   -a  只測指定的架構(預設 i386 riscv64 aarch64)
# /bin/bench 每項跑 5 輪(kernel 以預設參數啟動它),印出最小 / 中位數 / 最大值。
# 輸出:每個組合一段 bench: 開頭的結果;完整紀錄在 build/bench/<架構>-smp<N>.txt
# 這些是 QEMU 的數字,只適合同一台主機上前後比較,不代表實體硬體的速度。
set -u
cd "$(dirname "$0")/.."
ARCHS="i386 riscv64 aarch64"
while getopts "a:" o; do
  case "$o" in a) ARCHS="$OPTARG" ;; *) exit 2 ;; esac
done
OUT=build/bench; mkdir -p "$OUT"
fail=0
echo "host: $(uname -srm); $(qemu-system-x86_64.exe --version 2>/dev/null | head -1)"
for a in $ARCHS; do
  cmake --preset "$a" > "$OUT/$a-configure.txt" 2>&1 && cmake --build --preset "$a" > "$OUT/$a-build.txt" 2>&1 \
    || { echo "$a: build failed (see $OUT)"; fail=1; continue; }
  for n in 1 4; do
    log="$OUT/$a-smp$n.txt"
    QEMU_SMP=$n timeout 300 scripts/qemu.sh "$a" "build/$a/welyos.elf" -append "autoexit bench" > "$log" 2>&1 < /dev/null
    rc=$?
    echo "== $a, $n cpu(s), exit $rc"
    grep -a '^bench:' "$log" | sed 's/^/  /'
    [ "$rc" -eq 0 ] && grep -aq '^bench: exit 0' "$log" || { fail=1; echo "  $a,$n: FAILED (log $log)"; }
  done
done
exit $fail
