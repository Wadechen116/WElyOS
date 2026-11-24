#!/usr/bin/env bash
# test-exit5-class.sh - exit5_is_host_crash 的正反案例(由 regress.sh 執行)
cd "$(dirname "$0")/.." && . scripts/exit5-class.sh
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT; bad=0
t() { printf '%b' "$2" > "$d/log"; exit5_is_host_crash "$d/log"; r=$?; [ "$r" -eq "$1" ] || { echo "exit5-class: case '$3' gave $r, want $1"; bad=1; }; }
t 0 'backtrace\nexit: code 1 to the emulator\nWIN_EXIT=-1073741819 (0xC0000005)\n' "kernel 1 + access violation"
t 1 'backtrace\nexit: code 2 to the emulator\nWIN_EXIT=5 (0x00000005)\n'           "i386 kernel wrote 2"
t 1 'backtrace\nexit: code 1 to the emulator\n'                                     "no full exit code recorded"
t 1 'backtrace\nWIN_EXIT=-1073741819 (0xC0000005)\n'                                "kernel never reached exit"
t 1 'exit: code 1 to the emulator\nWIN_EXIT=-1073741571 (0xC00000FD)\n'              "other NTSTATUS"
# 第 73 步:結束碼寫在獨立檔(QEMU_WINEXIT_FILE)。f <預期> <log 內容> <結束碼檔內容或 NONE> <說明>
f() { printf '%b' "$2" > "$d/log"; rm -f "$d/wx"; [ "$3" = NONE ] || printf '%b' "$3" > "$d/wx"
      exit5_is_host_crash "$d/log" "$d/wx"; r=$?; [ "$r" -eq "$1" ] || { echo "exit5-class: case '$4' gave $r, want $1"; bad=1; }; }
f 0 'backtrace\nexit: code 1 to the emulator\n' 'WIN_EXIT=-1073741819 (0xC0000005)\n'  "sidecar: kernel 1 + access violation"
f 1 'backtrace\nexit: code 1 to the emulator\n' NONE                                      "sidecar missing"
f 1 'backtrace\nexit: code 1 to the emulator\n' ''                                        "sidecar empty"
f 1 'backtrace\nexit: code 1 to the emulator\n' 'WIN_EXIT=garbage\n'                     "sidecar malformed"
f 1 'backtrace\nexit: code 1 to the emulator\n' 'WIN_EXIT=5 (0x00000005)\n'              "sidecar: ordinary 5"
# 給了結束碼檔時,不可以退回去從 log 裡找(log 裡的標記可能是交錯殘留)
f 1 'backtrace\nexit: code 1 to the emulator\nWIN_EXIT=-1073741819 (0xC0000005)\n' NONE "sidecar missing, marker only in log"
# 交錯的 log(第 73 步 regress-73b 實際出現的形狀):kernel 的行被切斷,不可以算成「kernel 給了 1」
f 1 'backtrace\neWIN_EXIT=-1073741819 (0xC0000005)\nxit: code 1 to the emulator\n' 'WIN_EXIT=-1073741819 (0xC0000005)\n' "interleaved guest log"
[ $bad -eq 0 ] && echo "exit5-class: 12/12 cases PASS"; exit $bad
