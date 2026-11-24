#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp "${TMPDIR:-/tmp}/welyos-i349.XXXXXX")
trap 'rm -f "$out"' EXIT
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer tests/host/sched/i349_diagnosis_probe.c -o "$out"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out"
