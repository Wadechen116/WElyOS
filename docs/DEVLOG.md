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
