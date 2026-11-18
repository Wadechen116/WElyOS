#!/usr/bin/env bash
# 預設快速詞法檢查;--mode analyze 另外用三架構的真實編譯命令執行分析器。
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 scripts/static/check.py "$@"
