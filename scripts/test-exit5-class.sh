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
[ $bad -eq 0 ] && echo "exit5-class: 5/5 cases PASS"; exit $bad
