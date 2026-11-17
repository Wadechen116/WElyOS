#!/usr/bin/env bash
# 重新建置 RISC-V 的開機韌體 OpenSBI,拿掉它開機時印出的 "OpenSBI" 大字標誌。
#
# 為什麼需要:RISC-V 開機時最先執行的不是 kernel,而是 M-mode 的韌體 OpenSBI
# (QEMU 內附一份),它會先印出自己的標誌。WElyOS 的標誌由 kernel 印出,
# 三個架構完全相同(見 kernel/main.c 的 boot_logo);RISC-V 若再多一個韌體的標誌,
# 就和另外兩個架構不一致。韌體的標誌寫死在它的程式裡,要拿掉只能自己編譯一份。
#
# 做法:下載 OpenSBI 的原始碼(固定版本,見下方),只改一處 —
# 拿掉 lib/sbi/sbi_init.c 裡印出標誌的那一行 — 然後編譯。
# 韌體印出的版本與平台資訊保持原樣。
# 產生的 firmware/opensbi-riscv64-welyos.bin 已經放進版本庫,一般不需要執行本腳本;
# 只有要換 OpenSBI 版本時才需要。
#
# 需求:git、make、python3、riscv64-unknown-elf-gcc(與建置 kernel 相同的編譯器)
# 用法:bash scripts/build-opensbi.sh
set -e
cd "$(dirname "$0")/.."

# 版本:v1.3。QEMU 內附的是 v1.4,但 v1.4 起韌體一律建置成「可重新定位」的形式,
# 需要支援該功能的連結器;我們用的裸機工具鏈(riscv64-unknown-elf-)沒有。
# v1.3 可以建置成固定位址的版本(QEMU 本來就把韌體載入在固定的 0x80000000),
# WElyOS 用到的 SBI 功能(計時器、處理器間中斷、啟動其他 hart)兩個版本都有。
VERSION=v1.3
SRC=build/opensbi-src
OUT=firmware/opensbi-riscv64-welyos.bin

if [ ! -d "$SRC/.git" ]; then
  mkdir -p build
  git clone -q --depth 1 --branch "$VERSION" \
      https://github.com/riscv-software-src/opensbi.git "$SRC"
fi
git -C "$SRC" checkout -q -- lib/sbi/sbi_init.c      # 每次都從原始的檔案開始改

# 拿掉印出標誌的那一行(標誌的字串本身留著沒關係,沒有人用到它)
python3 - "$SRC/lib/sbi/sbi_init.c" <<'PY'
import sys
src = sys.argv[1]
text = open(src, encoding="utf-8").read()
if text.count("sbi_printf(BANNER);") != 1:
    sys.exit("找不到印出標誌的那一行,OpenSBI 的原始碼可能改版了")
open(src, "w", encoding="utf-8").write(text.replace("sbi_printf(BANNER);", "/* 標誌由 WElyOS 的 kernel 印出 */"))
PY

make -C "$SRC" -s -j"$(nproc)" PLATFORM=generic CROSS_COMPILE=riscv64-unknown-elf- \
     PLATFORM_RISCV_XLEN=64 FW_PAYLOAD=n FW_JUMP=n
cp "$SRC/build/platform/generic/firmware/fw_dynamic.bin" "$OUT"
cp "$SRC/COPYING.BSD" firmware/LICENSE-OpenSBI.txt
echo "已產生 $OUT($(stat -c %s "$OUT") 位元組)"
