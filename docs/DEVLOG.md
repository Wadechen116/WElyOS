# WElyOS 開發紀錄

逐步記錄每個步驟「做了什麼、為什麼這樣做、如何驗證」。每新增功能就追加一節。

## 環境

- 開發環境:Windows 11 + WSL2 (Ubuntu),NAT 網路模式
- 編譯器:WSL 內的 gcc(`-m32 -ffreestanding`),不需另外安裝交叉編譯器
- 建置:CMake ≥ 3.20 + make
- 模擬器:Windows 版 QEMU(`C:\Program Files\qemu\qemu-system-i386.exe`),由 WSL 呼叫
- 目標架構:i386(32 位元保護模式),以 multiboot v1 由 QEMU `-kernel` 直接載入
- 語言標準:C11(`CMAKE_C_EXTENSIONS OFF`),freestanding,不連結任何標準庫

---

## 第 1 步:最小可開機 kernel

**目標**:在 QEMU 序列埠印出 `Hello WElyOS`。

**檔案**

| 檔案 | 作用 |
|---|---|
| `CMakeLists.txt` | 專案定義、編譯與連結旗標 |
| `cmake/toolchain-i386.cmake` | 以主機 gcc 產生 32 位元目標 |
| `arch/i386/boot.S` | multiboot header、設定 stack、呼叫 `kmain`、`hlt` |
| `arch/i386/linker.ld` | 載入位址 1MB,配置各 section |
| `kernel/main.c` | COM1 (0x3F8) 序列埠輸出 |

**重點概念**

1. QEMU 在 ELF 內找到 multiboot 魔術數 `0x1BADB002`,把 kernel 載入 1MB 並跳到 `_start`。此時 CPU 已在 32 位元保護模式。
2. C 需要 stack,所以 `boot.S` 先設定 `esp` 才呼叫 `kmain`。
3. 沒有 libc,輸出直接對 I/O port 寫入;寫入前輪詢 `COM1+5` 的 bit 5,等傳送緩衝區空。
4. 旗標 `-ffreestanding -nostdlib -fno-pic -fno-stack-protector` 避免編譯器假設有 OS 或 libc。

**驗證**:執行 QEMU,序列埠輸出 `Hello WElyOS`。

---

## 第 2 步:`run` 與 `debug` target

**目標**:一個指令建置並啟動;另有可接 GDB 的除錯模式。

**做法**

- `scripts/qemu.sh`:自動尋找 QEMU;WSL 下使用 Windows 版並以 `wslpath -w` 轉換路徑;統一加上 `-serial stdio -display none -no-reboot` 與 `isa-debug-exit`。
- CMake 新增 `run`、`debug`(後者加 `-s -S`,即 GDB stub 在 tcp::1234 並於第一條指令前暫停)。
- `kmain` 結束後寫 port `0xF4` 讓 QEMU 自動離開。

**踩到的坑**

- `isa-debug-exit` 寫入 0 時 QEMU 回傳 **1**,make 會視為失敗。腳本中把 1 轉成 0。
- WSL2 為 NAT 模式時,Windows 版 QEMU 開的 1234 埠不能用 `127.0.0.1` 從 WSL 連;要連 Windows 主機位址(`ip route | awk '/default/{print $3}'`)。

**用法**

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i386.cmake
cmake --build build --target run
cmake --build build --target debug
```

**驗證**:`run` 輸出 `Hello WElyOS`,結束碼 0;`debug` 時 1234 埠可連線。

---

## 第 3 步:`kprintf` 格式化輸出

**目標**:kernel 能印出字串、數字、位址,之後除錯與 panic 都靠它。

**重構**

- 序列埠獨立成 `kernel/serial.c` + `include/serial.h`,並新增 `serial_init()`(關中斷、115200 baud、8N1、啟用 FIFO)。
- I/O port 存取 `outb/inb` 移到 `include/io.h`(inline)。
- CMake 加入 `include/` 為 include 目錄。

**`kprintf`**(`kernel/kprintf.c`、`include/kprintf.h`)

- 支援 `%c %s %d %i %u %x %X %p %%`,旗標 `0`、寬度(例如 `%08x`、`%5d`)、修飾 `l`。
- 使用 `<stdarg.h>`:它是編譯器提供的 freestanding 標頭,不需要 libc。
- `\n` 自動轉成 `\r\n`,序列終端機才會正確換行。
- 數字以 `out_num()` 統一處理:反向產生位數再倒序輸出;負數取 `0u - v` 以避免 `INT_MIN` 溢位;補零時負號置於零之前(`%05d` 印 -7 得 `-0007`)。
- `%p` 固定輸出 `0x` + 8 位十六進位(32 位元位址)。
- 目前不支援浮點、`%ll`(64 位元)、精度(`%.3s`);需要時再擴充。

**驗證**(`run` 的輸出)

```
Hello WElyOS
char=A str=text
dec=42 neg=-42 uns=4000000000
hex=beef HEX=BEEF pad=[00001234] w=[    7] z=[-0007]
ptr=0x00000000 kmain=0x0010005a pct=100%
```

`kmain` 位址 `0x0010005a` 落在 1MB 之後,與 linker script 相符。

---

## 第 4 步:VGA 文字螢幕與開機參數

**目標**:畫面也能顯示輸出;kernel 能讀取 bootloader 傳入的資訊。

**VGA 文字模式**(`kernel/vga.c`、`include/vga.h`)

- 顯示記憶體映射在 `0xB8000`,80×25,每格 16 位元:低位元組是字元,高位元組是屬性(高 4 位背景、低 4 位前景)。
- `vga_putc` 處理 `\n \r \b \t`,到行尾自動換行,到螢幕底自動捲動(整體上移一列、清空最後一列)。
- 硬體游標透過 CRTC 暫存器(port `0x3D4` 選索引、`0x3D5` 寫值;索引 `0x0F` 低位元組、`0x0E` 高位元組)更新。
- `vga_set_color` 切換前景/背景色。
- `kprintf` 的 `out_char` 同時輸出到序列埠與 VGA。

**multiboot 資訊**

- `boot.S` 依規範:進入點的 `eax` 為魔術數 `0x2BADB002`,`ebx` 為 info 結構位址。把兩者以 cdecl 順序推入 stack,`kmain(magic, mbi_addr)` 就能直接收到。
- `include/multiboot.h` 定義 `struct multiboot_info`(目前用 flags、mem_lower/upper、cmdline)。
- kernel 會印出開機參數與記憶體大小(例如 lower=639KB、upper=129920KB)。

**開機參數 `autoexit`**

- 以 QEMU 的 `-append` 傳入。`kmain` 看到 `autoexit` 才呼叫 `isa-debug-exit` 結束,否則 `hlt` 迴圈。
- `run`:帶 `autoexit`,適合自動化測試。
- `run-gui`:開 QEMU 視窗(`QEMU_DISPLAY=default`),kernel 持續運行。
- 注意:QEMU 會把 kernel 檔案路徑放在 cmdline 最前面,所以參數比對採用「以空白分隔的單字搜尋」。

**驗證方式(無視窗也能看畫面)**

用 QEMU monitor 截圖,再轉成圖片檢視:

```
(sleep 3; echo "screendump <path>.ppm"; sleep 2; echo quit) | \
  qemu-system-i386 -kernel welyos.elf -display none -monitor stdio -serial null
```

已確認:文字、綠色屬性、硬體游標、捲動(印 30 行後螢幕最後一行為第 30 行)皆正確。

**已知限制**

- 目前沒有 `memcpy/memmove`,捲動以迴圈手寫。編譯器在最佳化時可能把迴圈轉成 `memcpy` 呼叫,屆時需補 `lib/string.c`。

---

## 註解慣例

- 註解使用繁體中文;每個檔案開頭說明用途與背景知識(硬體、規範)。
- 註解說明「為什麼」與硬體細節(暫存器、port、位元意義),而非重述程式碼。
- `boot.S` 逐段說明開機時 CPU 狀態、stack 設定與 cdecl 參數傳遞。
- 新增功能時一併撰寫註解;只加註解的修改不應改變行為,以 `make run` 輸出不變為驗證。

---

## 第 5 步:多架構重構(i386 / RISC-V 64 / AArch64)

**目標**:同一份 kernel 可編譯到三種架構;架構相關程式集中在 `arch/<名稱>/`。

**分層**

```
include/arch.h        架構介面:arch_init / arch_putc / arch_halt / arch_exit / arch_name
include/boot_info.h   與架構無關的開機資訊(cmdline、記憶體位址與大小)
kernel/main.c         只透過 arch_* 與 boot_info,不碰任何硬體
kernel/fdt.c          裝置樹解析(RISC-V / AArch64 共用)
lib/string.c          memcpy/memmove/memset/memcmp/strlen/strcmp/strncmp
arch/<名稱>/          boot.S、linker.ld、start.c、arch.c、arch.cmake(+ i386 的 serial/vga)
```

流程:`boot.S` → `arch_start()`(各架構的 `start.c`,把 multiboot 或 DTB 轉成 `boot_info`)→ `kmain()`。

**三種架構的差異**

| | i386 | RISC-V 64 | AArch64 |
|---|---|---|---|
| 載入者 | QEMU multiboot | OpenSBI(內建) | QEMU 直接載入 |
| 載入位址 | 0x100000 | 0x80200000 | 0x40080000 |
| 開機資訊 | multiboot info(eax/ebx) | DTB(a1) | DTB(x0) |
| 輸出 | COM1(port I/O)+ VGA | NS16550 @0x10000000(MMIO) | PL011 @0x09000000(MMIO) |
| 結束 QEMU | isa-debug-exit(port 0xF4) | SiFive finisher @0x100000 | PSCI SYSTEM_OFF(hvc) |
| 特殊編譯旗標 | `-m32` | `-march=rv64imac_zicsr -mcmodel=medany -mno-relax` | `-mgeneral-regs-only -mstrict-align` |

**重點與踩坑**

- **MMIO vs port I/O**:x86 用 `in/out` 存取獨立的 I/O 位址空間;RISC-V、AArch64 的週邊映射在記憶體位址,用 `volatile` 指標存取。
- **AArch64 `-mstrict-align`**:MMU 關閉時記憶體為 Device 類型,未對齊存取會觸發例外;`-mgeneral-regs-only` 避免編譯器用到尚未啟用的 FP/SIMD。
- **RISC-V `-mno-relax`**:連結器的 gp 相對最佳化需要先設定 `gp` 暫存器,目前不做。
- **BSS**:RISC-V、AArch64 的 `boot.S` 自行清 BSS(linker script 提供 `__bss_start/__bss_end`)。
- **多核心**:非 0 號核心在 `boot.S` 直接休眠。
- **裝置樹**:`fdt.c` 以位元組組合讀取大端整數(避免未對齊存取),只取 `/chosen/bootargs` 與 `/memory` 的第一組 `reg`。
- **i386 無 libgcc**:32 位元的 libgcc 未安裝,64 位元 `/`、`%` 會缺 `__udivdi3`。`kprintf` 的十進位轉換改用位移-相減的 `udivmod()`。
- **`-fno-tree-loop-distribute-patterns`**:避免編譯器把 `lib/string.c` 的迴圈改寫成呼叫 `memcpy` 本身。
- **kprintf 擴充**:支援 `l`、`ll`、`z` 修飾,`%p` 依指標寬度輸出。以主機端測試與標準 `printf` 逐項比對。

**建置**(`CMakePresets.json`)

```
cmake --preset i386    && cmake --build build/i386    --target run
cmake --preset riscv64 && cmake --build build/riscv64 --target run
cmake --preset aarch64 && cmake --build build/aarch64 --target run
```

**需要的套件(WSL)**:`sudo apt install gcc-riscv64-unknown-elf gcc-aarch64-linux-gnu`

**驗證**:三個架構的 `run` 皆輸出 `Hello WElyOS`、架構名稱、`cmdline: "...autoexit"`、記憶體資訊,並自動結束(結束碼 0)。

| 架構 | 偵測到的記憶體 | 備註 |
|---|---|---|
| i386 | base=0x100000 size=129920 KB | cmdline 含 kernel 路徑 |
| riscv64 | base=0x80000000 size=131072 KB | OpenSBI 橫幅先於 kernel 輸出 |
| aarch64 | base=0x40000000 size=131072 KB | 需載入 welyos.bin(見下) |

**AArch64 踩坑:ELF 拿不到 DTB**

- 現象:以 ELF 載入時 `x0 = 0`,在 RAM 0x40000000~0x48000000 全面掃描也找不到 DTB 魔術數 `0xd00dfeed`。
- 原因:QEMU 只在載入「非 ELF」的 kernel 時,才依 Linux 啟動協定把 DTB 位址放進 `x0`。
- 解法:CMake 在 aarch64 建置後以 `objcopy -O binary` 另產生 `welyos.bin`,`scripts/qemu.sh` 載入它。原始二進位在 QEMU 會被放到 RAM 起點 +0x80000(= 0x40080000),與 linker script 一致。ELF 仍保留供 GDB 讀符號。
- 判斷方式:`arch.cmake` 設 `ARCH_RAW_BINARY ON` 即啟用,其他架構不受影響。

**排查技巧**

- 透過管線(`| grep`)看 QEMU 輸出,若以 timeout 強制結束,最後幾行可能因緩衝而遺失;懷疑時改用 `-serial file:...` 寫到檔案再讀。
- QEMU monitor(`-monitor stdio`)可用 `xp /4wx <位址>` 檢視實體記憶體、`info registers` 看暫存器。
