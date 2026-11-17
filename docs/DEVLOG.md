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


---

## 第 6 步:GDB 除錯

**目標**:用同一個 `gdb-multiarch` 除錯三種架構,在 QEMU 中下中斷點、單步、看暫存器與記憶體。

**需要**:`sudo apt install gdb-multiarch`(Qt 附的 gdb.exe 只支援 x86_64,不能用)。

**元件**

| 檔案 | 作用 |
|---|---|
| `scripts/gdb.sh <arch> <elf>` | 選對 `set architecture`(i386 / riscv:rv64 / aarch64)、載入 ELF 符號、連到 QEMU |
| `scripts/gdbinit` | 共用設定與自訂指令(`welyos-start`、`bt-short`) |
| CMake `gdb` target | 呼叫 gdb.sh |
| CMake `debug` target | 啟動 QEMU,帶 `-s -S`(GDB stub 於 tcp::1234,並在第一條指令前暫停) |

**用法(需要兩個終端機)**

```
# 終端機 A:啟動 QEMU 並暫停等待
cmake --build build/riscv64 --target debug

# 終端機 B:連上 gdb
cmake --build build/riscv64 --target gdb
(gdb) welyos-start        # 在 arch_start、kmain 設中斷點並執行
(gdb) bt                  # backtrace
(gdb) info registers
(gdb) x/5i $pc            # 反組譯
(gdb) stepi / next / step # 單步
(gdb) print *bi
```

**WSL NAT 的連線位址**:QEMU 是 Windows 程式,gdb 在 WSL。NAT 模式下 `127.0.0.1` 連不到,`gdb.sh` 會自動改用 Windows 主機位址(`ip route | awk '/default/{print $3}'`);可用 `GDB_HOST`、`GDB_PORT` 覆寫。

**驗證結果**(三個架構皆以 `-batch` 腳本自動測試)

| 架構 | QEMU 暫停位置 | 中斷點 | backtrace |
|---|---|---|---|
| i386 | 0x0000fff0(重設向量) | kmain @0x100625 | kmain → arch_start → _start |
| riscv64 | 0x00001000(QEMU 內建 ROM) | kmain @0x80200240 | 同上,`dtb=0x87e00000` |
| aarch64 | 0x40000000 | kmain @0x40080218 | 同上,`dtb=0x44000000` |

三者都能讀取變數(`bi->mem_base`、`bi->cmdline`)與反組譯。

**重點與踩坑**

- **AArch64**:QEMU 載入的是 `welyos.bin`,gdb 讀的是 `welyos.elf` 的符號;兩者位址一致(0x40080000),所以中斷點正常。DTB 位址 `0x44000000` 也證實了:ELF 模式下 QEMU 不載入 DTB。
- **backtrace 結尾的提示**(`frame did not save the PC` 等):`_start` 沒有呼叫框架資訊(CFI),gdb 無法繼續往上推,屬正常現象。
- **kmain 不會返回**,所以不能用 `finish`,否則 gdb 會一直等待。
- **批次模式**結尾要加 `detach` 與 `quit`,否則 gdb 不會結束。
- **不要用 `pkill -f qemu-...`**:比對字串若出現在自己的指令列,會連目前的 shell 一起殺掉。改用 `taskkill.exe /F /IM qemu-system-xxx.exe`。
- 暫停在第一條指令時,尚未進入 kernel(位址 `0x...fff0`/`0x1000`/`0x40000000` 是 CPU 重設位置或 QEMU 內建啟動碼),要 `continue` 到中斷點才進入 WElyOS。

---

## 觀念:符號位址如何決定,以及中斷點為何不在函式開頭

以 GDB 看到的 `kmain @ 0x80200240`(RISC-V)、`kmain @ 0x40080218`(AArch64)為例。這兩個數字由**兩層**決定。

### 第一層:函式本身的位址(連結器決定)

| | RISC-V | AArch64 |
|---|---|---|
| `linker.ld` 起點 `. = ...` | `0x80200000` | `0x40080000` |
| `.text` 第一個放 `.text.boot`(`_start`) | `0x80200000` | `0x40080000` |
| `arch_start` | `0x80200034` | `0x40080044` |
| `cmdline_has` | `0x8020014a` | `0x400800fc` |
| **`kmain`** | **`0x80200234`** | **`0x4008020c`** |
| `kprintf` | `0x80200a48` | `0x40080d2c` |

決定過程:

1. **起點由 `linker.ld` 手動指定**,依硬體與韌體的約定:
   - RISC-V:OpenSBI 佔用 `0x80000000` 起的空間,約定 S-mode kernel 載入於 `0x80200000`。
   - AArch64:`virt` 的 RAM 自 `0x40000000` 起,慣例保留前 `0x80000`(給 DTB 等),kernel 放在 `0x40080000`。
2. **`.text` 內的順序**由 linker script 與連結順序決定:先放 `*(.text.boot)`(`boot.S`,確保 `_start` 在最前),再依 CMake 中列出的檔案順序放 `*(.text*)`:`boot.S` → `start.c` → `arch.c` → `main.c` → `kprintf.c` → `fdt.c` → `string.c`。
3. **每個函式位址 = 起點 + 它前面所有程式碼的大小**。因此:
   - 在前面的檔案加幾行程式,`kmain` 的位址就會改變。**不要記位址,要用符號**(`break kmain`、`nm`)。
   - 同一份 C 程式在不同架構的位址不同,是因為指令長度不同(RISC-V 有 2 位元組壓縮指令,AArch64 固定 4 位元組)。

### 第二層:中斷點位置 = 函式位址 + 序言長度(GDB 決定)

`break kmain` 並不是停在 `kmain` 的第一個位元組,而是**跳過函式序言(prologue)**,停在第一行 C 程式碼。

序言是編譯器在函式開頭產生的固定動作:調整 `sp`、儲存返回位址與框架指標、把傳入參數存到 stack。反組譯如下:

```
RISC-V                                    AArch64
80200234 addi sp,sp,-32   ┐               4008020c stp x29,x30,[sp,#-32]!  ┐
80200236 sd   ra,24(sp)   │ 序言          40080210 mov x29,sp               │ 序言
80200238 sd   s0,16(sp)   │ (12 位元組)   40080214 str x0,[sp,#24]          ┘ (12 位元組)
8020023a addi s0,sp,32    │
8020023c sd   a0,-24(s0)  ┘
80200240 auipc a0,0x1   ← 第 39 行         40080218 adrp x0,...  ← 第 39 行
```

- GDB 讀取編譯時由 `-g` 產生的**行號表**(DWARF),找到「第一行原始碼」對應的位址,中斷點下在那裡。所以是 `kmain+12`。
- 序言執行前,參數與區域變數尚未放入 stack,此時查看會得到不正確的值;停在序言之後才能正確讀取 `bi` 等變數。
- 想停在函式最前面:`break *kmain` 或 `break *0x80200234`(位址前加 `*` 代表確切位址,不跳過序言)。

### 如何自己驗證

```
# 列出符號與位址(-n 依位址排序)
riscv64-unknown-elf-nm -n  build/riscv64/welyos.elf | grep -E " (_start|arch_start|kmain)$"
aarch64-linux-gnu-nm -n    build/aarch64/welyos.elf | grep -E " (_start|arch_start|kmain)$"

# 查看各區段的起點與大小
riscv64-unknown-elf-readelf -S build/riscv64/welyos.elf

# 反組譯函式,對照序言與第一行程式碼
riscv64-unknown-elf-objdump -d --no-show-raw-insn build/riscv64/welyos.elf | less
```

在程式中也可以直接印出:`kmain` 啟動時會輸出 `kmain=0x...`,應與 `nm` 結果一致(RISC-V 為 `0x80200234`,AArch64 為 `0x4008020c`)。

### 小結

| 問題 | 由誰決定 | 依據 |
|---|---|---|
| kernel 載入哪裡 | 我們(`linker.ld`) | 硬體/韌體約定的位址 |
| 函式放在哪 | 連結器 | 起點 + 檔案順序 + 前面程式碼大小 |
| 中斷點停在哪 | GDB | 函式位址 + 序言長度(行號表) |

---

## 第 7 步(RISC-V):例外處理與 panic

**目標**:kernel 出錯時不再無聲卡住,而是印出原因、暫存器與呼叫堆疊;並驗證「例外 → 處理 → 返回」這條路是對的。

### 什麼是 trap

CPU 遇到例外(非法指令、存取錯誤、`ebreak`…)或中斷時,**硬體**自動:

1. 把當時的 pc 存進 `sepc`,原因存進 `scause`,附加資訊(出錯位址、非法指令內容)存進 `stval`
2. 關閉中斷(`sstatus.SIE` 清 0),舊狀態記在 `sstatus.SPIE`
3. 跳到 `stvec` 指向的位址

硬體**不會**替我們存任何一般暫存器,所以入口程式必須先全部存起來,否則被打斷的程式會被破壞。

### 元件

| 檔案 | 作用 |
|---|---|
| `arch/riscv64/trap.S` | `trap_entry`:挖出 trap frame、存 x1~x31 與 4 個 CSR、呼叫 C、還原、`sret` |
| `arch/riscv64/trap.c` | `trap_init()` 設定 `stvec`;`trap_handler()` 判斷原因並處理 |
| `arch/riscv64/trap.h` | `struct trap_frame`(版面須與 `trap.S` 一致) |
| `kernel/panic.c`、`include/panic.h` | 與架構無關的 `panic()` |
| `arch_backtrace()` | 三個架構各自實作,沿 frame pointer 鏈回溯 |
| `scripts/backtrace.sh` | 把 backtrace 位址轉成「函式 at 檔案:行號」 |

### trap frame 版面

`trap.S` 把暫存器依編號存在 stack 上,`trap.c` 的 `struct trap_frame` 欄位順序必須完全一致,並以 `_Static_assert` 在編譯期檢查:

| slot | 內容 | offset |
|---|---|---|
| 0~30 | x1~x31(slot = 暫存器編號 − 1;x0 恆為 0 不存) | 0~240 |
| 31 | sepc | 248 |
| 32 | sstatus | 256 |
| 33 | scause | 264 |
| 34 | stval | 272 |

共 35 slot = 280 位元組,補到 288 以維持 16 位元組對齊。`x2`(sp)存的是被打斷時的值(目前 sp + 288),不是 trap 當下的 sp。

### 處理流程

```
例外發生 → 硬體存 sepc/scause/stval,跳到 stvec
  → trap_entry:存暫存器 → 呼叫 trap_handler(frame)
      ├ 中斷:目前尚未啟用,視為錯誤 → panic
      ├ breakpoint(可恢復):印訊息、sepc += 指令長度、return
      └ 其他例外:印 scause/sepc/stval/暫存器/backtrace → panic_halt
  → 還原 CSR 與暫存器 → sret(回到 sepc)
```

- **跳過 ebreak**:`sepc` 指向 `ebreak` 本身,不調整會無限重複。指令長度看低 2 位元:`11` 為 4 位元組,否則為 2 位元組壓縮指令(`ebreak` 有 2 位元組的 `c.ebreak` 版本)。
- `handler` 修改 frame 內的 `sepc`,`trap.S` 返回前才寫回 CSR,所以可以改變返回位置。

### 常見 scause 值

| scause | 意義 | 範例 |
|---|---|---|
| 2 | 非法指令 | `stval` 為該指令的編碼 |
| 3 | 中斷點 | `ebreak` |
| 5 | 讀取存取錯誤 | `stval` 為出錯位址 |
| 7 | 寫入存取錯誤 | 同上 |
| 12/13/15 | 指令/讀取/寫入 page fault | 啟用分頁後才會出現 |
| 最高位元為 1 | 中斷(非例外) | 如計時器 |

### panic 與測試用開機參數

`panic()` 印出訊息與 backtrace。行為由 `panic_set_exit()` 決定:帶 `autoexit` 時結束 QEMU 並回傳**失敗碼**(讓腳本與 CI 能判斷),否則永久閒置(方便用 GDB 檢查現場)。

測試以開機參數觸發:

```
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit crash=fault"
```

| 參數 | 行為 | RISC-V 結果 |
|---|---|---|
| `crash=panic` | 直接呼叫 `panic()` | KERNEL PANIC,rc=1 |
| `crash=fault` | 讀取位址 0(原名 `crash=null`,見下方「跨架構」一節) | Load access fault(scause=5, stval=0),rc=1 |
| `crash=illegal` | 執行非法指令 | Illegal instruction(scause=2, stval=0xffffffff),rc=1 |
| `crash=break` | 執行軟體中斷點(`ebreak`) | 印出 resuming、繼續執行、印出 survived,rc=0 |

`crash=break` 驗證了整條「存檔 → 處理 → 還原 → sret」路徑:若任何暫存器還原錯誤,後面的程式就會壞掉。

### backtrace

使用 `-fno-omit-frame-pointer` 保留 frame pointer 鏈。各架構的 frame 版面不同:

| 架構 | fp 暫存器 | 上一層 fp | 返回位址 |
|---|---|---|---|
| RISC-V | s0 | `*(fp − 16)` | `*(fp − 8)` |
| i386 | ebp | `*(ebp)` | `*(ebp + 4)` |
| AArch64 | x29 | `*(fp)` | `*(fp + 8)` |

回溯時每一步都檢查:fp 在 stack 範圍內、對齊、且往高位址前進,避免遇到損壞的鏈時再次出錯。`boot.S` 把 fp 歸零,作為鏈的終點。

**用工具解析**(自動處理下面的 `−1`):

```
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit crash=fault" > log.txt
bash scripts/backtrace.sh riscv64 build/riscv64/welyos.elf < log.txt
  #0 crash_test at kernel/main.c:51
  #1 kmain at kernel/main.c:77
  #2 arch_start at arch/riscv64/start.c:21
  #3 _start at arch/riscv64/boot.S:34
```

**為什麼 `ra` 要減 1 才能查**:`ra` 是返回位址,即呼叫指令的**下一條**。若被呼叫的是不會返回的函式(如 `kmain`),呼叫指令是呼叫者的最後一條,返回位址就剛好落在**下一個函式**的開頭,直接查會得到錯誤的函式(實測曾把 `arch_start` 誤判成 `arch_init`)。減 1 後必定落在呼叫那一行。`pc`(出錯指令位址)則不用減。

**限制**:葉函式(leaf)若沒有建立 frame,則該層會被略過;沒有符號表在 kernel 內,故只印位址,靠 `addr2line` 還原。

### 踩坑

- **把架構專屬指令寫進共用程式碼**:一度在 `kernel/main.c` 直接寫 RISC-V 的 `ebreak`,i386 組譯失敗。改成 `arch_test_breakpoint()`、`arch_test_illegal()`,由各架構實作。
- **檢查建置結果要看結束碼**:我的 `grep error` 只比對小寫,漏掉組譯器的 `Error:`,導致 i386 / AArch64 用舊的執行檔測試,看起來「沒有 panic」。教訓:以 `cmake --build ...; echo rc=$?` 確認建置成功,且比對 `kmain=` 位址是否變動。
- **AArch64 失敗碼**:PSCI `SYSTEM_OFF` 無法帶結束碼,panic 會被誤判為成功(rc=0)。失敗時改用 semihosting `SYS_EXIT`(`hlt #0xf000`,`qemu.sh` 為 aarch64 加 `-semihosting`),成功仍用 PSCI。
- **i386 的結束碼**:`isa-debug-exit` 寫入 N 得到 `(N<<1)|1`,成功為 1(腳本轉成 0)、panic 為 3。

---

## 第 7 步(AArch64):例外向量表

### 機制

例外發生時硬體自動:返回位址存入 `ELR_EL1`、處理器狀態存入 `SPSR_EL1`、原因存入 `ESR_EL1`、出錯位址存入 `FAR_EL1`,然後跳到 **`VBAR_EL1` + 偏移**。

與 RISC-V「單一入口、再看 scause」不同,AArch64 用**向量表**:16 個入口,每個固定 0x80 位元組,整張表須 2KB 對齊。

| 偏移 | 來源 | 四個入口(各 0x80) |
|---|---|---|
| 0x000 | 目前 EL,使用 SP_EL0 | sync / IRQ / FIQ / SError |
| 0x200 | 目前 EL,使用 SP_ELx(**kernel 平常的狀態**) | 同上 |
| 0x400 | 較低 EL,AArch64(未來的使用者程式) | 同上 |
| 0x600 | 較低 EL,AArch32 | 同上 |

- **sync**:由指令本身引發(未定義指令、存取錯誤、`brk`、`svc`)。原因細分看 `ESR_EL1` 的 EC 欄位(bit 31:26)。
- **IRQ / FIQ / SError**:非同步;目前尚未啟用,收到即 panic。

### 實作

- `trap.S`:`VENTRY` 巨集產生 16 個入口,每個只做「挖 frame、存 x0/x1、帶種類編號跳到 `trap_common`」(入口空間只有 32 條指令)。`trap_common` 存其餘暫存器與 4 個系統暫存器,呼叫 `trap_handler(frame, kind)`,還原後 `eret`。
- trap frame:x0~x30、sp、elr、spsr、esr、far,共 36 slot = 288 位元組。
- `trap_init()`:`msr vbar_el1` 後加 `isb`,確保設定在後續指令前生效。
- `brk` 的 `ELR` 指向 `brk` 本身,處理時 `elr += 4`(AArch64 指令固定 4 位元組)。

### 常見 EC 值

| EC | 意義 |
|---|---|
| 0x00 | 未知原因(通常是未定義指令) |
| 0x15 | SVC(系統呼叫) |
| 0x21 / 0x20 | Instruction abort(同 EL / 較低 EL) |
| 0x25 / 0x24 | Data abort(同 EL / 較低 EL),`FAR` 為出錯位址 |
| 0x22 / 0x26 | PC / SP 對齊錯誤 |
| 0x3C | BRK 中斷點 |

### 踩坑

- **backtrace 少一層**:`arch_test_fault → kmain`,中間的 `crash_test` 不見。AArch64 的 GCC 預設不替**葉函式**建立 frame record(返回位址只留在 `x30`),即使有 `-fno-omit-frame-pointer`。加上 `-mno-omit-leaf-frame-pointer` 後完整。
- **位址 0 不會出錯**:virt 機器的位址 0 是 flash,讀得到。改讀超出 CPU 實體位址範圍的位址(`0x0010000000000000`),MMU 關閉時產生 address size fault(Data abort,`FAR` 即該位址)。

---

## 第 7 步(i386):GDT、IDT 與例外入口

### 為什麼 x86 多了 GDT

保護模式下,`cs`/`ds` 等區段暫存器存的是**選擇子**,指向 GDT(全域描述符表)的一項,描述區段的基底、長度、權限。multiboot 進入時區段可用,但 bootloader 的 GDT 位置不保證保留,而 IDT 的每一項都要參照程式碼區段選擇子,所以必須先建立自己的 GDT。

採**平坦模型**:基底 0、長度 4GB,等於不使用分段(位址保護之後交給分頁)。

| 索引 | 選擇子 | 內容 | access |
|---|---|---|---|
| 0 | 0x00 | 空描述符(規定) | — |
| 1 | 0x08 | kernel 程式碼 | 0x9A |
| 2 | 0x10 | kernel 資料 | 0x92 |

載入:`lgdt` 後,資料區段可直接 `mov`;**`cs` 不能 `mov`,必須用遠跳躍(`ljmp $0x08, $1f`)** 才會重新載入。

### IDT 與例外入口

CPU 以向量編號(0~255)查 IDT,在 stack 推入 `eflags`、`cs`、`eip`(部分例外再推**錯誤碼**),然後跳到入口。

兩個麻煩,以巨集解決:

1. **CPU 不告訴入口自己是第幾號**:每個向量各有一個小入口,把編號推上 stack 再跳到共用的 `isr_common`。
2. **有的例外有錯誤碼(8、10~14、17、21、29、30),有的沒有**:沒有的自己補推一個 0,讓 stack 版面統一。

統一後的 trap frame(低位址在前):`pusha` 的 8 個暫存器 → `vector`、`err` → CPU 推入的 `eip`、`cs`、`eflags`。

- 閘類型 `0x8E`:存在、ring 0、32 位元中斷閘。
- 目前只安裝 0~31(CPU 例外);32~255 留待硬體中斷步驟。
- 同權限等級發生例外時 CPU **不推入** `esp`/`ss`,出錯當下的 esp 以「`eflags` 之後的位址」推算。

### trap 與 fault 的差別

| 類別 | 存入的 eip | 範例 | 返回後 |
|---|---|---|---|
| fault | 指向**出錯指令本身** | #UD、#GP、#PF | 重新執行該指令(修好原因才有意義) |
| trap | 指向**下一條指令** | #BP(`int3`) | 繼續往下 |

所以 `int3` 處理後直接返回即可,不用像 RISC-V(`sepc += 長度`)、AArch64(`elr += 4`)那樣手動跳過。

### 踩坑與注意

- **未開分頁時沒有記憶體存取例外**:讀寫任何 4GB 內的位址都不會出錯。測試改用保護錯誤:載入超出 GDT 範圍的選擇子(`0x38`)→ #GP,錯誤碼即該選擇子。真正的 page fault 要等分頁步驟。
- **`-mgeneral-regs-only`**:kernel 不使用 x87/SSE(尚未初始化 FPU,也免得例外入口要存浮點狀態)。
- **`cld`**:呼叫 C 之前清除方向旗標,這是 C 呼叫慣例的要求。

---

## 第 7 步總結:三個架構的例外處理對照

| | RISC-V | AArch64 | i386 |
|---|---|---|---|
| 入口設定 | `stvec` | `VBAR_EL1` | IDT(`lidt`),需先有 GDT |
| 入口數量 | 1 個 | 16 個(向量表) | 每個向量 1 個(256) |
| 原因 | `scause` | `ESR_EL1` 的 EC | 向量編號 + 錯誤碼 |
| 返回位址 | `sepc` | `ELR_EL1` | stack 上的 `eip` |
| 出錯位址 | `stval` | `FAR_EL1` | `CR2`(僅 page fault) |
| 狀態 | `sstatus` | `SPSR_EL1` | stack 上的 `eflags` |
| 返回指令 | `sret` | `eret` | `iret` |
| 硬體自動存 | 只存到 CSR | 只存到系統暫存器 | 推入 stack |
| 中斷點 | `ebreak`,要 `sepc += 2或4` | `brk`,要 `elr += 4` | `int3`,不用調整 |
| 檔案 | `trap.S`/`trap.c`/`trap.h` | 同左 | 同左(含 GDT/IDT) |

三者的 C 介面一致:`trap_init()` 由各自的 `arch_start()` 呼叫;錯誤最後都走 `arch_backtrace()` 與 `panic_halt()`。

### 跨架構的測試(`crash=` 開機參數)

原本的 `crash=null`(讀位址 0)只在 RISC-V 會出錯,因此改為 `crash=fault`,由 `arch_test_fault()` 以各架構可行的方式觸發:

| 架構 | `crash=fault` 的做法 | 得到的例外 |
|---|---|---|
| riscv64 | 讀位址 0 | Load access fault |
| aarch64 | 讀超出實體位址範圍的位址 | Data abort |
| i386 | 載入無效的區段選擇子 | General protection fault (#GP) |

同理 `arch_test_illegal()`、`arch_test_breakpoint()` 也由各架構實作。

### 驗證矩陣(QEMU 結束碼)

| | 無 | break | panic | illegal | fault |
|---|---|---|---|---|---|
| i386 | 0 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 1 | 1 | 1 |

- 0 = 成功;非 0 = 失敗(i386 為 3 是因為 `isa-debug-exit` 的結束碼為 `(N<<1)|1`)。
- `break` 三者都印出 `resuming` 與 `survived breakpoint`,證明「存檔 → 處理 → 還原 → 返回」正確。
- 三個架構的 backtrace 經 `scripts/backtrace.sh` 解析後完全一致:`arch_test_xxx → crash_test → kmain → arch_start → _start`。

測試方式:

```
for c in break panic illegal fault; do
  bash scripts/qemu.sh aarch64 build/aarch64/welyos.elf -append "autoexit crash=$c"; echo "rc=$?"
done
```

---

## 觀念:中斷點為何要調整返回位址(sepc += 2 或 4、elr += 4)

關鍵問題:**例外發生時,CPU 存下來的返回位址指向哪裡?**

### RISC-V、AArch64:存的是「引發例外的指令本身」

同步例外一律把引發例外的那條指令位址存進 `sepc` / `ELR_EL1`。這是為了「修好原因再重試」:例如 page fault,kernel 補上缺的分頁後返回,CPU 重新執行同一條指令就會成功。

但中斷點不是可以修好的錯誤,`ebreak` / `brk` 指令本身就是原因:

```
sepc / elr → ebreak / brk     ← 返回位址指向這裡
              下一條指令
```

處理完若直接返回,CPU 會再執行一次同一條指令 → 再進例外 → 再返回,形成**無限迴圈**。所以處理程式必須自己把返回位址往後推,跳過這條指令。

### 為什麼 RISC-V 是 2 或 4,AArch64 固定是 4

要跳過一條指令,必須知道它的長度:

- **AArch64**:所有指令固定 4 位元組 → `elr += 4`。
- **RISC-V**:有壓縮指令擴充(C extension;我們以 `rv64imac` 編譯,其中的 `c`)。常用指令有 2 位元組的短版本,`ebreak` 也有(`c.ebreak`),組譯器會自動選用。因此要先判斷長度:**指令最低 2 位元為 `11` 表示 4 位元組,否則為 2 位元組**。

  ```c
  uint16_t first = *(volatile uint16_t *)tf->sepc;
  tf->sepc += ((first & 3) == 3) ? 4 : 2;
  ```

  若固定加 4 而實際是 2 位元組的 `c.ebreak`,返回位址會落在下一條指令的中間,CPU 會把錯位的位元組當成指令執行。

### 為什麼 i386 不用調整

x86 把例外分成兩類,由硬體決定存哪個位址:

| 類別 | 存入的 eip | 例子 | 返回後 |
|---|---|---|---|
| fault | 出錯指令本身 | #PF、#GP、#UD | 重新執行(修好原因才有意義) |
| trap | **下一條指令** | #BP(`int3`) | 繼續往下 |

`int3` 屬於 trap,CPU 已存好下一條指令的位址,直接 `iret` 即可。

### 小結

| 架構 | 中斷點返回位址指向 | 處理方式 | 指令長度 |
|---|---|---|---|
| RISC-V | `ebreak` 本身 | `sepc += 2 或 4` | 2(壓縮)或 4 |
| AArch64 | `brk` 本身 | `elr += 4` | 固定 4 |
| i386 | `int3` 的下一條 | 不用調整 | (硬體已處理) |

RISC-V 與 AArch64 把「要不要跳過」交給軟體決定;x86 由硬體依例外類別決定。同樣的道理日後也適用於系統呼叫:RISC-V 的 `ecall` 返回前要 `sepc += 4`,否則會重複執行系統呼叫(AArch64 的 `svc` 則由硬體存下一條指令的位址)。

---

## 第 8 步:計時器中斷

**目標**:kernel 有時間的概念。硬體計時器每秒產生 100 次中斷,每次累加 tick 計數。這是排程器(定時切換工作)的前置條件。

### 例外與中斷的差別

| | 例外(同步) | 中斷(非同步) |
|---|---|---|
| 來源 | 目前執行的指令本身 | 外部裝置(計時器、鍵盤…) |
| 時機 | 可預期,每次執行到都發生 | 隨時,與正在執行的指令無關 |
| 返回位址 | 引發例外的指令(中斷點除外,見前一節) | **被打斷、尚未執行的指令** |
| 處理後 | 視情況調整返回位址 | 直接返回,不用調整 |
| 可否關閉 | 不可 | 可(全域開關 + 個別遮罩) |

中斷處理共用例外的入口與 trap frame(第 7 步),只是多了「判斷是哪個中斷」這一層。

### 共用層

| 檔案 | 內容 |
|---|---|
| `kernel/timer.c`、`include/timer.h` | `timer_init(hz)`、`timer_tick()`(架構層每次中斷呼叫)、`timer_ticks()`、`timer_hz()` |
| `include/arch.h` | 新增 `arch_timer_init(hz)`、`arch_irq_enable/disable()`、`arch_idle()` |

- `ticks` 宣告為 `volatile`:它在中斷處理程式中被修改,主程式必須每次重新讀取。
- **32 位元架構的 64 位元讀取**:i386 讀 `uint64_t` 需要兩條指令,中間若發生計時器中斷會讀到高低半不一致的值。`timer_ticks()` 連讀兩次直到相同。
- `arch_idle()`:讓 CPU 休眠到下一次中斷(`hlt` / `wfi`),等待時不空轉。
- `arch_halt()`(panic 用)改為**先關中斷**再停住,否則中斷會繼續把 CPU 叫醒。

`kmain` 啟動 100Hz 計時器、開中斷,印出兩次 `uptime`,之後進入 `arch_idle()` 迴圈。

### 三個架構的實作

| | RISC-V | AArch64 | i386 |
|---|---|---|---|
| 計時器 | SBI timer(`ecall` 請 OpenSBI 設定) | ARM generic timer(`CNTP_*` 系統暫存器) | PIT 8253(port 0x40/0x43) |
| 時脈 | 10 MHz(virt 固定) | 讀 `CNTFRQ_EL0` | 1193182 Hz(固定) |
| 模式 | 單次,每次中斷要重設 | 單次,每次中斷要重設 | **週期性**,設一次即可 |
| 中斷控制器 | 不需要(CPU 內建) | GICv2(distributor + CPU interface) | PIC 8259 ×2 |
| 個別開關 | `sie` 的 STIE 位元 | GIC 的 ISENABLER + 計時器 CTL | PIC 遮罩(port 0x21) |
| 全域開關 | `sstatus.SIE` | `PSTATE.I`(`daifclr #2`) | `eflags.IF`(`sti`) |
| 識別方式 | `scause` = 中斷 + 碼 5 | 讀 GIC 的 IAR 得中斷號 30 | 向量編號 32(= IRQ 0) |
| 處理完畢通知 | 重設計時器即清除 | 寫 GIC 的 EOIR | 對 PIC 寫 EOI(0x20) |
| 檔案 | `arch/riscv64/timer.c` | `arch/aarch64/timer.c` | `arch/i386/timer.c` |

**RISC-V:為什麼要透過 SBI**

計時器(`mtime`/`mtimecmp`)屬於 M-mode,S-mode 的 kernel 不能直接設定。以 `ecall` 請 M-mode 韌體(OpenSBI)代勞,這套介面稱為 SBI:`a7` = 擴充 ID(`0x54494D45`,即 "TIME")、`a6` = 功能 ID、`a0` = 參數。流程是「讀目前時間 → 設定下一次觸發時間 → 中斷時再設下一次」。

**AArch64:為什麼需要 GIC**

ARM 的週邊中斷都要經過中斷控制器才會送到 CPU:

```
計時器 --(中斷號 30)--> GIC distributor --> GIC CPU interface --> CPU 的 IRQ
```

- Distributor(`0x08000000`):每個中斷號的開關與優先權。
- CPU interface(`0x08010000`):讀 `IAR` 認領中斷(得到中斷號),處理完寫 `EOIR`。沒寫 `EOIR`,GIC 不會再送下一個。
- EL1 實體計時器是中斷號 30(每個 CPU 私有的 PPI)。

**i386:為什麼要重新對映 PIC**

開機時 BIOS 把 IRQ 0~7 對應到向量 8~15,但保護模式下向量 0~31 保留給 CPU 例外(8 是 double fault)。不改的話,計時器中斷會被當成 double fault。所以把 IRQ 0~15 改對應到向量 32~47,並在 IDT 增加這 16 個入口(沿用第 7 步的 `ISR_NOERR` 巨集)。

- 初始化要連續寫入 ICW1~ICW4 四個命令字。
- 遮罩設為 `0xFE`/`0xFF`:只開 IRQ 0。
- 中斷閘進入時 CPU 自動清除 IF(關中斷),`iret` 還原 `eflags` 時重新開啟。

### 驗證

三個架構輸出相同:

```
timer: 100 Hz
uptime: 1 s (ticks=100)
uptime: 2 s (ticks=200)
```

| 架構 | tick 數 | 實測總耗時(含開機) |
|---|---|---|
| i386 | 100 / 200 | 2.27 秒 |
| riscv64 | 100 / 200 | 2.42 秒 |
| aarch64 | 100 / 200 | 2.40 秒 |

- tick 數精確,總耗時與實際時間吻合(2 秒計時 + 開機),表示頻率設定正確。
- 兩秒內處理了 200 次中斷,期間主程式的 `kprintf` 與迴圈都正常,顯示中斷的暫存器存檔與還原正確。
- 第 7 步的 15 個例外測試情境全數通過(回歸)。

### 已知限制

- RISC-V 的 timebase 頻率寫死為 10MHz(QEMU virt);實機應從裝置樹 `/cpus` 的 `timebase-frequency` 讀取。
- GIC 與 UART 的位址寫死為 QEMU virt 的值;實機應從裝置樹讀取。
- AArch64 假設為 GICv2(virt 的預設);GICv3 需另外實作。
- PIT 除數為 16 位元,頻率下限約 19Hz;實際頻率為 1193182/11931 ≈ 100.007Hz,有極小誤差。
- `kprintf` 不可重入:中斷處理程式與主程式若同時輸出會交錯(目前中斷中不輸出)。

---

## 第 9 步:實體記憶體管理與 kernel heap

到上一步為止,kernel 用到的記憶體都是編譯時就決定大小的全域變數與 stack。要建立數量不固定的東西(工作、緩衝區、之後的頁表)就需要動態配置。這一步分兩層做:

| 層 | 介面 | 單位 | 檔案 |
|---|---|---|---|
| 實體記憶體管理員(PMM) | `pmm_alloc()` / `pmm_alloc_pages(n)` / `pmm_free()` | 頁(4KB) | `kernel/pmm.c` |
| kernel heap | `kmalloc()` / `kzalloc()` / `kfree()` | 位元組 | `kernel/kmalloc.c` |

`kmalloc` 不夠用時向 PMM 要頁,PMM 則直接管理整塊主記憶體。兩層都與架構無關,三個架構共用同一份程式。

### 為什麼分兩層

- **頁是硬體的單位**。之後開分頁(MMU)時,對映、保護都以頁為單位,所以一定需要一個「給我一頁」的配置器。
- **頁對小物件太浪費**。配置一個 24 位元組的結構不該用掉 4096 位元組,所以上面再疊一層把頁切成小塊。
- Linux 也是這個結構(buddy allocator 之上是 slab/`kmalloc`),只是每層的演算法更複雜。

### 哪些記憶體可以用

開機資訊只告訴我們主記憶體的範圍(`mem_base`、`mem_size`),但裡面有些地方已經有東西:

| 佔用者 | 怎麼知道位置 |
|---|---|
| kernel 映像(程式、資料、BSS、開機 stack) | linker script 新增的 `__kernel_start`、`__kernel_end` 符號 |
| kernel 之前的區域(RISC-V 的 OpenSBI 在 `0x80000000`~`0x80200000`;AArch64 的 RAM 開頭) | 規則:低於 `__kernel_end` 的一律不用 |
| 裝置樹 DTB(RISC-V / AArch64) | DTB header 的 `totalsize`,加上 DTB 內的記憶體保留表 |
| multiboot info 與開機參數字串(i386) | bootloader 傳入的指標 |
| PMM 自己的位元圖 | 自己決定的 |

後三項由各架構的 `start.c`(或 `fdt_parse`)用 `boot_reserve()` 登記到 `struct boot_info` 的 `reserved[]`,PMM 初始化時避開。

**`__kernel_end` 是怎麼來的**:linker script 裡 `__kernel_end = .;` 把「目前的位置」指定給一個符號。它沒有內容、不佔空間,只有位址有意義,所以 C 裡宣告成 `extern char __kernel_end[];`,用陣列名稱取得位址(若宣告成 `extern char *` 會變成去讀那個位址的內容,是錯的)。

實測各架構的版面(128MB 記憶體):

| | kernel 映像 | 位元圖 | 保留範圍 | 可用頁數 / 總頁數 |
|---|---|---|---|---|
| i386 | `0x100000`~`0x108ac0` | `0x10a000` | `0x9500`(multiboot info)、`0x109000`(開機參數) | 32469 / 32480 |
| riscv64 | `0x80200000`~`0x80207a98` | `0x80208000` | `0x87e00000`(DTB) | 32245 / 32768 |
| aarch64 | `0x40080000`~`0x40089738` | `0x4008a000` | `0x44000000`(DTB,1MB) | 32373 / 32768 |

RISC-V 少了約 520 頁,主要是 OpenSBI 佔的前 2MB(512 頁)。

### PMM:為什麼用位元圖

每頁用 1 個位元記錄(1 = 使用中、0 = 空閒)。128MB = 32768 頁,位元圖只要 4KB。

考慮過的兩種做法:

| | 位元圖(採用) | 空閒頁鏈結串列 |
|---|---|---|
| 做法 | 另外一塊記憶體,每頁一個位元 | 每個空閒頁的開頭存「下一個空閒頁」的指標 |
| 配置一頁 | 要掃描,O(頁數) | 取串列頭,O(1) |
| 配置連續多頁 | 找連續的 0 即可 | 很難(串列沒有位址順序) |
| 偵測重複釋放 | 可以(位元已經是 0) | 不行 |
| 額外空間 | 需要(4KB) | 不需要 |

選位元圖的理由是需要連續多頁(heap 一次擴充數頁、之後的 kernel stack),而且能抓到重複釋放。掃描慢的問題用一個 `hint`(最低的可能空閒頁)減輕:配置都從 `hint` 開始找,不必每次從頭。

初始化的順序值得注意:**先把所有位元設成 1(使用中),再把確定可用的頁逐一清成 0**。反過來做(先全部空閒再標記佔用)的話,只要漏標一個範圍,別人的資料就會被配置出去並覆蓋;現在的做法漏判時只是少用幾頁,比較安全。

### kmalloc:first-fit 空閒串列

每個區塊開頭有一個標頭,記錄大小與狀態,標頭之後才是交給呼叫者的空間:

```
+--------+----------------------+--------+-------------+
| 標頭   | 使用者資料           | 標頭   | 使用者資料  | ...
+--------+----------------------+--------+-------------+
         ^ kmalloc 回傳這裡
```

- **配置**:沿空閒串列找第一個夠大的區塊(first-fit)。太大就切成兩塊,後半塊留在串列。
- **不夠**:向 PMM 要頁(一次至少 4 頁,減少要的次數),當成新的空閒區塊。
- **釋放**:由指標往回減標頭大小找到標頭,放回空閒串列。
- **合併(coalescing)**:空閒串列依位址排序,釋放時檢查前後鄰居是否緊鄰,是就併成一塊。不合併的話,用久了會碎成很多小塊,總量夠卻配不出大塊(外部破碎)。

**對齊**:回傳位址固定對齊到 16 位元組,這是三個架構上所有基本型別都安全的對齊。標頭結構用 `_Alignas(16)` 宣告,`sizeof` 因此是 16 的倍數(i386 為 16、64 位元為 32),標頭之後的位址自然對齊。

**錯誤偵測**:標頭有一個 `magic` 欄位,使用中是 `"USED"`、空閒是 `"FREE"`。`kfree` 看到不是 `USED` 就 panic,可以抓到:重複釋放、傳入不是 `kmalloc` 給的指標、標頭被前一塊越界寫入破壞。這類錯誤若不當場攔下,會變成很久以後才在別處出現的資料損毀,非常難查。

### 斷言 `KASSERT`

`include/panic.h` 新增 `KASSERT(條件)`:條件不成立就 panic 並印出條件與原始碼位置。用在「理論上不可能發生」的地方,讓錯誤在最接近原因的位置爆出來。

### 踩坑:i386 的開機參數就在 kernel 後面

位元圖最直覺的位置是 kernel 映像的正後方。但 QEMU 的 multiboot 載入器把開機參數字串放在同一個地方:

```
pmm: kernel 0x00100000-0x00108ac0, bitmap 0x0010a000
pmm: reserved 0x109000-0x10903a        <- 開機參數字串
```

kernel 結束於 `0x108ac0`,下一頁 `0x109000` 就是開機參數。如果直接把位元圖放在那裡,`memset` 位元圖時會把 `autoexit`、`crash=` 這些參數蓋掉,症狀會是「參數突然失效」,而且和記憶體管理看起來毫無關係。

處理方式:i386 的 `start.c` 把開機參數登記為保留範圍;PMM 找位元圖位置時,碰到保留範圍就跳到它後面再重新檢查。上面的輸出中位元圖因此落在 `0x10a000`。RISC-V 與 AArch64 的開機參數在 DTB 裡,DTB 離 kernel 很遠,但同樣有登記。

### 驗證

每次開機都會跑 `mem_selftest()`(`kernel/main.c`),任何一項不符就 panic:

- PMM:連續兩次配置得到相鄰的頁;釋放後再配置拿回同一頁;連續 8 頁可配置且整段可寫;全部釋放後空閒頁數回到原值。
- kmalloc:24、100、5000 位元組(超過一頁)都能配置,位址對齊 16;各自寫滿後互不覆蓋;釋放 100 位元組那塊後再要 64 位元組會拿到同一個位址;`kzalloc` 內容為 0;全部釋放後空閒串列合併回 **1 塊**,且大小等於 heap 總量。

輸出(RISC-V):

```
pmm: kernel 0x0000000080200000-0x0000000080207a98, bitmap 0x0000000080208000 (4096 bytes)
pmm: reserved 0x87e00000-0x87e016d1
pmm: 32768 pages total, 32245 free (128980 KB)
pmm: selftest ok (first page 0x0000000080209000)
kmalloc: p1=0x0000000080209020 p2=0x0000000080209060 p3=0x00000000802090f0, heap 16384 bytes, 11088 free in 2 block(s)
kmalloc: selftest ok (all freed, coalesced into 1 block)
```

從位址可以看出版面:`p1` 在頁開頭 `+0x20`(前面是 32 位元組的標頭);`p2 - p1 = 0x40` = 24 對齊成 32,再加 32 的標頭。

另外新增兩個開機參數:

| 參數 | 作用 | 結果 |
|---|---|---|
| `memtest` | 把所有空閒頁配置出來,每頁寫入與位址相關的值再讀回,最後全部釋放;記憶體用完時 `kmalloc` 要回傳 `NULL` | i386 32465 頁、riscv64 32241 頁、aarch64 32369 頁全數通過 |
| `crash=dfree` | 同一個指標 `kfree` 兩次 | 三個架構都 panic:`kfree: invalid pointer or double free` |

`memtest` 的意義是確認 PMM 認為可用的每一頁真的是可讀寫的 RAM,而且沒有任何一頁和 kernel、位元圖、DTB 重疊(重疊的話寫入後 kernel 會壞掉或驗證失敗)。頁數比空閒總數少 4,是 heap 已經拿走的 4 頁。

回歸矩陣(QEMU 結束碼),共 21 個情境全數通過:

| 架構 | 無 | `memtest` | `crash=dfree` | `crash=break` | `crash=panic` | `crash=illegal` | `crash=fault` |
|---|---|---|---|---|---|---|---|
| i386 | 0 | 0 | 3 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 1 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 1 | 0 | 1 | 1 | 1 |

### 已知限制

- 這一步完成時 PMM 與 kmalloc 都沒有加鎖;第 10 步加入排程器時已補上關中斷保護。
- heap 向 PMM 拿的頁不會歸還。
- first-fit 與位元圖掃描都是線性時間,物件多了會慢;Linux 用 buddy 加 slab 解決。
- 只讀裝置樹 `/memory` 的第一組範圍,記憶體分成多段的機器只會用到第一段。
- 沒有解析裝置樹的 `/reserved-memory` 節點,目前靠「kernel 之前的都不用」這條規則避開 OpenSBI。
- i386 用 multiboot 的 `mem_upper`(1MB 以上的連續大小),沒有讀完整的記憶體分布表(mmap),也不使用 1MB 以下的記憶體。
- `kfree` 只檢查標頭的 magic,緩衝區溢位若沒有寫到下一塊的標頭就偵測不到。

---

## 第 10 步:核心執行緒與排程器

有了計時器(第 8 步)和動態記憶體(第 9 步),就能讓 kernel 同時進行多件事。這一步加入**核心執行緒**(kernel thread)和 **round-robin 排程器**,支援主動讓出與計時器搶佔。

| 檔案 | 內容 |
|---|---|
| `include/sched.h`、`kernel/sched.c` | 排程器本體,與架構無關 |
| `arch/<名稱>/switch.S` | context switch,每個架構約 20 行組合語言 |
| `arch/<名稱>/arch.c` | `arch_stack_init`(新執行緒的初始 stack)、`arch_irq_save` / `arch_irq_restore` |

### 執行緒是什麼

一條獨立的執行流程,有**自己的 stack 和暫存器狀態**,但和其他執行緒共用同一份記憶體(全域變數、heap)。目前都在 kernel 權限下執行,還沒有使用者模式,也沒有各自的位址空間(那是「行程」,要等分頁)。

每條執行緒用一個 `struct thread` 記錄,最重要的欄位只有一個:`sp`,被切換出去時的 stack pointer。

### context switch 就是換 stack

這是這一步最核心的觀念。`arch_switch(&prev->sp, next->sp)` 做四件事:

1. 把目前需要保留的暫存器推到**目前的 stack** 上
2. 把 stack pointer 存進 `prev->sp`
3. 把 stack pointer 換成 `next->sp`
4. 從**新的 stack** 彈出暫存器,然後 `ret`

第 4 步的 `ret` 會回到 `next` 上次呼叫 `arch_switch` 的地方。對 `next` 來說,就像它當初那次函式呼叫現在才返回。所以從 C 的角度看,`arch_switch` 是一個「呼叫之後,要等別人切回來才會返回」的函式。

**為什麼只存一部分暫存器**:`arch_switch` 是用一般函式呼叫進來的。C 的呼叫慣例把暫存器分成兩類,caller-saved 的那些,編譯器本來就假設函式呼叫後會被改掉,已經自己處理了。所以只需要存 callee-saved 的:

| 架構 | 要存的暫存器 | 大小 |
|---|---|---|
| i386 | `ebx`、`esi`、`edi`、`ebp`(返回位址由 `call` 推上 stack) | 16 位元組 |
| riscv64 | `ra`、`s0`~`s11` | 112 位元組 |
| aarch64 | `x19`~`x28`、`x29`、`x30` | 96 位元組 |

對照第 7 步的 trap frame(288 位元組,所有暫存器):例外和中斷可能發生在任何一條指令上,被打斷的程式沒有機會先保存,所以必須全存。context switch 是自己呼叫的,只需存一小部分。

i386 的返回位址在 stack 上(`call` 推入);RISC-V 和 AArch64 的返回位址在暫存器(`ra`、`x30`),所以要明確存起來。

### 新執行緒怎麼開始

新執行緒從來沒呼叫過 `arch_switch`,stack 上沒有可以彈出的東西。做法是**預先在新 stack 上偽造一份**,內容就像它曾經被切換出去一樣,只是「返回位址」填的是進入函式。`arch_stack_init()` 負責這件事。

第一次切換過去時,`arch_switch` 照常彈出暫存器、`ret`,就「返回」到了 `thread_entry()`:

```c
static void thread_entry(void)
{
    reap();                  /* 替剛結束的上一條執行緒收尾 */
    arch_irq_enable();       /* schedule() 是關著中斷切過來的 */
    current->fn(current->arg);
    thread_exit();
}
```

偽造時 frame pointer 填 0,返回位址也安排成 0(RISC-V 和 AArch64 多用一小段 `arch_thread_start` 把 `ra` / `x30` 清 0 再跳進去),這樣 backtrace 走到執行緒的起點就會停下來。

### 排程:round-robin

- **執行佇列**是一個先進先出的串列,裝著所有 READY 的執行緒。`schedule()` 把目前的執行緒放到隊尾,從隊頭取下一條來跑。
- **時間片**是 5 個 tick(50ms)。每個計時器 tick 扣 1,扣到 0 且有人在等就換人。
- **idle 執行緒**不進佇列,只有佇列空的時候才跑,內容是 `arch_idle()` 的迴圈,讓 CPU 休眠等中斷。
- **main 執行緒**就是開機流程本身。`sched_init()` 只是補一個 `struct thread` 給它,繼續用 `boot.S` 設定的 stack。

執行緒的狀態:

| 狀態 | 意義 | 在哪裡 |
|---|---|---|
| RUNNING | 正在用 CPU | `current` |
| READY | 等待輪到 | 執行佇列 |
| SLEEPING | 等時間到 | 休眠串列 |
| ZOMBIE | 已結束,等待回收 | `zombie` |

### 主動讓出與搶佔走同一條路

| | 主動 | 搶佔 |
|---|---|---|
| 觸發 | 執行緒呼叫 `thread_yield` / `thread_sleep` / `thread_exit` | 計時器中斷發現時間片用完 |
| 呼叫 `schedule()` 的位置 | 執行緒自己的程式 | 中斷處理程式的最後(`sched_irq_exit`) |
| 被打斷處的暫存器 | 不需要另外存(是函式呼叫) | 中斷入口已存在**該執行緒自己的 stack** 上(trap frame) |
| 切回來之後 | 從 `thread_yield` 返回 | 從中斷處理程式返回,`iret` / `sret` / `eret` 還原 trap frame |

搶佔不需要特別的機制,原因在第三列:每條執行緒有自己的 stack,中斷時的 trap frame 就留在那條 stack 上。切走再切回來,stack 上的東西原封不動,照正常流程從中斷返回即可。

搶佔發生時的 stack(由高位址到低位址):

```
執行緒原本的函式呼叫 ...
trap frame(中斷入口存的全部暫存器)
trap_handler → irq_handler → sched_irq_exit → schedule 的 frame
arch_switch 存的 callee-saved 暫存器      <- thread->sp 指向這裡
```

### 中斷狀態的規則

`schedule()` 一律在**中斷關閉**時執行(佇列正在被修改,不能被打斷)。切回來的執行緒各自還原自己的中斷狀態:

- 主動讓出的:`thread_yield` 結尾的 `arch_irq_restore(flags)`
- 被搶佔的:中斷返回指令會還原當時的狀態(中斷是開的,不然不會被打斷)
- 新執行緒:`thread_entry` 裡的 `arch_irq_enable()`

新增的 `arch_irq_save()` / `arch_irq_restore()` 是「關中斷,之後恢復**原本的**狀態」。不直接用 disable / enable 的原因:如果進來之前中斷本來就是關的(例如在中斷處理程式裡呼叫 `kmalloc`),結束時不可以擅自打開。

### 誰來釋放結束的執行緒

執行緒不能釋放自己的 stack,因為它還踩在上面。做法是 `thread_exit()` 把自己標成 ZOMBIE 並記在 `zombie` 變數,切到下一條執行緒之後,由**下一條**呼叫 `reap()` 釋放。`reap()` 放在 `arch_switch` 返回之後,以及新執行緒的 `thread_entry` 開頭,這兩個地方涵蓋了「切換過去」的所有可能。

### 連帶修改

- **`kmalloc` / `pmm` 加上關中斷保護**。有了搶佔,執行緒可能在修改空閒串列到一半時被切走;排程器也會在中斷裡呼叫 `kfree`(回收 zombie)。單一 CPU 上,關中斷就足以保證不被插入。
- **`kprintf` 整個呼叫期間關中斷**,一次輸出不會被別的執行緒從中間插入。
- **`panic()` 一開始就關中斷**,避免印到一半被搶佔、其他執行緒繼續跑。
- **backtrace 的 stack 範圍檢查**原本寫死開機 stack,改為問排程器「目前執行緒的 stack 範圍」。
- **中斷處理程式的順序**:`sched_irq_exit()` 必須在通知中斷控制器(PIC / GIC 的 EOI)**之後**呼叫,理由見下面的踩坑。

### stack 溢位偵測

每條執行緒的 stack 最底端放一個記號(`0x57454C59`,"WELY")。stack 往低位址成長,用過頭會先蓋掉它。`schedule()` 每次切換前檢查,不對就 panic。

這是機率性的偵測,有兩個限制:只有切換時才檢查,溢位到發現之間已經寫壞了別人的記憶體;而且若溢位的程式沒有寫到那 4 個位元組(例如宣告了大陣列但只用一小部分,直接「跨過」記號),就偵測不到。可靠的做法是在 stack 下方放一個不對映的保護頁(guard page),一碰到就觸發例外,這要等分頁。

### 踩坑

**1. 被喚醒的執行緒插隊,造成不公平**

第一版在計時器 tick 喚醒休眠者時,會立刻要求重新排程。搶佔測試的結果:

```
spin1=4191312 spin2=9769981
```

兩條一樣的忙碌迴圈,執行量差了 2.3 倍。原因是 main 每 20ms 醒來一次檢查進度,每次醒來都強制打斷正在跑的那條。打斷後的順序固定是「另一條 → main → 被打斷的那條」,結果一條每次只跑到 2 個 tick 就被打斷,另一條每次都跑滿 5 個 tick,比例 2:5 正好是 2.3 倍。

修正:被喚醒的執行緒排到隊尾等待輪到,不強制換人;只有 CPU 正在跑 idle 時才立刻切換。修正後:

```
spin1=6765576 spin2=6805917
```

代價是 `thread_sleep` 的喚醒可能延遲到別人的時間片用完。真正的系統用優先權解決這個取捨(互動型的工作優先權高,醒來可以搶佔),目前沒有優先權。

**2. EOI 要在切換之前送**

i386 的 PIC 和 AArch64 的 GIC 都要求處理完中斷後通知它(EOI),它才會送下一個。如果在通知之前就 `schedule()` 切到別的執行緒,原本的執行緒可能很久才回來補送 EOI,這段期間計時器中斷完全停擺,系統看起來像當掉。所以順序固定是:處理中斷 → EOI → `sched_irq_exit()`。RISC-V 沒有中斷控制器要通知,但同樣要先設好下一次鬧鐘再切換。

**3. i386 的 64 位元除法**

`thread_sleep` 把毫秒換算成 tick,一開始用 `uint64_t` 運算,i386 連結失敗:

```
undefined reference to `__udivdi3'
```

32 位元 CPU 沒有 64 位元除法指令,編譯器會呼叫 libgcc 的 `__udivdi3`,而 freestanding 環境沒有連結 libgcc(和第 3 步 `kprintf` 遇到的是同一件事)。這裡數值不大,改用 32 位元運算即可。

**4. 溢位測試跨過了記號**

`crash=stack` 的第一版每層遞迴宣告 512 位元組陣列但只寫第一個位元組,AArch64 上剛好沒有任何寫入落在記號上,溢位了卻沒被偵測到(i386 和 RISC-V 是碰巧蓋到)。測試改成把陣列寫滿。這也實際示範了上面說的偵測限制。

### 驗證

三個示範每次開機都會跑(`kernel/main.c` 的 `sched_demo()`):

```
sched: cooperative (yield)
  [A] step 1
  [B] step 1
  [C] step 1
  [A] step 2
  [B] step 2
  [C] step 2
  [A] step 3
  [B] step 3
  [C] step 3
sched: preemptive (busy loops, no yield)
  spin1=6765576 spin2=6805917, 21 context switches
sched: sleep
  [s100] woke after 100 ms (tick 69)
  [s200] woke after 200 ms (tick 79)
  [s300] woke after 300 ms (tick 89)
sched: all threads exited, memory reclaimed
uptime: 92 ticks, 80 context switches
```

- **主動讓出**:三條執行緒每印一行就 `thread_yield()`,輸出整齊地 A、B、C 輪流,證明佇列是先進先出、暫存器與 stack 在切換後正確還原。
- **搶佔**:兩條執行緒是完全不讓出 CPU 的忙碌迴圈。兩個計數器都在增加而且接近(各次執行的差距在 0.6%~3% 之間),證明計時器中斷確實把 CPU 從一條手上拿走交給另一條。0.5 秒內共切換 21 次,和 50ms 時間片的量級相符(0.5 秒約 10 個時間片,其餘是 main 定期醒來檢查進度的進出)。程式用 `KASSERT` 檢查兩者差距在 3 倍以內。
- **休眠**:建立順序是 300、100、200 毫秒,醒來順序是 100、200、300,間隔剛好 10 個 tick。
- **回收**:所有示範執行緒結束後,執行緒數回到 1、PMM 空閒頁數與 heap 空閒量都回到示範前的值,證明 stack 和 `struct thread` 都有釋放。

新增兩個開機參數:

| 參數 | 作用 | 結果 |
|---|---|---|
| `crash=stack` | 執行緒遞迴到超出 16KB | panic:`stack overflow in thread 1 (overflow)` |
| `crash=thread` | 在執行緒裡 panic | backtrace 正確走訪該執行緒的 stack:`fault_worker` ← `thread_entry`,然後結束 |

回歸矩陣(QEMU 結束碼),三個架構各 9 個情境,共 27 個全數通過。另外每個架構連續執行 10 次正常開機,30 次全部通過(搶佔的時機每次不同,用來確認沒有偶發的競爭問題)。

| 架構 | 無 | `memtest` | `stack` | `thread` | `dfree` | `break` | `panic` | `illegal` | `fault` |
|---|---|---|---|---|---|---|---|---|---|
| i386 | 0 | 0 | 3 | 3 | 3 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |

### 已知限制

- 只支援單一 CPU。「關中斷就安全」的前提在多核心上不成立,那時需要 spinlock。
- 沒有優先權,所有執行緒平等輪流;`thread_sleep` 的喚醒可能延遲最多一輪時間片。
- 這一步完成時沒有同步機制與 `thread_join`,示範裡 main 用輪詢等待;休眠串列也沒有排序。兩者都在第 11 步補上。
- `kprintf` 輸出期間關中斷,輸出很長時會延遲中斷處理。
- stack 溢位偵測是機率性的(見上)。
- 沒有儲存浮點暫存器,kernel 也不使用浮點(編譯時已用 `-mgeneral-regs-only` 等選項禁止)。

---

## 觀念:執行緒數量的上限,以及排程器還能怎麼更有效率

### 示範只有三條,但上限是記憶體

第 10 步的示範各用 2~3 條執行緒,只是為了讓輸出容易閱讀。排程器本身沒有數量上限:`struct thread` 用 `kmalloc` 配置、stack 向 PMM 要,執行佇列是鏈結串列,沒有任何固定大小的表。

每條執行緒的成本是 16KB 的 stack 加一個 `struct thread`(i386 約 60 位元組、64 位元約 110 位元組)。128MB 記憶體理論上可以放約 7900 條。

開機參數 `stress` 實際建立 1000 條,每條讓出 CPU 3 次後結束(三個架構結果相同):

```
stress: created 1000 threads, 16112 KB used (stacks + thread structs)
stress: all finished in 4 ticks, 4004 context switches
stress: ok (stacks reclaimed, heap grew by 112 KB)
```

4004 次切換在 4 個 tick(40ms)內完成,平均每次切換加上排程約 10 微秒,而且這是在 QEMU 的軟體模擬之下,真實硬體會快得多。結束後 1000 條 stack 全部歸還給 PMM。

### 目前的做法哪裡快、哪裡慢

| 操作 | 目前的成本 | 說明 |
|---|---|---|
| 選下一條來跑 | O(1) | 從佇列頭取一個,和執行緒總數無關 |
| context switch | 固定 | 存取 4~13 個暫存器,已經接近最少 |
| 每個 tick 檢查休眠者 | **O(休眠數)** | 走訪整個休眠串列 |
| 建立執行緒 | **O(頁數)** | PMM 掃描位元圖找連續 4 頁 |
| 等待別人完成 | **浪費** | main 每 20ms 醒來看一次計數器 |

所以切換本身已經不是瓶頸,值得改進的是後三項。

### 可以改進的方向(由效益高到低)

**1. 用「等待佇列」取代輪詢(效益最大)**

現在 main 等其他執行緒結束的方式是每 20ms 醒來檢查一次。這有兩個浪費:沒事也要醒來;事情發生後最多要晚 20ms 才知道。正確做法是讓等待者睡在一個佇列上,事件發生時由對方直接叫醒它,這就是 semaphore、mutex、`thread_join` 的基礎,也是所有阻塞式 I/O 的做法。這是下一步要做的。

**2. 休眠串列依喚醒時間排序**

排序後每個 tick 只需要看串列的第一個,時間還沒到就可以直接結束,成本從 O(休眠數) 降為 O(1)。插入時要找位置,是 O(休眠數),但插入遠比 tick 少。更進一步是 timer wheel(Linux 使用)或 min-heap。

**3. 回收 stack 重複使用**

執行緒結束時不把 stack 還給 PMM,而是放進一個快取串列,下次建立直接拿來用,省掉位元圖掃描。`struct thread` 也可以用同樣的方式(這就是 slab allocator 的概念)。

**4. 優先權**

Round-robin 把所有執行緒視為相同。實際上互動型的工作(等鍵盤、等網路)大多在睡覺,醒來時應該優先處理,否則反應會遲鈍;第 10 步「被喚醒者要不要插隊」的取捨就是這個問題。常見做法:

- **多層優先權佇列**:每個優先權一個佇列,再用一個位元圖記錄哪些佇列非空,用一條「找最高位元」的指令就能在 O(1) 選出下一條。
- **MLFQ(多層回饋佇列)**:自動調整,用完整個時間片的降級、提早讓出的升級。
- **CFS / EEVDF(Linux)**:記錄每條執行緒已使用的 CPU 時間,永遠選用得最少的那條,用紅黑樹維持順序。

**5. 不需要時停掉計時器(tickless)**

現在不論有沒有事,每秒固定被中斷 100 次。如果所有執行緒都在睡,可以把計時器直接設成「最早的喚醒時間」,中間完全不中斷。RISC-V 和 AArch64 的計時器本來就是單次鬧鐘,很適合這樣做。主要好處是省電。

**6. 多核心**

真正讓效能倍增的是用上多顆 CPU。這需要每顆 CPU 自己的執行佇列(避免搶同一把鎖)、work stealing(閒的 CPU 去別人的佇列拿工作)、spinlock。代價是目前「關中斷就安全」的假設全部要重新檢視,是很大的一步。

### 目前的選擇

現階段維持 round-robin。理由:它的行為最容易預測和驗證,而上面第 1 項(等待佇列)對實際效率的影響遠大於更換排程演算法。等到有了使用者程式和 I/O,執行緒的行為出現差異之後,優先權才有意義。

---

## 第 11 步:等待佇列、semaphore、mutex、thread_join

第 10 步結束時,執行緒之間沒有正式的合作方式:main 靠每 20ms 醒來看一次計數器來等別人結束,共用資料只能靠直接關中斷保護。這一步補上同步機制,同時處理上一節列出的效率問題第 1、2 項。

| 檔案 | 內容 |
|---|---|
| `kernel/sched.c`、`include/sched.h` | 等待佇列(`wq_sleep` / `wq_wake_one` / `wq_wake_all`)、`thread_join`、排序的休眠串列 |
| `kernel/sync.c`、`include/sync.h` | semaphore、mutex |

### 等待佇列:所有阻塞操作的共同基礎

等待佇列就是「在等某件事的執行緒」排隊的地方,只有兩個操作:

- **`wq_sleep(wq)`**:把自己掛進佇列,狀態改成 BLOCKED,呼叫 `schedule()` 讓出 CPU。BLOCKED 的執行緒不在執行佇列裡,完全不會被排到。
- **`wq_wake_one(wq)`**:從佇列取出等最久的一條,放回執行佇列。

semaphore、mutex、`thread_join` 都只是「一個條件」加「一個等待佇列」。之後的鍵盤輸入、磁碟讀寫也會是同樣的結構:沒資料就睡在佇列上,中斷處理程式收到資料時喚醒。

和輪詢比較:

| | 輪詢(`thread_sleep` 迴圈) | 等待佇列 |
|---|---|---|
| 沒事的時候 | 仍然定期醒來檢查 | 完全不執行 |
| 事件發生後多久知道 | 最多晚一個輪詢週期 | 立刻被放回執行佇列 |

### 兩個必須遵守的寫法

semaphore 和 mutex 都是同一個模式:

```c
unsigned long flags = arch_irq_save();
while (條件不成立)
    wq_sleep(&等待佇列);
取得資源;
arch_irq_restore(flags);
```

**1. 「檢查條件」和「睡下去」之間不能開中斷**

假設可以被打斷,會發生這樣的順序:

1. A 檢查:資源沒有,準備去睡
2. A 被切走;B 釋放資源並喚醒等待者,但此時佇列是空的,沒有人可叫
3. A 回來,照原計畫睡下去
4. 再也沒有人會來叫醒 A

這種錯誤叫 **lost wakeup**。關鍵是 A 的決定(要睡)是根據已經過時的資訊做的。解法是讓檢查和睡下去成為不可分割的一段,這裡用關中斷達成,所以 `wq_sleep` 規定呼叫前必須已關中斷。

**2. 用 `while`,不是 `if`**

被喚醒只代表「條件可能成立了,去看一下」。從被喚醒到真正輪到自己執行之間還有一段時間,其他執行緒可能先把資源拿走。所以醒來後要重新檢查,不成立就再睡。

### semaphore

一個不會小於 0 的計數器:

- `sem_wait`:計數大於 0 就減 1;等於 0 就睡。
- `sem_post`:計數加 1,喚醒一條等待者。

用途有兩種:計算資源數量(緩衝區的空位、待處理的項目),以及通知事件(初值 0,一方 wait、另一方完成後 post)。任何執行緒都可以 post,這點和 mutex 不同。`sem_post` 不會阻塞,可以在中斷處理程式裡呼叫。

### mutex

一次只讓一條執行緒進入某段程式。它和「初值為 1 的 semaphore」的差別是**記得擁有者**:

| 錯誤 | semaphore 的行為 | mutex 的行為 |
|---|---|---|
| 同一條執行緒重複上鎖 | 自己等自己,永遠卡住,沒有任何訊息 | panic:`mutex: recursive lock` |
| 不是上鎖的那條來解鎖 | 默默成功,保護失效 | panic:`mutex: unlock by non-owner` |

所以保護共用資料一律用 mutex,錯誤會當場被指出來。

**關中斷和 mutex 怎麼選**:

| | 關中斷 | mutex |
|---|---|---|
| 適合 | 極短的操作(改幾個指標) | 較長的操作,或中間可能睡眠 |
| 代價 | 期間所有中斷被延遲 | 只擋住要用同一把鎖的執行緒 |
| 中斷處理程式裡 | 可以用 | 不能用(不能睡) |

`kmalloc`、`pmm`、排程器本身用關中斷,因為操作很短,而且中斷路徑也會用到。

### thread_join 與執行緒編號

`thread_join(tid)` 等待指定的執行緒結束。每條執行緒有一個 `joiners` 等待佇列,`thread_exit()` 結束前把上面的人全部喚醒。

連帶的介面修改:**`thread_create` 改為回傳編號(tid)而不是指標**。原因是執行緒結束後 `struct thread` 就被釋放了,呼叫者手上的指標會變成懸空指標(dangling pointer),拿去 join 等於存取已釋放的記憶體。編號不重複使用,所以 `thread_join` 用編號在「所有執行緒」串列裡找:找到就睡在它的佇列上;找不到代表已經結束,立即返回。

`thread_create` 內部也有同樣的問題:把新執行緒放進執行佇列並開中斷之後,它隨時可能跑完並被釋放,所以要回傳的編號必須在開中斷之前先存到區域變數。

### 休眠串列改為排序

`thread_sleep` 插入時依喚醒時間排好,計時器 tick 只看第一個:第一個還沒到期,後面的一定也還沒。每個 tick 的成本從 O(休眠數) 變成 O(1)(到期的數量),代價是插入變成 O(休眠數),但插入遠比 tick 少。

### 示範與驗證

**競爭條件與 mutex**。3 條執行緒各對共用計數器加 100 次,「讀出、加 1、寫回」的中間刻意讓出 CPU:

```
sync: race condition, 3 threads x 100 increments
  without mutex: 100    with mutex: 300    (expected 300)
```

沒有保護時結果是 100:三條都讀到同一個舊值,各自加 1 寫回,三次更新只算一次。加上 mutex 後是正確的 300。實際程式裡這個空檔是由搶佔偶爾造成的,所以錯誤時有時無、很難重現;示範用 `thread_yield()` 讓它每次必然發生。

**生產者 / 消費者**。1 個生產者把 20 個數字經過只有 4 格的緩衝區交給 2 個消費者,用兩個 semaphore(空位數、項目數)和一個 mutex:

```
sync: producer/consumer, 20 items through a 4-slot buffer
  consumed 10 + 10 items, sum=210 (expected 210)
```

緩衝區比資料少,所以生產者會多次因為滿而睡著、被消費者取走後喚醒;消費者則在空的時候睡著。總和 210 = 1+2+…+20,代表沒有遺失也沒有重複。

**join**:

```
sync: join returned after 11 ticks, 4 context switches
```

對方睡 100ms(10 個 tick 加上第 10 步說明過的 1 個 tick 進位),join 在它結束的那一刻返回。整個過程只有 4 次 context switch;用 20ms 輪詢的話,光是 main 的醒來與睡回去就要十次以上。另外也驗證了對已結束的 tid 和不存在的 tid 呼叫 `thread_join` 會立即返回。

**改用 join 之後的副作用**:第 10 步的搶佔示範從 21 次 context switch 降為 13 次,少掉的就是 main 輪詢造成的切換。`stress`(1000 條)改用 semaphore 當完成通知,4003 次切換在 3 個 tick 內完成。

新增開機參數 `crash=relock`(同一條執行緒對同一個 mutex 上鎖兩次),三個架構都 panic,backtrace 為 `mutex_lock` ← `sched_crash_test` ← `kmain`。

回歸矩陣(QEMU 結束碼),三個架構各 11 個情境,共 33 個全數通過;另外每個架構連續執行 10 次 `stress`(含所有示範),30 次全部通過。

| 架構 | 無 | `memtest` | `stress` | `relock` | `stack` | `thread` | `dfree` | `break` | `panic` | `illegal` | `fault` |
|---|---|---|---|---|---|---|---|---|---|---|---|
| i386 | 0 | 0 | 0 | 3 | 3 | 3 | 3 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |

### 已知限制

- 只偵測「自己鎖兩次」。兩條執行緒互相等對方手上的鎖(A 拿 1 等 2、B 拿 2 等 1)這種死結不會被偵測,系統會靜靜卡住。
- 沒有優先權,所以也還沒有優先權反轉的問題與對策(priority inheritance)。
- `thread_join` 拿不到執行緒的返回值;以編號尋找是線性搜尋。
- 沒有條件變數(condition variable)、讀寫鎖、有逾時的等待。
- mutex 解鎖後不是直接交給下一個等待者,而是讓它醒來重新搶;理論上等待者可能連續搶輸(飢餓),目前靠 round-robin 的公平性緩解。
- 仍然只適用於單一 CPU:整套機制靠關中斷達成不可分割,多核心需要 spinlock。
