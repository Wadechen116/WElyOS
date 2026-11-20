#!/usr/bin/env bash
# exit5-class.sh - 判定一次「模擬器結束碼 5」是不是 Windows 版 QEMU 自己在結束時當掉(docs/ISSUES.md N-002(a))
# 用法(由 regress.sh source):exit5_is_host_crash <輸出檔>  回傳 0 = 是;1 = 不是或證據不足
# 兩個條件都要成立:kernel 印出它交給模擬器的值是 1(當機情境的值),
# 而且 win-exit.ps1 記下的 Windows 完整結束碼是 0xC0000005(存取違規)。
exit5_is_host_crash() {
  local log=$1 win
  win=$(grep -ao 'WIN_EXIT=[-0-9]* (0x[0-9A-F]*)' "$log" | tail -1)
  grep -aq '^exit: code 1 to the emulator' "$log" && [ "${win##*(0x}" = "C0000005)" ]
}
