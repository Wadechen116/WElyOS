#!/usr/bin/env python3
"""模擬有人在打字:等 kernel 開機完成後,一個字元一個字元慢慢送出。

用法: type-input.py <開始前等待秒數> <每個字元間隔秒數> <文字>

文字中可以使用的跳脫寫法:
    \\n      Enter
    \\x03    Ctrl-C        \\x04    Ctrl-D        \\x7f    Backspace
    \\x1a    Ctrl-Z
    \\x1f    不送出任何字元,只是停頓 0.3 秒(例如等一個命令開始執行再按 Ctrl-C)

為什麼不直接用管線把整段文字送進去:Windows 版的 QEMU 在客體來不及接收時
會直接把字元丟掉(見 docs/DEVLOG.md 第 21 步與第 25 步的量測)。
每個字元間隔 2ms 以上就不會掉字。

要測試「使用者坐在終端機前面」的情況(例如 Ctrl-C 會不會被終端機攔下來),
用虛擬終端機執行 QEMU,而不是直接接管線:
    python3 scripts/type-input.py 1.5 0.01 'sleep 30\n\x1f\x03exit\n' | \
        script -qc "bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf" /dev/null

範例:
    python3 scripts/type-input.py 1.5 0.01 'ls /bin | wc\\nexit\\n' | \\
        bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit shell"
"""
import sys
import time

start, gap = float(sys.argv[1]), float(sys.argv[2])
text = sys.argv[3].encode().decode("unicode_escape")
time.sleep(start)
for ch in text:
    if ch == "\x1f":
        time.sleep(0.3)
        continue
    sys.stdout.write(ch)
    sys.stdout.flush()
    time.sleep(gap)
time.sleep(1.0)      # 讓最後的輸出有時間印出來,再關閉輸入
