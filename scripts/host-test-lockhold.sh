#!/usr/bin/env bash
# I-055:純模型邊界檢查，含 ASan/UBSan；不啟動 QEMU。
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp "${TMPDIR:-/tmp}/welyos-lockhold.XXXXXX")
trap 'rm -f "$out"' EXIT
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Iinclude tests/host/time/lockhold_model_test.c -o "$out"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out"
