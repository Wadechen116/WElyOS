#!/usr/bin/env python3
# 模擬有人在打字:等 kernel 開機完成後,一個字元一個字元慢慢送出。
# 用法: type.py <開始前等待秒數> <每個字元間隔秒數> <文字(\n 代表 Enter)>
import sys, time
start, gap, text = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3].encode().decode('unicode_escape')
time.sleep(start)
for ch in text:
    sys.stdout.write(ch); sys.stdout.flush()
    time.sleep(gap)
time.sleep(1.0)
