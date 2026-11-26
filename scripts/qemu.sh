#!/usr/bin/env bash
# 用法: qemu.sh <arch> <kernel.elf> [額外 QEMU 參數...]
#   arch: i386 | riscv64 | aarch64
# 自動尋找 QEMU;在 WSL 下使用 Windows 版並轉換路徑。
# 環境變數: QEMU(指定執行檔)、QEMU_DISPLAY(預設 none;i386 的 GUI 用 default)
#           QEMU_SMP(CPU 數,預設 4;kernel 最多使用 8 顆)、QEMU_CPU(CPU 型號)
#           QEMU_DISK(磁碟映像檔;預設 build/<arch>/disk.img,不存在時自動建立 16MB 的空白檔;
#                     設為 none 則不接磁碟)
#           QEMU_NET(設為 none 則不接網路卡;預設接上使用者模式的網路)
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

# 磁碟:一個普通的檔案當作磁碟的內容(映像檔)。kernel 第一次看到全空白的磁碟時
# 會在上面建立檔案系統,之後每次開機都掛載在 /disk,內容跨開機保留。
#   i386             接在 IDE 第一個位置(kernel 以 ATA PIO 存取)
#   riscv64/aarch64  virtio 區塊裝置(virtio-mmio;force-legacy=false 選用現行版本的介面)
DISK="${QEMU_DISK:-$(dirname "$0")/../build/$ARCH/disk.img}"
DISK_ARGS=()
if [ "$DISK" != none ]; then
  [ -f "$DISK" ] || { mkdir -p "$(dirname "$DISK")"; truncate -s 16M "$DISK"; }
fi

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
  done
  [ "$DISK" != none ] && DISK="$(wslpath -w "$DISK")" ;;
esac
if [ "$DISK" != none ]; then
  if [ "$ARCH" = i386 ]; then
    DISK_ARGS=(-drive "file=$DISK,format=raw,if=ide,index=0,media=disk")
  else
    DISK_ARGS=(-global virtio-mmio.force-legacy=false
               -drive "file=$DISK,format=raw,if=none,id=hd0" -device virtio-blk-device,drive=hd0)
  fi
fi

# 從終端機直接執行時(標準輸入是終端機):關掉終端機自己的「整行緩衝」與「回顯」。
# 否則打的字要等按下 Enter 才一次送進去(一長串同時到達,客體來不及收會掉字),
# 而且每個字會顯示兩次(終端機顯示一次、WElyOS 自己再回顯一次)。
#
# 另外把終端機的「中斷鍵」從 Ctrl-C 改成 Ctrl-]:
#   Ctrl-C  原樣送進 WElyOS(終止正在執行的命令),不再被終端機攔下來結束 QEMU
#   Ctrl-]  結束 QEMU(WElyOS 沒有回應時的最後手段;正常的離開方式是在 shell 輸入 exit)
# Ctrl-Z 也原樣送進 WElyOS(暫停正在執行的命令);Ctrl-\ 一併交給它(目前不處理)。
# 結束時還原終端機的設定。
if [ -t 0 ]; then
  STTY_SAVED="$(stty -g)"
  trap 'stty "$STTY_SAVED" 2>/dev/null' EXIT
  stty -icanon -echo intr '^]' quit undef susp undef
fi

# 網路:QEMU 的「使用者模式網路」(不需要管理員權限,也不會動到主機的網路設定)。
# 客體看到的是一個小型的私有網路:自己是 10.0.2.15,閘道 10.0.2.2,名稱伺服器 10.0.2.3。
#   i386             NE2000(ISA 介面,I/O port 0x300):不需要 PCI 匯流排的支援
#   riscv64/aarch64  virtio 網路卡(virtio-mmio)
# 模擬器另外在閘道上提供一個 TFTP(Trivial File Transfer Protocol)伺服器,把主機的一個目錄
# 唯讀地分享給客體:預設是專案的 rootfs/(可用 QEMU_TFTP 指定別的目錄)。
# 在 WElyOS 裡用 `tftp etc/motd` 就能把檔案抓進來;自我測試也用它驗證 UDP 的收送。
NET_ARGS=()
if [ "${QEMU_NET:-user}" != none ]; then
  TFTP_DIR="${QEMU_TFTP:-$(dirname "$0")/../rootfs}"
  case "$QEMU" in *.exe) TFTP_DIR="$(wslpath -w "$TFTP_DIR")" ;; esac
  if [ "$ARCH" = i386 ]; then
    NET_ARGS=(-netdev "user,id=n0,tftp=$TFTP_DIR" -device ne2k_isa,netdev=n0,iobase=0x300,irq=9)
  else
    NET_ARGS=(-global virtio-mmio.force-legacy=false -netdev "user,id=n0,tftp=$TFTP_DIR"
              -device virtio-net-device,netdev=n0)
  fi
fi

# 診斷用:QEMU_WINEXIT=1 時(而且用的是 Windows 版的 QEMU)改由 scripts/win-exit.ps1 啟動,
# 它會把 Windows 行程完整的 32 位元結束碼印到標準錯誤(WIN_EXIT=…)。平常不需要。
LAUNCH=("$QEMU")
case "$QEMU" in *.exe)
  if [ "${QEMU_WINEXIT:-0}" = 1 ]; then
    # QEMU_WINEXIT_FILE:結束碼寫到這個獨立檔,不寫 stderr(第 73 步:和序列埠輸出寫同一個檔會交錯)。
    # 先建立空檔:wslpath 需要檔案所在的目錄存在;讀取端把空檔當成「沒有記到」,不會誤判成功
    WX=()
    if [ -n "${QEMU_WINEXIT_FILE:-}" ]; then
      : > "$QEMU_WINEXIT_FILE"
      WX=(-WinExitFile "$(wslpath -w "$QEMU_WINEXIT_FILE")")
    fi
    LAUNCH=(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$(dirname "$0")/win-exit.ps1")"
            "${WX[@]}" "$(wslpath -w "$QEMU")")
  fi ;;
esac

# QEMU_RUN_TAG:回歸測試給這次執行的專屬名稱,結束時只回收帶這個名稱的 QEMU(不影響別人的)
NAME_ARGS=()
[ -n "${QEMU_RUN_TAG:-}" ] && NAME_ARGS=(-name "$QEMU_RUN_TAG")

set +e
# QEMU_TS_FILE(I-349 R6):設定時,序列埠輸出照原樣逐行送到標準輸出,另外寫一份「每行前面加主機單調時鐘
# 秒數」的副本到這個檔(主機讀到那一行的時刻,不是客體內事件的精確時間)。結束碼仍是 QEMU 自己的。
# 沒設定時完全照舊。
if [ -n "${QEMU_TS_FILE:-}" ]; then
  "${LAUNCH[@]}" "${MACHINE[@]}" "${DISK_ARGS[@]}" "${NET_ARGS[@]}" "${NAME_ARGS[@]}" -kernel "$ELF" -serial stdio \
          -display "${QEMU_DISPLAY:-none}" -no-reboot "$@" |
    perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e '
      open(my $t, ">", $ARGV[0]) or die "cannot open $ARGV[0]\n"; shift @ARGV;
      $| = 1; my $o = select($t); $| = 1; select($o);
      while (<STDIN>) { print $_; printf $t "%.6f %s", clock_gettime(CLOCK_MONOTONIC), $_; }' "$QEMU_TS_FILE"
  rc=${PIPESTATUS[0]}
else
  "${LAUNCH[@]}" "${MACHINE[@]}" "${DISK_ARGS[@]}" "${NET_ARGS[@]}" "${NAME_ARGS[@]}" -kernel "$ELF" -serial stdio \
          -display "${QEMU_DISPLAY:-none}" -no-reboot "$@"
  rc=$?
fi
# i386 的 isa-debug-exit 寫入 0 時,QEMU 回傳 1,視為成功
[ "$ARCH" = i386 ] && [ "$rc" -eq 1 ] && exit 0
exit $rc
