#!/usr/bin/env bash
# 在主機(Linux)上以多條執行緒測試執行佇列的兩個實作。
# 用法: bash scripts/host-test.sh [每條執行緒的操作次數]
# 每個實作跑兩遍:一般最佳化(速度快、時序緊),以及 ThreadSanitizer
# (ThreadSanitizer 會檢查每一次記憶體存取有沒有資料競爭,慢約 10 倍)。
set -e
cd "$(dirname "$0")/.."
OUT=build/host; mkdir -p "$OUT"
N="${1:-2000000}"
rc=0
for impl in ring list; do
  SRC="tests/host/runq_test.c kernel/runq_${impl}.c"
  INC="-iquote tests/host/stub -iquote include"   # -iquote:只影響 "..." 形式,<sched.h> 仍是系統的
  gcc -std=c11 -O2 -g -Wall -Wextra -pthread $INC $SRC -o "$OUT/runq_${impl}"
  # 同優先權輪流的性質(第 50 步:作廢的號碼牌不可以讓一個工作佔兩個位置)
  gcc -std=c11 -O1 -g -Wall -Wextra -Werror $INC tests/host/runq_fair.c kernel/runq_${impl}.c -o "$OUT/runq_fair_${impl}"
  "$OUT/runq_fair_${impl}" || rc=1
  "$OUT/runq_${impl}" "$N" || rc=1          # 節點很多:大量使用溢出串列
  "$OUT/runq_${impl}" "$N" 200 || rc=1      # 節點少:幾乎只用到無鎖的環與竊取
  if gcc -std=c11 -O1 -g -fsanitize=thread -pthread $INC $SRC -o "$OUT/runq_${impl}_tsan" 2>/dev/null; then
    echo "  (ThreadSanitizer)"
    # WSL 的位址空間配置較隨機,ThreadSanitizer 需要關閉 ASLR 才能穩定啟動
    setarch "$(uname -m)" -R "$OUT/runq_${impl}_tsan" $((N / 10)) || rc=1
    setarch "$(uname -m)" -R "$OUT/runq_${impl}_tsan" $((N / 10)) 200 || rc=1
  else
    echo "  (此編譯器沒有 ThreadSanitizer,略過)"
  fi
done
exit $rc
