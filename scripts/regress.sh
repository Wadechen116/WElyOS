#!/usr/bin/env bash
# 回歸測試:建置三個架構,逐一執行各種開機參數,核對 QEMU 的結束碼。
#
# 用法: bash scripts/regress.sh [選項]
#   (不帶選項)   每個架構跑完整的 21 個情境
#   -r N          另外重複執行「全部測試」N 次,分別以 1、4、8 顆 CPU(找偶發的錯誤)
#   -a "架構..."  只測指定的架構,例如 -a "riscv64"
#   -q            只跑最基本的三個情境(只開機、selftest、全部測試)
# 環境變數 QEMU_SMP 決定 20 個情境使用的 CPU 數(預設 4)。
# 另外每個架構做一次「磁碟跨開機測試」(九次開機,使用獨立的映像檔;-q 時略過)。
# 20 個情境使用預設的磁碟映像 build/<架構>/disk.img(測試結束時會把自己建立的東西刪掉)。
#
# 預期的結束碼:
#   正常結束                    0
#   故意觸發的錯誤(crash=...)  i386 為 3,riscv64 / aarch64 為 1
#   crash=break(可恢復的例外)  0
# 任何不符合預期的結果都會列出來,腳本最後以非 0 結束。
cd "$(dirname "$0")/.."
ARCHS="i386 riscv64 aarch64"
REPEAT=0
QUICK=0
while getopts "r:a:q" opt; do
  case $opt in
    r) REPEAT=$OPTARG ;;
    a) ARCHS=$OPTARG ;;
    q) QUICK=1 ;;
    *) exit 2 ;;
  esac
done

OUT=build/regress; mkdir -p "$OUT"
CRASH="panic fault illegal break dfree stack mainstack thread relock null wtext exec uaccess uexec spin spinsleep smp wdog"
fail=0

for a in $ARCHS; do
  if ! cmake --preset "$a" >/dev/null 2>&1 || ! cmake --build --preset "$a" 2>&1 | grep -E " error |warning:" ; [ "${PIPESTATUS[0]}" -ne 0 ]; then
    : # 建置訊息已在上面列出(沒有輸出代表沒有錯誤與警告)
  fi
  [ -f "build/$a/welyos.elf" ] || { echo "$a: 建置失敗"; fail=1; continue; }

  cases=("" "selftest" "selftest memtest stress")
  if [ "$QUICK" -eq 0 ]; then
    for c in $CRASH; do cases+=("crash=$c"); done
  fi
  ok=0
  for c in "${cases[@]}"; do
    want=0
    case "$c" in
      crash=break) want=0 ;;
      crash=*) [ "$a" = i386 ] && want=3 || want=1 ;;
    esac
    log="$OUT/$a-${c// /+}.txt"; [ -z "$c" ] && log="$OUT/$a-boot.txt"
    t0=$(date +%s.%N)
    timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" -append "autoexit $c" > "$log" 2>&1 < /dev/null
    rc=$?
    t=$(echo "$(date +%s.%N) - $t0" | bc)
    if [ "$rc" -eq "$want" ]; then
      ok=$((ok + 1))
      case "$c" in ""|selftest|"selftest memtest stress") printf "  %-8s %-26s %.2f 秒\n" "$a" "${c:-(只開機)}" "$t" ;; esac
    else
      fail=1
      printf "  %-8s %-26s 結束碼 %s(預期 %s),輸出在 %s\n" "$a" "${c:-(只開機)}" "$rc" "$want" "$log"
    fi
  done
  echo "$a: ${ok}/${#cases[@]} 個情境符合預期"

  # 磁碟:用一個全新的映像檔連續開機五次(寫入 -> 重開機確認還在並故意弄壞 -> 確認開機時修復了
  # -> 故意讓磁碟讀寫失敗 -> 確認之後的開機檢查過、資料完好)。
  # 見 kernel/main.c 的 disk_stage_test
  if [ "$QUICK" -eq 0 ]; then
    img="$OUT/disk-$a.img"; rm -f "$img"; dok=0
    for s in 1 2 3 4 5 6 7 8 9 10 11; do
      log="$OUT/$a-disktest$s.txt"
      # 第 6 階段故意 panic(測試黑盒子):預期的結束碼和其他當機情境相同
      dwant=0; [ "$s" -eq 6 ] && { [ "$a" = i386 ] && dwant=3 || dwant=1; }
      QEMU_DISK="$img" timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" \
           -append "autoexit disktest=$s" > "$log" 2>&1 < /dev/null
      if [ "$?" -eq "$dwant" ]; then
        dok=$((dok + 1))
      else
        fail=1; echo "  $a 磁碟測試第 $s 階段失敗,輸出在 $log"
      fi
    done
    rm -f "$img"
    echo "$a: 磁碟跨開機測試 ${dok}/11 個階段通過"

    # 安全模式:連續三次開機失敗(故意在開機途中 panic),第四次應該自動以安全模式開機;
    # 在安全模式裡正常結束之後,第五次回到一般的開機
    rm -f "$img"; sok=0; pwant=1; [ "$a" = i386 ] && pwant=3
    for step in "crash=boot:$pwant" "crash=boot:$pwant" "crash=boot:$pwant" "expect=safe:0" "expect=normal:0"; do
      QEMU_DISK="$img" timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" \
           -append "autoexit ${step%%:*}" > "$OUT/$a-safemode.txt" 2>&1 < /dev/null
      if [ "$?" -eq "${step##*:}" ]; then
        sok=$((sok + 1))
      else
        fail=1; echo "  $a 安全模式測試在「${step%%:*}」失敗,輸出在 $OUT/$a-safemode.txt"; break
      fi
    done
    rm -f "$img"
    echo "$a: 安全模式測試 ${sok}/5 次開機符合預期"
  fi

  if [ "$REPEAT" -gt 0 ]; then
    for n in 1 4 8; do
      good=0
      for i in $(seq "$REPEAT"); do
        log="$OUT/$a-repeat-smp$n-$i.txt"
        if QEMU_SMP=$n timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" \
             -append "autoexit selftest stress memtest" > "$log" 2>&1 < /dev/null; then
          good=$((good + 1)); rm -f "$log"
        else
          fail=1; echo "  $a,$n 顆 CPU,第 $i 次失敗,輸出在 $log"
        fi
      done
      echo "$a: ${n} 顆 CPU 重複 ${good}/${REPEAT} 次通過"
    done
  fi
done

# 網路的協定堆疊:主機端的測試(不需要 QEMU;-q 時略過)
if [ "$QUICK" -eq 0 ] && [ -f scripts/host-test-net.sh ]; then
  if bash scripts/host-test-net.sh > "$OUT/host-net.txt" 2>&1; then
    echo "網路堆疊主機測試:$(grep -o 'net: PASS.*' "$OUT/host-net.txt" | head -1)"
  else
    fail=1; echo "  網路堆疊主機測試失敗,輸出在 $OUT/host-net.txt"
  fi
fi

# 靜態檢查(約 2 秒):只看相對於「上一次提交」新增的提示,見 scripts/static-gate.sh
if [ -f scripts/static-gate.sh ]; then
  bash scripts/static-gate.sh || fail=1
fi

# Windows 版 QEMU 逾時被中止時可能殘留行程
command -v taskkill.exe >/dev/null && taskkill.exe /F /IM qemu-system-i386.exe /IM qemu-system-riscv64.exe /IM qemu-system-aarch64.exe >/dev/null 2>&1
[ "$fail" -eq 0 ] && echo "全部符合預期" || echo "有不符合預期的結果"
exit $fail
