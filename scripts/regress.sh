#!/usr/bin/env bash
# 回歸測試:建置三個架構,逐一執行各種開機參數,核對 QEMU 的結束碼。
#
# 用法: bash scripts/regress.sh [選項]
#   (不帶選項)   每個架構跑完整的 24 個情境(含效能基準 bench:只確認它能跑完,數字不做門檻)
#   -r N          另外重複執行「全部測試」N 次,分別以 1、4、8 顆 CPU(找偶發的錯誤)
#   -a "架構..."  只測指定的架構,例如 -a "riscv64"
#   -q            只跑最基本的四個情境(只開機、selftest、全部測試、bench)
# 環境變數 QEMU_SMP 決定 23 個情境使用的 CPU 數(預設 4)。
# 另外每個架構做一次「磁碟跨開機測試」(15 次開機,使用獨立的映像檔;-q 時略過)。
# 23 個情境使用預設的磁碟映像 build/<架構>/disk.img(測試結束時會把自己建立的東西刪掉)。
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
CRASH="panic fault illegal break dfree stack mainstack thread relock null wtext exec uaccess uexec spin spinsleep lockorder mxorder smp wdog"
fail=0
. scripts/exit5-class.sh
hostcrash=0                  # 已證實是模擬器結束時當掉、重試後通過的次數

for a in $ARCHS; do
  # A failed configure/build must never fall through to an old ELF.
  if ! cmake --preset "$a" > "$OUT/$a-configure.txt" 2>&1; then
    echo "$a: configure failed; log: $OUT/$a-configure.txt"
    cat "$OUT/$a-configure.txt"
    fail=1
    continue
  fi
  if ! cmake --build --preset "$a" > "$OUT/$a-build.txt" 2>&1; then
    echo "$a: build failed; log: $OUT/$a-build.txt"
    cat "$OUT/$a-build.txt"
    fail=1
    continue
  fi
  grep -E " error |warning:" "$OUT/$a-build.txt" || true
  [ -s "build/$a/welyos.elf" ] || { echo "$a: missing/empty ELF after build"; fail=1; continue; }
  if [ "$a" = aarch64 ] && [ ! -s "build/$a/welyos.bin" ]; then
    echo "$a: missing/empty binary after build"; fail=1; continue
  fi

  cases=("" "selftest" "selftest memtest stress" "bench")
  if [ "$QUICK" -eq 0 ]; then
    cases+=("memhog=file" "memhog=fork")      # I-240:使用者把記憶體用到被拒絕,kernel 保留底線守住
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
    # 當機情境一律記錄 Windows 行程完整的 32 位元結束碼(每次多約 0.3 秒):
    # 「模擬器以結束碼 5 結束」至今只出現在這類情境,而且約七百次才一次,專門去重現(500 次)沒有遇到。
    # 讓它下次出現時自己留下證據(見下方與 docs/ISSUES.md 的 N-002)
    wx=0; case "$c" in crash=*) wx=1 ;; esac
    QEMU_WINEXIT=$wx timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" -append "autoexit $c" > "$log" 2>&1 < /dev/null
    rc=$?
    t=$(echo "$(date +%s.%N) - $t0" | bc)
    # 結束碼 5(docs/ISSUES.md N-002(a))。第 51 步的追查(1400 次當機情境)抓到兩次,都在 aarch64:
    # kernel 印出「exit: code 1」(給的值正確),Windows 行程的完整結束碼卻是 0xC0000005(存取違規)—
    # 是 Windows 版 QEMU 在結束時自己當掉,最低位元組 5 被 WSL 看成「結束碼 5」。
    # 判定規則:只有「kernel 確實印出了預期的值」而且「完整結束碼是 0xC0000005」兩者同時成立,
    # 才認定是模擬器的問題:保留證據、重試一次,重試符合預期就算通過並在最後列出次數。
    # 其他任何結束碼 5(例如 i386 的 kernel 寫了 2,或沒有記到完整結束碼)一律算失敗。
    if [ "$rc" -eq 5 ] && [ "$want" -ne 5 ]; then
      win=$(grep -ao 'WIN_EXIT=[-0-9]* (0x[0-9A-F]*)' "$log" | tail -1 || true)
      cp "$log" "$log.exit5"
      mkdir -p build/exit5-hunt && cp "$log" "build/exit5-hunt/regress-$(date +%Y%m%d-%H%M%S)-$a-${c// /+}.txt"
      if exit5_is_host_crash "$log"; then
        echo "  注意:$a ${c:-(只開機)}:kernel 給了正確的值,模擬器結束時當掉($win),重試一次(輸出保留在 $log.exit5)"
        QEMU_WINEXIT=1 timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" -append "autoexit $c" > "$log" 2>&1 < /dev/null
        rc=$?
        [ "$rc" -eq "$want" ] && hostcrash=$((hostcrash + 1))
      else
        echo "  $a ${c:-(只開機)}:結束碼 5 但不符合「模擬器當掉」的條件($win),算失敗;輸出保留在 $log.exit5"
        fail=1
      fi
    fi
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
    for s in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 16 15; do
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
    echo "$a: 磁碟跨開機測試 ${dok}/16 個階段通過"

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

    # 不是我們的磁碟:整顆填滿外來的資料(0xA5)。開機後必須一個位元組都沒有被改到 —
    # 不掛載、不格式化,黑盒子也不可以把紀錄寫在它最後的 8KB(Codex 找到的問題:第 113 號留言)
    python3 -c "import sys; open(sys.argv[1],'wb').write(b'\\xa5' * (16 << 20))" "$img"
    before=$(md5sum < "$img")
    QEMU_DISK="$img" timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" \
         -append "autoexit" > "$OUT/$a-foreign.txt" 2>&1 < /dev/null
    frc=$?
    if [ "$frc" -eq 0 ] && [ "$(md5sum < "$img")" = "$before" ] && grep -q "blackbox: disabled" "$OUT/$a-foreign.txt"; then
      echo "$a: 外來的磁碟沒有被寫入"
    else
      fail=1; echo "  $a 外來磁碟測試失敗(結束碼 $frc),輸出在 $OUT/$a-foreign.txt"
    fi
    rm -f "$img"

    # 沒有磁碟、也沒有網路卡:每個驅動程式都必須正確回報「沒有」,自我測試照樣通過。
    # (第 48 步以前只在 RISC-V 上跑過這個組合,i386 的 NE2000 偵測因此把空的匯流排當成一張卡)
    QEMU_DISK=none QEMU_NET=none timeout 60 scripts/qemu.sh "$a" "build/$a/welyos.elf" \
         -append "autoexit selftest" > "$OUT/$a-bare.txt" 2>&1 < /dev/null
    brc=$?
    if [ "$brc" -eq 0 ] && grep -q "^net: none" "$OUT/$a-bare.txt" && grep -q "^disk: none" "$OUT/$a-bare.txt"; then
      echo "$a: 沒有磁碟與網路卡時正確偵測為「沒有」,自我測試通過"
    else
      fail=1; echo "  $a 無磁碟無網路測試失敗(結束碼 $brc),輸出在 $OUT/$a-bare.txt"
    fi
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

# 架構規則:模組之間的界線(誰可以存取哪一組狀態、誰可以引入哪個內部標頭),見 scripts/arch-check.sh
if [ -f scripts/arch-check.sh ]; then
  bash scripts/arch-check.sh || fail=1
fi

# 靜態檢查(約 2 秒):只看相對於「上一次提交」新增的提示,見 scripts/static-gate.sh
if [ -f scripts/static-gate.sh ]; then
  bash scripts/static-gate.sh || fail=1
fi

# Windows 版 QEMU 逾時被中止時可能殘留行程
command -v taskkill.exe >/dev/null && taskkill.exe /F /IM qemu-system-i386.exe /IM qemu-system-riscv64.exe /IM qemu-system-aarch64.exe >/dev/null 2>&1
bash scripts/test-exit5-class.sh || fail=1
[ "$hostcrash" -gt 0 ] && echo "模擬器結束時當掉(0xC0000005,kernel 給的值正確)後重試通過:$hostcrash 次(證據在 build/exit5-hunt/)"
[ "$fail" -eq 0 ] && echo "全部符合預期" || echo "有不符合預期的結果"
exit $fail
