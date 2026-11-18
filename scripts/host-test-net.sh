#!/usr/bin/env bash
# 主機端封包測試不使用 Claude 的 build/ 與 QEMU;輸出放在獨立暫存目錄。
set -euo pipefail
cd "$(dirname "$0")/.."
NET_TEST_OUT=$(mktemp -d "${TMPDIR:-/tmp}/welyos-net.XXXXXX")
echo "Artifacts: $NET_TEST_OUT"
COMMON=(-std=c11 -Wall -Wextra -Werror -fno-builtin -iquote include)
gcc "${COMMON[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-pie -no-pie tests/host/net/test_net.c kernel/net/stack.c -o "$NET_TEST_OUT/test_net"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$NET_TEST_OUT/test_net"
if [[ -f tests/host/net/test_dns.c ]]; then
    gcc "${COMMON[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        -fno-pie -no-pie tests/host/net/test_dns.c -o "$NET_TEST_OUT/test_dns"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$NET_TEST_OUT/test_dns"
fi
for target in i386 riscv64 aarch64; do
    case "$target" in
        i386) compiler=gcc; options=(-m32 -mgeneral-regs-only) ;;
        riscv64) compiler=riscv64-unknown-elf-gcc; options=(-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany) ;;
        aarch64) compiler=aarch64-linux-gnu-gcc; options=(-mgeneral-regs-only -mstrict-align) ;;
    esac
    if ! command -v "$compiler" >/dev/null; then
        echo "$target: coverage unavailable (missing $compiler)"; continue
    fi
    "$compiler" "${COMMON[@]}" "${options[@]}" -O2 -ffreestanding -nostdlib \
        -c kernel/net/stack.c -o "$NET_TEST_OUT/stack-$target.o"
    if [[ -f tests/host/net/compile_dns.c ]]; then
        "$compiler" "${COMMON[@]}" "${options[@]}" -O2 -ffreestanding -nostdlib \
            -c tests/host/net/compile_dns.c -o "$NET_TEST_OUT/dns-$target.o"
    fi
    # i386 不連結 libgcc,不能偷偷產生除法、浮點等執行期輔助函式。
    if nm -u "$NET_TEST_OUT/stack-$target.o" | grep -Eq '__.*(div|mod|float|muldi)'; then
        echo "$target: forbidden runtime helper"; exit 1
    fi
    echo "$target: freestanding compile PASS"
done
