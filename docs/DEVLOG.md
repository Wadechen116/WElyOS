# WElyOS 開發紀錄

逐步記錄每個步驟「做了什麼、為什麼這樣做、如何驗證」。每新增功能就追加一節。

各步驟的「已知限制」記錄的是**當時**的狀態,保留原文以呈現開發的過程。後來已經解決或有進展的項目,在該項後面以「〔後續:…〕」註明是在哪一步處理的。

## 名詞對照

本文件出現的專業名詞與縮寫。內文中每個縮寫第一次出現時也會在括號內附上英文全名。

### 縮寫

| 縮寫 | 英文全名 | 中文 |
|---|---|---|
| ACPI | Advanced Configuration and Power Interface | 進階組態與電源介面:BIOS 描述硬體的一組表格 |
| AP | Application Processor | 應用處理器:開機後才被叫醒的其他 CPU |
| AHCI | Advanced Host Controller Interface | 進階主機控制器介面:現代 SATA 硬碟的標準介面(本專案未使用) |
| ASID | Address Space Identifier | 位址空間編號 |
| ATA | AT Attachment | PC 傳統的硬碟介面(又稱 IDE,Integrated Drive Electronics) |
| BSS | Block Started by Symbol | 未初始化資料區段 |
| BSP | Bootstrap Processor | 開機處理器:開機時唯一在執行的那顆 CPU |
| CAS | Compare-And-Swap | 比較並交換:「值仍是我看到的那個才改」的原子操作 |
| CFS | Completely Fair Scheduler | 完全公平排程器 |
| COW | Copy-On-Write | 寫入時複製:先共用,有人寫入時才複製 |
| CRTC | Cathode Ray Tube Controller | 映像管控制器:VGA 中管理游標位置、顯示起點等的一組暫存器 |
| BCD | Binary-Coded Decimal | 二進位編碼的十進位:每 4 個位元表示一位十進位數字 |
| CMOS | Complementary Metal-Oxide-Semiconductor | 這裡指 PC 主機板上由電池供電的時鐘與設定記憶體 |
| CSR | Control and Status Register | 控制與狀態暫存器 |
| DMA | Direct Memory Access | 直接記憶體存取:裝置自己把資料搬進記憶體,不經過 CPU |
| fsck | file system check | 檔案系統檢查:找出並修復磁碟上不一致的地方 |
| LBA | Logical Block Addressing | 邏輯區塊定址:磁區從 0 開始依序編號 |
| MMIO | Memory-Mapped I/O | 記憶體對映輸出入:裝置的暫存器出現在記憶體位址上 |
| PCI | Peripheral Component Interconnect | 周邊元件互連:PC 上探索與設定裝置的匯流排 |
| PIO | Programmed I/O | 程式控制輸出入:CPU 自己用指令逐字搬資料 |
| RSS | Resident Set Size | 常駐記憶體大小:行程目前實際對映的記憶體(含與別人共用的) |
| RTC | Real-Time Clock | 即時時鐘:由電池供電、關機時也在走的硬體時鐘 |
| UTC | Coordinated Universal Time | 世界協調時間:不含時區的標準時間 |
| WFS | WElyOS File System | 本專案自訂的磁碟檔案系統 |
| DTB | Device Tree Blob | 裝置樹二進位檔 |
| EEVDF | Earliest Eligible Virtual Deadline First | 最早合格虛擬截止時間優先 |
| EFER | Extended Feature Enable Register | 延伸功能啟用暫存器 |
| ELF | Executable and Linkable Format | 可執行與可連結格式 |
| EOI | End Of Interrupt | 中斷結束通知 |
| FIFO | First In, First Out | 先進先出;在序列埠上指硬體內建的小型收發緩衝區 |
| FDT | Flattened Device Tree | 扁平化裝置樹 |
| GDB | GNU Debugger | GNU 除錯器 |
| GDT | Global Descriptor Table | 全域描述符表 |
| GIC | Generic Interrupt Controller | 通用中斷控制器 |
| HSM | Hart State Management | hart 狀態管理(SBI 用來啟動、停止 CPU 的擴充) |
| ICR | Interrupt Command Register | 中斷命令暫存器:LAPIC 中用來發出 IPI 的暫存器 |
| IDT | Interrupt Descriptor Table | 中斷描述符表 |
| IPI | Inter-Processor Interrupt | 處理器間中斷 |
| IRQ | Interrupt Request | 中斷請求 |
| ISR | Interrupt Service Routine | 中斷服務常式 |
| LAPIC | Local Advanced Programmable Interrupt Controller | 本地進階可程式化中斷控制器 |
| MADT | Multiple APIC Description Table | 多重 APIC 描述表:ACPI 中列出各 CPU 的表格 |
| MLFQ | Multi-Level Feedback Queue | 多層回饋佇列 |
| MMIO | Memory-Mapped I/O | 記憶體對映輸入輸出 |
| MMU | Memory Management Unit | 記憶體管理單元 |
| MPIDR | Multiprocessor Affinity Register | 多處理器親和暫存器:AArch64 每顆 CPU 的硬體編號 |
| MSR | Model-Specific Register | 型號特定暫存器 |
| NX | No-eXecute | 禁止執行位元 |
| OpenSBI | Open Source Supervisor Binary Interface | RISC-V 的開源 SBI 韌體 |
| PAE | Physical Address Extension | 實體位址延伸 |
| PAN | Privileged Access Never | 特權永不存取 |
| PCID | Process-Context Identifier | 行程上下文識別碼 |
| PDPT | Page Directory Pointer Table | 頁目錄指標表 |
| PIC | Programmable Interrupt Controller | 可程式化中斷控制器 |
| PIT | Programmable Interval Timer | 可程式化間隔計時器 |
| PL011 | ARM PrimeCell UART (PL011) | ARM 的序列埠控制器 |
| PMM | Physical Memory Manager | 實體記憶體管理員 |
| PLIC | Platform-Level Interrupt Controller | 平台層級中斷控制器:RISC-V 上彙整週邊裝置中斷的控制器 |
| PPI | Private Peripheral Interrupt | 私有週邊中斷:GIC 中每顆 CPU 各自一份的中斷號(16~31) |
| PSCI | Power State Coordination Interface | 電源狀態協調介面 |
| PTE | Page Table Entry | 頁表項目 |
| PXN | Privileged eXecute Never | 特權不可執行 |
| QEMU | Quick Emulator | 快速模擬器 |
| RSDP | Root System Description Pointer | 根系統描述指標:找到 ACPI 表格的起點 |
| RSDT | Root System Description Table | 根系統描述表:列出其他 ACPI 表格的位址 |
| SPI | Shared Peripheral Interrupt | 共用週邊中斷:GIC 中不屬於特定 CPU 的中斷號(32 以上),要另外指定送給哪顆 CPU |
| SGI | Software Generated Interrupt | 軟體產生的中斷:GIC 中用來當作處理器間中斷的中斷號(0~15) |
| SBI | Supervisor Binary Interface | 管理者二進位介面 |
| SIPI | Startup Inter-Processor Interrupt | 啟動用處理器間中斷:讓 x86 的 AP 開始執行 |
| SMAP | Supervisor Mode Access Prevention | 管理者模式存取保護 |
| SMEP | Supervisor Mode Execution Prevention | 管理者模式執行保護 |
| SMP | Symmetric Multiprocessing | 對稱多處理 |
| SPMC | Single Producer Multiple Consumer | 單一放入者、多個取出者 |
| TLB | Translation Lookaside Buffer | 轉譯後備緩衝區 |
| TR | Task Register | 工作暫存器:x86 中指向目前 TSS 的暫存器 |
| TSan | ThreadSanitizer | 執行緒消毒器:偵測資料競爭的編譯器工具 |
| TSC | Time Stamp Counter | 時間戳計數器 |
| TSS | Task State Segment | 工作狀態區段 |
| TTAS | Test and Test-And-Set | 先測試再做原子交換的 spinlock 寫法 |
| UART | Universal Asynchronous Receiver-Transmitter | 通用非同步收發器 |
| UXN | Unprivileged eXecute Never | 非特權不可執行 |
| VFS | Virtual File System | 虛擬檔案系統:各種檔案系統共用的上層介面 |
| VGA | Video Graphics Array | 視訊圖形陣列 |
| W^X | Write XOR Execute | 可寫與可執行互斥 |

### 其他用語

| 用語 | 英文 | 說明 |
|---|---|---|
| aging | aging | 老化:等越久優先權越高,用來防止飢餓 |
| argv | argument vector | 參數陣列:傳給程式 main 的字串陣列,argv[0] 依慣例是程式名稱 |
| ABA problem | ABA problem | ABA 問題:值由 A 變 B 又變回 A,CAS 誤以為沒變過 |
| atomic | atomic operation | 原子操作:不會被其他 CPU 或中斷從中間插入的操作 |
| backtrace | backtrace | 呼叫堆疊回溯 |
| bitmap | bitmap | 位元圖 |
| break-before-make | break-before-make | ARM 修改頁表的規定:先失效再寫入 |
| callee-saved | callee-saved registers | 被呼叫者保存的暫存器 |
| canonical mode | canonical mode | 規範模式:輸入以「行」為單位交給程式,行內的編輯由 kernel 處理 |
| cache line | cache line | 快取線:CPU 快取存取記憶體的最小單位(通常 64 位元組) |
| coalescing | coalescing | 合併相鄰的空閒區塊 |
| crt0 | C runtime zero | C 程式的啟動程式:在 main 之前執行、準備好參數的一小段程式 |
| context switch | context switch | 上下文切換:換到另一條執行緒執行 |
| cross-call | cross-CPU call | 跨 CPU 呼叫:請其他 CPU 各執行一次指定的函式 |
| circular queue | circular queue / ring buffer | 環狀佇列:固定大小的陣列,用到尾端繞回開頭 |
| data race | data race | 資料競爭:多方同時存取同一記憶體且至少一方寫入,又沒有同步 |
| deadlock | deadlock | 死結:互相等待而永遠無法前進 |
| demand paging | demand paging | 需求分頁:用到某一頁時才配置或載入它 |
| double fault | double fault | 雙重錯誤:處理例外的過程中又發生例外 |
| echo | echo | 回顯:把使用者打的字顯示回螢幕上 |
| exec | execute | 讓目前的行程改為執行另一個程式(行程編號不變) |
| first-fit | first-fit | 最先適合:取第一個夠大的空閒區塊 |
| foreground | foreground process group | 前景群組:目前「擁有」終端機的那一組行程,Ctrl-C 的對象 |
| fork | fork | 複製目前的行程,產生一個幾乎完全相同的新行程 |
| file descriptor | file descriptor (fd) | 檔案描述符:行程用來代表一個開啟中檔案的小整數 |
| frame pointer | frame pointer | 框架指標 |
| freestanding | freestanding | 獨立環境:不依賴作業系統與標準函式庫 |
| futex | Fast Userspace Mutex | 快速使用者空間互斥鎖 |
| guard page | guard page | 保護頁:刻意不對映、用來偵測越界的頁 |
| hart | hardware thread | RISC-V 對一顆 CPU 核心(硬體執行緒)的稱呼 |
| heap | heap | 堆積:動態配置記憶體的區域 |
| block device | block device | 區塊裝置:以固定大小的磁區為單位讀寫的儲存裝置 |
| sector / block | sector / block | 磁區(512 位元組,磁碟的最小單位)/ 區塊(4KB,檔案系統的配置單位) |
| superblock | superblock | 超級區塊:磁碟開頭描述整個檔案系統的那一個區塊 |
| bitmap | bitmap | 位元圖:每個區塊一個位元,記錄是否使用中 |
| indirect block | indirect block | 間接區塊:內容是「其他區塊的編號」的區塊,用來記錄大檔案 |
| buffer cache | buffer cache | 區塊快取:記憶體裡留著最近用過的磁碟區塊 |
| dirty | dirty | 髒:快取裡的內容已修改、還沒寫回磁碟 |
| mount | mount | 掛載:把一個檔案系統接到目錄樹的某個目錄上 |
| journaling | journaling | 日誌:先把要做的修改記下來再執行,當機後可以重做或捨棄(本專案未實作) |
| flush | flush | 要求裝置把它自己暫存的資料真正寫到儲存媒體上 |
| polling | polling | 輪詢:反覆檢查裝置是否完成,不靠中斷通知 |
| virtio / virtqueue | virtio / virtqueue | 為虛擬機器設計的裝置規格 / 它用來交換要求的共用記憶體佇列 |
| disk image | disk image | 磁碟映像檔:主機上的一個普通檔案,內容被模擬器當成一顆磁碟 |
| escape sequence | escape sequence (ANSI escape code) | 控制序列:夾在文字裡、以 ESC 開頭的命令,用來移動游標、清除畫面、換顏色 |
| raw mode | raw mode | 原始模式:按鍵不經行編輯、立刻交給程式(相對於等整行的 canonical mode) |
| CPU utilization | CPU utilization | CPU 使用率:一段時間內 CPU 不是閒置的比例 |
| hard link | hard link | 硬連結:同一個檔案(inode)的另一個名稱 |
| working directory | current working directory (cwd) | 目前的工作目錄:相對路徑的起點,每個行程各有一個 |
| relative path | relative path | 相對路徑:不以 "/" 開頭,從工作目錄算起的路徑 |
| break | program break (brk / sbrk) | heap 的結尾;行程以移動它來取得或歸還記憶體 |
| first fit | first fit | 首次適配:配置時使用第一個夠大的空塊 |
| overcommit | memory overcommit | 過度承諾:答應給的記憶體多於實際有的,真的用到時才配置 |
| demand paging | demand paging | 需求分頁:頁在第一次被存取時才配置 |
| signal handler | signal handler | 訊號處理函式:行程登記的、收到某個訊號時執行的函式 |
| sigreturn | signal return | 處理函式結束時用來還原被打斷時暫存器的系統呼叫 |
| trap frame | trap frame | 從使用者模式進入 kernel 時存下來的整組暫存器 |
| calling convention | calling convention | 呼叫慣例:參數、返回位址放在哪些暫存器或 stack 的約定 |
| fault injection | fault injection | 錯誤注入:測試時故意製造失敗,以執行平常走不到的錯誤處理路徑 |
| credentials | credentials | 身分:行程的使用者編號、群組編號與特權 |
| capability | capability | 特權(能力):把「root 什麼都能做」拆成一項一項可以個別放棄的權力 |
| least privilege | principle of least privilege | 最小權限原則:只保留完成工作所需的權限 |
| permission bits | permission bits (mode) | 權限位元:擁有者、群組、其他人各自的讀、寫、執行 |
| commit accounting | commit accounting | 承諾記帳:記錄已經答應出去的記憶體總量,不答應超過實際有的 |
| quota | resource quota | 配額:單一行程可以使用的資源上限 |
| anonymous mapping | anonymous memory mapping (mmap) | 匿名對映:一塊不對應任何檔案、可以個別歸還的記憶體 |
| identity mapping | identity mapping | 恆等對映:虛擬位址等於實體位址 |
| initramfs | initial RAM file system | 初始記憶體檔案系統:開機時解開到記憶體的一包檔案 |
| inode | index node | 索引節點:代表一個檔案或目錄本身,與名稱無關 |
| large page | large page / huge page | 大分頁:一個項目對映 2MB 或 4MB |
| job | job | 工作:shell 中一條在背景(或前景)執行的管線,有自己的小編號 |
| job control | job control | 工作控制:shell 管理前景與背景工作的功能(`&`、`jobs`、`fg`、`bg`、Ctrl-Z) |
| stop / continue | stop / continue (SIGSTOP, SIGTSTP, SIGCONT) | 暫停與繼續:行程保留所有狀態但不執行,之後從停下的地方繼續 |
| EINTR | Error: Interrupted system call | 系統呼叫被訊號打斷而提早返回的錯誤碼(本專案的暫停不使用這種做法) |
| hardware scrolling | hardware scrolling | 硬體捲動:不搬動畫面內容,只改變顯示卡「從顯示記憶體的哪裡開始畫」 |
| line discipline | line discipline | 行規程:kernel 中把輸入的字元整理成一行一行的那一層 |
| lock-free | lock-free | 無鎖:不靠鎖、以原子操作協調多方存取 |
| orphan | orphan process | 孤兒行程:父行程已經結束的行程 |
| lock ordering | lock ordering | 鎖的順序:多把鎖一律依固定順序取得,以避免死結 |
| linker script | linker script | 連結器腳本:決定各區段的位址 |
| lost wakeup | lost wakeup | 遺失的喚醒:喚醒發生在睡下去之前 |
| multiboot | Multiboot Specification | x86 的開機載入規範 |
| mutex | mutual exclusion lock | 互斥鎖 |
| page fault | page fault | 分頁錯誤:存取的頁沒有對映或權限不符 |
| process group | process group | 行程群組:一組一起接收訊號的行程(例如同一條管線) |
| per-CPU | per-CPU data | 每顆 CPU 各自一份的資料 |
| null device | null device (/dev/null) | 空裝置:讀取永遠得到「結束」、寫入的內容被丟掉的裝置節點 |
| pseudo-terminal | pseudo-terminal (pty) | 虛擬終端機:由程式模擬的終端機,測試時用來代替真人坐在終端機前面 |
| pipe | pipe | 管線:一條單向的資料通道,一端寫入、另一端依序讀出 |
| pipeline | pipeline | 管線串接:多個程式以管線首尾相接,前一個的輸出是後一個的輸入 |
| priority inheritance | priority inheritance | 優先權繼承 |
| priority inversion | priority inversion | 優先權反轉 |
| race condition | race condition | 競爭條件:結果取決於執行順序 |
| reference count | reference count | 參考計數:記錄一個物件有幾個使用者,降到 0 才釋放 |
| redirection | I/O redirection | 重新導向:讓程式的輸出(或輸入)改接到檔案 |
| ramfs | RAM file system | 記憶體檔案系統:內容只存在記憶體裡 |
| real mode | real mode | 真實模式:x86 開機時的 16 位元模式,沒有保護與分頁 |
| round-robin | round-robin | 輪流排程 |
| semaphore | semaphore | 號誌 |
| semihosting | semihosting | 半主機:由模擬器或除錯器代為執行 I/O |
| signal | signal | 訊號:從外面通知一個行程的機制;這裡只用來要求它結束 |
| shell | shell | 命令直譯器:讀取使用者輸入的命令並執行對應的程式 |
| spawn | spawn | 建立一個新行程來執行指定的程式 |
| spinlock | spinlock | 自旋鎖:拿不到鎖時原地反覆嘗試 |
| stack | stack | 堆疊:函式呼叫使用的記憶體 |
| starvation | starvation | 飢餓:執行緒長期得不到 CPU |
| system call | system call (syscall) | 系統呼叫 |
| tar / ustar | tape archive / Unix Standard TAR | 把多個檔案串成一個檔案的格式;ustar 是它的標準化版本 |
| task gate | task gate | 工作閘:觸發硬體工作切換的 IDT 項目 |
| tickless | tickless | 無固定 tick:沒事時不產生計時器中斷 |
| TLB shootdown | TLB shootdown | 通知其他 CPU 讓 TLB 紀錄失效 |
| trap | trap | 例外與中斷的統稱(RISC-V 用語) |
| trap frame | trap frame | 進入例外時存放暫存器的結構 |
| trampoline | trampoline | 跳板:把 CPU 從一種執行環境帶到另一種的一小段程式 |
| type-stable memory | type-stable memory | 型別穩定的記憶體:釋放後只會再被當成同一種結構使用 |
| wait queue | wait queue | 等待佇列 |
| zombie | zombie process | 殭屍行程:已經結束、但結束碼還沒被父行程取走的行程 |
| work stealing | work stealing | 工作竊取:閒置的 CPU 到別顆 CPU 的佇列拿工作 |

---

## 環境

- 開發環境:Windows 11 + WSL2 (Ubuntu),NAT 網路模式
- 編譯器:WSL 內的 gcc(`-m32 -ffreestanding`),不需另外安裝交叉編譯器
- 建置:CMake ≥ 3.20 + make
- 模擬器:Windows 版 QEMU(Quick Emulator,`C:\Program Files\qemu\qemu-system-i386.exe`),由 WSL 呼叫
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

1. QEMU 在 ELF(Executable and Linkable Format) 內找到 multiboot 魔術數 `0x1BADB002`,把 kernel 載入 1MB 並跳到 `_start`。此時 CPU 已在 32 位元保護模式。
2. C 需要 stack,所以 `boot.S` 先設定 `esp` 才呼叫 `kmain`。
3. 沒有 libc,輸出直接對 I/O port 寫入;寫入前輪詢 `COM1+5` 的 bit 5,等傳送緩衝區空。
4. 旗標 `-ffreestanding -nostdlib -fno-pic -fno-stack-protector` 避免編譯器假設有 OS 或 libc。

**驗證**:執行 QEMU,序列埠輸出 `Hello WElyOS`。

---

## 第 2 步:`run` 與 `debug` target

**目標**:一個指令建置並啟動;另有可接 GDB(GNU Debugger) 的除錯模式。

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

**VGA(Video Graphics Array) 文字模式**(`kernel/vga.c`、`include/vga.h`)

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

流程:`boot.S` → `arch_start()`(各架構的 `start.c`,把 multiboot 或 DTB(Device Tree Blob) 轉成 `boot_info`)→ `kmain()`。

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

- **MMIO(Memory-Mapped I/O) vs port I/O**:x86 用 `in/out` 存取獨立的 I/O 位址空間;RISC-V、AArch64 的週邊映射在記憶體位址,用 `volatile` 指標存取。
- **AArch64 `-mstrict-align`**:MMU(Memory Management Unit) 關閉時記憶體為 Device 類型,未對齊存取會觸發例外;`-mgeneral-regs-only` 避免編譯器用到尚未啟用的 FP/SIMD。
- **RISC-V `-mno-relax`**:連結器的 gp 相對最佳化需要先設定 `gp` 暫存器,目前不做。
- **BSS(Block Started by Symbol)**:RISC-V、AArch64 的 `boot.S` 自行清 BSS(linker script 提供 `__bss_start/__bss_end`)。
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
   - RISC-V:OpenSBI(Open Source Supervisor Binary Interface) 佔用 `0x80000000` 起的空間,約定 S-mode kernel 載入於 `0x80200000`。
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
- `handler` 修改 frame 內的 `sepc`,`trap.S` 返回前才寫回 CSR(Control and Status Register),所以可以改變返回位置。

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
- **AArch64 失敗碼**:PSCI(Power State Coordination Interface) `SYSTEM_OFF` 無法帶結束碼,panic 會被誤判為成功(rc=0)。失敗時改用 semihosting `SYS_EXIT`(`hlt #0xf000`,`qemu.sh` 為 aarch64 加 `-semihosting`),成功仍用 PSCI。
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
- **IRQ(Interrupt Request) / FIQ / SError**:非同步;目前尚未啟用,收到即 panic。

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

保護模式下,`cs`/`ds` 等區段暫存器存的是**選擇子**,指向 GDT(Global Descriptor Table,全域描述符表)的一項,描述區段的基底、長度、權限。multiboot 進入時區段可用,但 bootloader 的 GDT 位置不保證保留,而 IDT(Interrupt Descriptor Table) 的每一項都要參照程式碼區段選擇子,所以必須先建立自己的 GDT。

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

**RISC-V:為什麼要透過 SBI(Supervisor Binary Interface)**

計時器(`mtime`/`mtimecmp`)屬於 M-mode,S-mode 的 kernel 不能直接設定。以 `ecall` 請 M-mode 韌體(OpenSBI)代勞,這套介面稱為 SBI:`a7` = 擴充 ID(`0x54494D45`,即 "TIME")、`a6` = 功能 ID、`a0` = 參數。流程是「讀目前時間 → 設定下一次觸發時間 → 中斷時再設下一次」。

**AArch64:為什麼需要 GIC(Generic Interrupt Controller)**

ARM 的週邊中斷都要經過中斷控制器才會送到 CPU:

```
計時器 --(中斷號 30)--> GIC distributor --> GIC CPU interface --> CPU 的 IRQ
```

- Distributor(`0x08000000`):每個中斷號的開關與優先權。
- CPU interface(`0x08010000`):讀 `IAR` 認領中斷(得到中斷號),處理完寫 `EOIR`。沒寫 `EOIR`,GIC 不會再送下一個。
- EL1 實體計時器是中斷號 30(每個 CPU 私有的 PPI)。

**i386:為什麼要重新對映 PIC(Programmable Interrupt Controller)**

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
- GIC 與 UART(Universal Asynchronous Receiver-Transmitter) 的位址寫死為 QEMU virt 的值;實機應從裝置樹讀取。
- AArch64 假設為 GICv2(virt 的預設);GICv3 需另外實作。
- PIT(Programmable Interval Timer) 除數為 16 位元,頻率下限約 19Hz;實際頻率為 1193182/11931 ≈ 100.007Hz,有極小誤差。 **〔後續:後來改為 1000Hz,並以 TSC 校正補記遺失的 tick,見「效能調整:開機耗時與計時準確度」。〕**
- `kprintf` 不可重入:中斷處理程式與主程式若同時輸出會交錯(目前中斷中不輸出)。 **〔後續:已解決:第 10 步起整個 `kprintf` 呼叫期間關中斷;第 16 步改為主控台的 spinlock,多顆 CPU 同時輸出也不會交錯。〕**

---

## 第 9 步:實體記憶體管理與 kernel heap

到上一步為止,kernel 用到的記憶體都是編譯時就決定大小的全域變數與 stack。要建立數量不固定的東西(工作、緩衝區、之後的頁表)就需要動態配置。這一步分兩層做:

| 層 | 介面 | 單位 | 檔案 |
|---|---|---|---|
| 實體記憶體管理員(PMM) | `pmm_alloc()` / `pmm_alloc_pages(n)` / `pmm_free()` | 頁(4KB) | `kernel/pmm.c` |
| kernel heap | `kmalloc()` / `kzalloc()` / `kfree()` | 位元組 | `kernel/kmalloc.c` |

`kmalloc` 不夠用時向 PMM(Physical Memory Manager) 要頁,PMM 則直接管理整塊主記憶體。兩層都與架構無關,三個架構共用同一份程式。

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

選位元圖的理由是需要連續多頁(heap 一次擴充數頁、之後的 kernel stack),而且能抓到重複釋放。掃描慢的問題用一個 `hint`(最低的可能空閒頁)減輕:配置都從 `hint` 開始找,不必每次從頭。(這一步寫的 `hint` 更新邏輯有效能缺陷,第 12 步發現並修正,見該節的踩坑。)

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

- 這一步完成時 PMM 與 kmalloc 都沒有加鎖;第 10 步加入排程器時已補上關中斷保護。 **〔後續:第 16 步再改為各自的 spinlock。〕**
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
- **中斷處理程式的順序**:`sched_irq_exit()` 必須在通知中斷控制器(PIC / GIC 的 EOI(End Of Interrupt))**之後**呼叫,理由見下面的踩坑。

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

- 只支援單一 CPU。「關中斷就安全」的前提在多核心上不成立,那時需要 spinlock。 **〔後續:已解決:第 16 步完成 spinlock 與每顆 CPU 的狀態,第 17 步啟動其他 CPU,第 18 步讓它們參與排程。〕**
- 沒有優先權,所有執行緒平等輪流;`thread_sleep` 的喚醒可能延遲最多一輪時間片。 **〔後續:已解決:第 14 步加入 8 級優先權;較高優先權的執行緒被喚醒時立刻搶佔。〕**
- 這一步完成時沒有同步機制與 `thread_join`,示範裡 main 用輪詢等待;休眠串列也沒有排序。兩者都在第 11 步補上。
- `kprintf` 輸出期間關中斷,輸出很長時會延遲中斷處理。
- stack 溢位偵測是機率性的(見上)。 **〔後續:已解決:第 13 步改用保護頁,溢位在發生當下就被攔下。〕**
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
- **MLFQ(Multi-Level Feedback Queue,多層回饋佇列)**:自動調整,用完整個時間片的降級、提早讓出的升級。
- **CFS(Completely Fair Scheduler) / EEVDF(Earliest Eligible Virtual Deadline First,Linux)**:記錄每條執行緒已使用的 CPU 時間,永遠選用得最少的那條,用紅黑樹維持順序。

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
- 沒有優先權,所以也還沒有優先權反轉的問題與對策(priority inheritance)。 **〔後續:已解決:第 14 步加入優先權與優先權繼承,第 15.5 步補成完整版。〕**
- `thread_join` 拿不到執行緒的返回值;以編號尋找是線性搜尋。
- 沒有條件變數(condition variable)、讀寫鎖、有逾時的等待。
- mutex 解鎖後不是直接交給下一個等待者,而是讓它醒來重新搶;理論上等待者可能連續搶輸(飢餓),目前靠 round-robin 的公平性緩解。
- 仍然只適用於單一 CPU:整套機制靠關中斷達成不可分割,多核心需要 spinlock。 **〔後續:已解決:第 16 步改由排程器的 spinlock 保護。〕**

---

## 第 12 步:分頁(MMU)

到目前為止 kernel 直接使用實體位址,任何指標都能讀寫任何記憶體。這一步開啟分頁:每次記憶體存取都先經過頁表轉換,並檢查權限。

從這一步開始,設計上明確以**快速**與**安全**為準則,這一節的每個選擇都會說明這兩方面的考量。

| 檔案 | 內容 |
|---|---|
| `include/vm.h`、`kernel/vm.c` | 頁表的走訪、對映、拆分,與架構無關 |
| `arch/<名稱>/mmu.h`、`mmu.c` | 各架構的頁表項目格式、開啟方式、TLB 失效、裝置對映 |
| `arch/<名稱>/linker.ld` | 各區段對齊到頁,新增 `__text_end`、`__rodata_end` |

### 分頁給了什麼

- **保護**:沒有對映的位址一碰就觸發例外;每一頁可以各自設定可寫、可執行。
- **重新安排**:任何實體頁可以出現在任何虛擬位址。
- **隔離**(之後):每個行程一份頁表,互相看不到對方的記憶體。這是使用者模式的前提。

### 頁表是一棵多層的樹

把虛擬位址切成幾段,每段當作一層表格的索引,一路查到最後一層得到實體頁的位址與權限。每張表格剛好一頁。

| | i386 | RISC-V(Sv39) | AArch64 |
|---|---|---|---|
| 虛擬位址寬度 | 32 位元 | 39 位元 | 39 位元(`T0SZ=25`) |
| 層數 | 2 | 3 | 3 |
| 每層索引 | 10 位元(1024 項) | 9 位元(512 項) | 9 位元(512 項) |
| 項目大小 | 32 位元 | 64 位元 | 64 位元 |
| 根表格放在 | `CR3` | `satp` | `TTBR0_EL1` |
| 開啟方式 | `CR0.PG` | `satp` 的模式欄位 = 8 | `SCTLR_EL1.M` |
| 出錯位址 | `CR2` | `stval` | `FAR_EL1` |
| 讓 TLB 一筆失效 | `invlpg` | `sfence.vma` | `tlbi vaae1` |
| 大分頁 | 4MB(需 `CR4.PSE`) | 2MB | 2MB(block) |
| 禁止執行 | 不支援(需 PAE) | X 位元 | PXN 位元 |

用多層而不是一張大表的原因:沒用到的位址範圍不需要配置下層表格。39 位元的位址空間若用單層,光是表格就要 1GB。

三個架構的差異只有上表這些。走訪樹的邏輯完全相同,所以 `kernel/vm.c` 用 `MMU_LEVELS`、`MMU_BITS` 兩個常數寫成一份,各架構只提供「怎麼編碼一個項目」的小函式。

AArch64 多一個其他兩者沒有的概念:**記憶體屬性**。它要求明確標示一段位址是一般記憶體(可快取、存取可重排)還是裝置暫存器(每次讀寫都有副作用,必須照順序)。屬性定義在 `MAIR_EL1`,每個項目用編號選用。

### 恆等對映

目前採恆等對映:虛擬位址等於實體位址。好處是開啟分頁的前後,所有既有的指標、stack、返回位址都繼續有效,開啟的那一刻不需要任何跳躍或位址調整;頁表本身的實體位址也能直接當指標用。

這不是最終的安排。有了使用者程式之後,通常把 kernel 移到位址空間的高半部,低半部留給使用者,到時候會再調整。

### 安全方面的設計

**1. W^X(Write XOR Execute):可寫的不可執行,可執行的不可寫**

| 區段 | 權限 | 擋下什麼 |
|---|---|---|
| `.text`(程式碼) | 唯讀、可執行 | 程式碼被錯誤的指標或攻擊者改寫 |
| `.rodata`(常數) | 唯讀 | 常數被改寫 |
| 其餘(資料、heap、stack、空閒頁) | 可讀寫、不可執行 | 把資料當成程式碼執行(多數攻擊的最後一步) |
| 裝置樹、開機參數 | 唯讀 | bootloader 的資料被誤寫 |

`vm_map()` 本身也會拒絕同時帶有可寫與可執行的要求,所以之後的程式不小心也建不出這種頁。

linker script 為此把各區段對齊到頁:權限以頁為單位,程式碼和資料若共用一頁就無法分別設定。

**2. NULL 指標偵測**

位址 0 所在的頁刻意不對映。解參考 NULL 指標在沒有分頁時會靜靜讀到垃圾或寫壞東西(i386 上位址 0 是真實的記憶體),現在會立刻觸發 page fault 並指出位置。

**3. i386 要另外開 `CR0.WP`**

x86 預設 ring 0 可以寫入唯讀頁,只有使用者模式受限制。不開 WP 的話,把 kernel 程式碼設成唯讀毫無效果。這是容易漏掉的一點。

**4. i386 沒有「禁止執行」**

傳統的 32 位元頁表格式沒有 NX(No-eXecute) 位元,要改用 PAE(Physical Address Extension,64 位元的項目、三層)才有。所以 i386 上資料頁仍然可以執行,`vm_query` 也如實回報 `rwx`。這是目前三個架構中安全性最弱的一點,列在已知限制。

### 效能方面的設計:大分頁

第一版把 128MB 全部用 4KB 頁對映,能運作,但有兩個代價:CPU 的 TLB(Translation Lookaside Buffer,轉換結果的快取)只有幾十到幾百筆,每筆涵蓋 4KB 的話很快就不夠用,每次 miss 都要重新走訪頁表;而且最後一層表格本身要佔不少記憶體。

改為能用大分頁就用:位址對齊、整塊在記憶體範圍內、而且整塊權限相同時,在上一層直接放一個涵蓋 2MB(i386 為 4MB)的項目。

| 架構 | 全部 4KB 頁 | 使用大分頁 |
|---|---|---|
| i386 | 33 頁表格 | 3 頁表格,30 個 4MB 大分頁 |
| riscv64 | 68 頁表格 | 7 頁表格,61 個 2MB 大分頁 |
| aarch64 | 69 頁表格 | 7 頁表格,62 個 2MB 大分頁 |

好處有三個:一筆 TLB 紀錄涵蓋的範圍是原來的 512 或 1024 倍;TLB miss 時少走一層;表格用量約為十分之一。kernel 映像所在的那一塊仍用 4KB 頁,因為裡面的程式碼、常數、資料權限不同。

**自動拆分**:之後若要單獨修改大分頁裡的某一頁(改權限或移除),`vm_map` / `vm_unmap` 會先把那一塊拆成一整張 4KB 的表格,內容與權限不變,再修改目標那一頁。裝置樹設成唯讀時就用到了這個機制。

這個效益沒有在 QEMU 上量測:QEMU 是軟體模擬,它的 TLB 行為和真實 CPU 不同,量出來的數字不能代表實機。上面的表格用量是實測值;TLB 的效益是依據硬體的運作方式推論的。

### TLB 失效

CPU 把查過的轉換結果快取在 TLB。修改頁表項目之後,必須讓對應的那一筆失效,否則 CPU 可能繼續用舊的對映,等於修改沒有生效。這種錯誤很隱蔽,因為要等到 TLB 裡剛好還留著舊紀錄才會出現。`vm_map`、`vm_unmap`、拆分都會呼叫 `arch_tlb_flush()`。

### 踩坑

**1. 頁面配置器的 O(n²):用時間量測才發現的效能缺陷**

開啟分頁後回歸測試全部通過,但 `memtest`(把所有空閒頁配置一遍)的耗時從約 0.1 秒變成:

| 架構 | 修正前 | 修正後 |
|---|---|---|
| i386 | 13.9 秒 | 1.29 秒(含開機與所有示範) |
| riscv64 | 22.8 秒 | 1.32 秒 |
| aarch64 | 13.9 秒 | 1.29 秒 |

直覺會懷疑是分頁造成的,但加上時間標記後發現時間全花在「配置」迴圈,而不是記憶體存取。原因在第 9 步寫的 `hint`(掃描起點)更新邏輯:

```c
if (first == hint)       /* 只有配置到的剛好是 hint 那一頁才更新 */
    hint = i + 1;
```

`hint` 更新後可能落在一個**已使用**的頁上。下一次配置找到的頁在它之後,`first != hint`,於是 `hint` 不更新;再下一次也一樣。`hint` 從此停住,每次配置都要重新掃過前面越來越長的已使用區段,配置 n 頁變成 O(n²)。

之前沒發現,是因為在分頁之前記憶體是從頭連續配置的,`hint` 後面永遠是空閒頁。開了分頁,頁表佔掉的頁打破了這個巧合。

修正:掃描時記下遇到的第一個空閒頁,配置完用它來決定 `hint`,保證 `hint` 一定會跨過已使用的區段。另外加了「整個位元組都是 1 就一次跳過 8 頁」的加速。

值得記住的是:**功能測試全部通過不代表沒有問題**,這個缺陷只有看時間才看得出來。也因此回歸測試現在會留意耗時。

**2. 自我測試的假設太強**

第 9 步的測試假設「連續兩次配置會得到相鄰的兩頁」。頁表配置與釋放之後記憶體中出現了空洞,這個假設不再成立,測試誤報。真正的保證只有「由低位址往高位址配置、不重疊」,測試改成檢查這個。

**3. 大分頁的拆分示範沒有測到拆分**

第一版的示範選了一個剛配置的頁來改權限,但它剛好落在 kernel 映像附近、本來就是 4KB 對映的區域,輸出顯示「0 new table page」,代表拆分的程式根本沒有執行到。改成選用主記憶體 1/4 處、確定位於大分頁內的位址,並用 `KASSERT` 要求剛好多用 1 頁表格。教訓和上一個類似:測試要確認它真的走到了想測的那條路徑。

### 驗證

```
vm: paging enabled (RISC-V Sv39), 126 MB mapped, 61 large pages of 2 MB, 7 page-table pages
vm: text 0x0000000080200000-0x0000000080207000 r-x, rodata -0x0000000080209000 r--, rest rw-
vm: kmain r-x, rodata r--, data rw-
vm: 0x00000000c0000000 and 0x00000000c0001000 both map to physical 0x0000000080216000 (2 new table pages)
vm: split a 2 MB page to write-protect 0x0000000082001000 (1 new table page)
```

`vm_demo()` 每次開機都會跑:

- **權限**:查詢 `kmain`、一個常數、一個變數所在的頁,確認分別是 `r-x`、`r--`、`rw-`;位址 0 沒有對映。
- **別名**:把同一個實體頁對映到 `0xC0000000` 和 `0xC0001000` 兩個原本不存在的位址。從其中一個寫入,另一個和實體位址都讀到相同的值。
- **W^X**:要求可寫又可執行的對映被拒絕。
- **拆分**:把大分頁中的一頁改成唯讀,確認剛好多用 1 頁表格、該頁變成唯讀、相鄰的頁仍可寫入、內容不變。

開啟分頁之後,既有的所有功能(記憶體配置、執行緒、同步、計時器)都照常運作;`memtest` 把每個空閒頁都寫入並讀回,確認整塊主記憶體都正確對映。

三個新的開機參數,驗證保護確實生效:

| 參數 | 動作 | i386 | riscv64 | aarch64 |
|---|---|---|---|---|
| `crash=null` | 讀取位址 0 | Page fault,`cr2=0` | Load page fault,`stval=0` | Data abort,`far=0` |
| `crash=wtext` | 寫入 `kmain` 的程式碼 | Page fault,`err=0x3` | Store page fault | Data abort |
| `crash=exec` | 跳到資料頁執行 | 略過(無 NX) | Instruction page fault | Instruction abort |

i386 的 `err=0x3` 表示「頁存在」加「寫入」,也就是權限違規而不是沒有對映,和預期一致。三者回報的出錯位址都正是被存取的位址。

回歸矩陣(QEMU 結束碼),三個架構各 14 個情境,共 42 個,結果全部符合預期;另外三個架構各連續執行 5 次 `stress memtest`,15 次全部通過。

| 架構 | 無 | `memtest` | `stress` | `null` | `wtext` | `exec` | `relock` | `stack` | `thread` | `dfree` | `break` | `panic` | `illegal` | `fault` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| i386 | 0 | 0 | 0 | 3 | 3 | 0(略過) | 3 | 3 | 3 | 3 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |

### 為什麼這一步還沒有做 stack 保護頁

第 10 步提到可靠的 stack 溢位偵測要靠保護頁:在 stack 下方留一頁不對映,一碰到就觸發例外。有了分頁,移除對映只要一行,但還缺一個條件:**例外處理程式需要另一條 stack**。

目前例外發生時,入口把暫存器存在「當下的 stack」上。stack 溢位時當下的 stack pointer 正指著保護頁,入口一寫入就再次出錯,形成例外中的例外,i386 上會直接重置。要解決得讓例外切換到獨立的 stack:i386 用 TSS(Task State Segment),RISC-V 用 `sscratch`,AArch64 用 `SP_EL0` / `SP_EL1` 的分工。這些機制正好也是使用者模式需要的,所以留到下一步一起做。在那之前,stack 溢位仍由第 10 步的記號偵測。

### 已知限制

- **i386 的資料頁可以執行**(沒有 NX)。要解決需改用 PAE 分頁。 **〔後續:已解決:第 15.5 步改用 PAE 分頁並啟用 NX。〕**
- 只有一份 kernel 頁表,採恆等對映;還沒有使用者位址空間,kernel 也還沒有移到高半部。 **〔後續:部分解決:第 15 步加入每個行程獨立的位址空間;kernel 仍是恆等對映、未移到高半部。〕**
- 拆分大分頁時直接以新表格取代原項目。ARM 架構正式的要求是先讓項目失效、清 TLB、再寫入新項目(break-before-make),否則實機上可能出現 TLB 衝突;但被拆的那一塊可能正包含執行中的 stack 或頁表本身,中間那一刻無法失效。QEMU 上沒有問題,實機需要另外處理。
- AArch64 開啟快取前沒有先清除快取內容。QEMU 不模擬快取所以沒有影響,實機上必須做。
- 頁表用掉的頁不會釋放(移除對映後,空的表格仍留著);拆開的大分頁也不會合併回去。
- 裝置的位址寫死為 QEMU `virt` / PC 的值,應從裝置樹讀取。
- TLB 失效只處理目前這顆 CPU;多核心需要通知其他 CPU(TLB shootdown)。 **〔後續:已解決:第 18 步加入 TLB shootdown。〕**
- page fault 目前一律視為致命錯誤。之後的需求分頁(demand paging)、寫入時複製(copy-on-write)都是靠「處理 page fault 後繼續執行」達成的。 **〔後續:部分解決:第 22 步起,使用者模式的 page fault 若是寫入時複製或 stack 成長造成的,會處理後繼續執行。〕**

---

## 效能調整:開機耗時與計時準確度

第 12 步修正配置器之後,一次 `memtest` 執行仍要 1.3 秒。這一節記錄把它降到 0.4 秒的過程,以及途中發現的計時準確度問題。

### 先量測,再決定改哪裡

直覺會想去最佳化 `memtest` 本身。先在主機端替每一行輸出加上時間戳(RISC-V,共 1.35 秒):

| 階段 | 耗時 |
|---|---|
| QEMU 啟動到 kernel 第一行輸出 | 0.19 秒 |
| 開啟分頁 | 0.01 秒 |
| `memtest`(32225 頁寫入並驗證) | 0.06 秒 |
| 搶佔示範 | 0.55 秒 |
| 休眠示範 | 0.35 秒 |
| 同步示範(含 join) | 0.12 秒 |
| `stress`(1000 條執行緒) | 0.03 秒 |

`memtest` 只佔 0.06 秒。超過 1 秒是示範在**等時間**:計時器只有 100Hz,時間片 50ms,要觀察到幾輪搶佔就得空轉 0.5 秒。再怎麼最佳化 `memtest` 也省不到 5%。

### 把 tick 提高到 1000Hz

| | 之前 | 現在 |
|---|---|---|
| tick 頻率 | 100Hz(10ms) | 1000Hz(1ms) |
| 時間片 | 5 tick = 50ms | 10ms(以毫秒定義,依頻率換算成 tick) |
| `thread_sleep` 精度 | 10ms | 1ms |
| 搶佔示範 | 空轉 500ms | 空轉 60ms(同樣是 6 個時間片左右) |
| 休眠示範 | 100 / 200 / 300ms | 10 / 20 / 30ms |

這不只是讓示範變短。時間片 10ms 表示一條執行緒最多等其他人 10ms 就能再輪到,互動反應比 50ms 好;`thread_sleep(1)` 也真的只睡約 1ms。代價是每秒多處理 900 次計時器中斷,每次中斷的工作量很小(計數、看休眠串列的第一個、扣時間片),相對於 1ms 可以忽略。Linux 的 `CONFIG_HZ` 常見值是 250 或 1000,取捨相同。

時間片改為以毫秒定義(`TIME_SLICE_MS`),在 `sched_init()` 依 `timer_hz()` 換算,之後再調頻率就不必改排程器。因此 `timer_init()` 要在 `sched_init()` 之前呼叫。

### 踩坑:提高頻率後 tick 開始遺失

改成 1000Hz 後,設定空轉 60ms 的示範實際花了 90~125ms。用「睡 1000ms、在主機端量實際經過的時間」驗證:

| 架構 | 修正前 | 修正後 |
|---|---|---|
| i386 | 1.488 秒 | 1.002~1.004 秒 |
| riscv64 | 未直接量測(見下) | 1.001 秒 |
| aarch64 | 未直接量測(見下) | 1.002~1.003 秒 |

i386 的系統時鐘慢了約 49%。RISC-V 與 AArch64 修正前沒有做同樣的一秒量測,但當時的開機紀錄顯示 kernel 計了 133 個 tick 的期間,主機端實際經過約 279ms,也就是時鐘大約只有一半的速度。原因有兩個,都和「中斷送達的時間比預定晚」有關。在模擬器上,計時器是由主機的作業系統排程的,晚個 1ms 很常見;100Hz 時這相對於 10ms 的間隔不明顯,1000Hz 時就和間隔一樣大了。實機上只要中斷被關閉得夠久,同樣會發生。

**原因一(RISC-V、AArch64):以「現在」為基準設定下一次**

```c
sbi_set_timer(read_time() + interval);    /* 修正前 */
```

每次中斷都晚到一點,下一次又從晚到的時間點起算,延遲就一直累加。修正為以**上一次的預定時間**為基準:

```c
while (deadline <= now) {
    timer_tick();
    deadline += interval;     /* 預定時間固定以 interval 遞增,與實際送達時間無關 */
}
sbi_set_timer(deadline);
```

這樣延遲不會累積。如果晚了不只一個間隔,迴圈會把錯過的 tick 一次補記回來。AArch64 為此從 `CNTP_TVAL`(相對的倒數值)改用 `CNTP_CVAL`(絕對的比較值)。

**原因二(i386):PIT 不知道漏了幾次**

PIT 是週期性計時器,自己不會漂移,但它只會發出中斷。兩次中斷若合併成一次,kernel 就少算一個 tick,而且沒有任何方法知道。

解法是引入獨立的時間來源:**TSC(Time Stamp Counter)**,CPU 內建、隨時間遞增的 64 位元計數器。PIT 中斷只負責叫醒,實際該記幾個 tick 由 TSC 經過的時間決定,寫法和上面的迴圈相同。

TSC 每秒跳幾次因 CPU 而異,開機時要校正:讓 PIT 的 channel 2 倒數 10ms,輪詢到它結束,看這段期間 TSC 增加多少。CPU 不支援 TSC 時(用 CPUID 檢查)退回每次中斷記一個 tick。

補記有上限(一次最多 100 個)。落後超過這個量時放棄補記、重新對時,避免被除錯器暫停很久之後,恢復時一口氣補上幾萬個 tick。

### 結果

| | 修正配置器前 | 修正配置器後 | 這一節之後 |
|---|---|---|---|
| i386 `memtest` 整次執行 | 13.9 秒 | 1.29 秒 | 0.50 秒 |
| riscv64 | 22.8 秒 | 1.32 秒 | 0.41 秒 |
| aarch64 | 13.9 秒 | 1.29 秒 | 0.40 秒 |

一般開機(含所有示範)為 0.31~0.45 秒,其中約 0.19 秒是 QEMU 本身的啟動時間(從 WSL 啟動 Windows 程式),kernel 無法影響;i386 另外多 10ms 的 TSC 校正。

42 個回歸情境結果不變;三個架構各連續執行 10 次 `stress memtest`,30 次全部通過。

### 這次學到的

- **先量測再最佳化**。使用者感受到的是「memtest 很慢」,但瓶頸完全不在 memtest。
- **計時不能以「中斷來了幾次」為準**,要以「時間過了多少」為準。前者在中斷延遲時會悄悄出錯,而且功能測試看不出來,只有拿外部時鐘對照才會發現。
- 兩個問題都是提高頻率之後才浮現的,但缺陷在 100Hz 時就已經存在,只是誤差小到沒被注意。

### 已知限制

- 搶佔示範兩條執行緒的執行量差距由 1%~3% 變為約 10%:tick 補記時會一次扣掉多個 tick 的時間片,時間片的實際長度因此有些抖動。這是模擬器上中斷延遲造成的,仍遠在公平性檢查(3 倍以內)的範圍內。
- TSC 校正假設它的頻率固定(現代 CPU 的 invariant TSC)。舊 CPU 會隨省電狀態變動頻率,需要改用 HPET 或 ACPI PM timer。
- 仍然是固定頻率的 tick。所有執行緒都在睡時也照樣每秒中斷 1000 次;tickless 做法見「排程器還能怎麼更有效率」一節。

---

## 第 13 步:stack 保護頁與獨立的例外 stack

第 10 步的 stack 溢位偵測是在 stack 底端放一個記號,切換執行緒時檢查。它有兩個弱點:溢位到被發現之間已經寫壞了相鄰的記憶體;而且寫入若剛好跨過記號就偵測不到。這一步改用保護頁,讓溢位在**發生的那一刻**就被攔下,記號機制移除。

### 保護頁

每條 stack 多配置一頁放在最低位址,並把這一頁從頁表移除:

```
低位址                                              高位址
| 保護頁(不對映) |        可用的 stack(16KB)        |
```

stack 往低位址成長,用過頭的第一個存取就會落在保護頁上,觸發 page fault。相鄰的記憶體完全不會被寫到。main 執行緒使用的開機 stack 也一樣:`boot.S` 在它下方預留一頁,排程器啟動時移除對映。

保護頁位於以大分頁對映的記憶體裡時,第 12 步的自動拆分會處理。

### 難處:例外處理程式自己也需要 stack

移除對映只要一行,真正的問題在後面。例外發生時,入口要把暫存器存到 stack 上;但 stack 溢位時 stack pointer 正指著保護頁,一存就再次出錯,又進入口,無窮迴圈。所以**例外入口必須在碰 stack 之前,先確定 stack 還能用**,不能用就換一條。

三個架構的做法不同,原因在於硬體在進入例外時做了多少事:

| | RISC-V | AArch64 | i386 |
|---|---|---|---|
| 硬體進入例外時會用到 stack 嗎 | 不會 | 不會 | **會**:CPU 自動推入 `eflags`、`cs`、`eip` |
| 做法 | 入口第一件事檢查 `sp` | 入口第一件事檢查 `sp` | TSS 加 double fault 工作閘 |
| 檢查時暫存器不夠用,暫放在 | `sscratch` | `TPIDR_EL1`、`TPIDRRO_EL0` | 不適用 |
| 例外 stack | `exc_stack`(8KB) | `exc_stack`(8KB) | `df_stack`(8KB) |

**RISC-V 與 AArch64**:入口比較 `sp` 與 `kstack_limit`(目前執行緒 stack 的下限,排程器每次切換時更新)。放不下一個 trap frame 就把 `sp` 換成專用的例外 stack,原本的 `sp` 記在 frame 裡供回報。

這裡有個細節:做比較需要暫存器,但進入例外的瞬間所有暫存器都還是被打斷程式的值,一個都不能動。兩個架構都提供了專門給 kernel 暫放東西的系統暫存器,先把要用的一兩個暫存器存進去,比完再取回。

**i386**:CPU 在執行我們的任何一條指令**之前**,就已經往目前的 stack 推入三個值。stack 溢位時這個推入本身就失敗,升級成 double fault(#DF);#DF 同樣要推入,再失敗就是 triple fault,CPU 直接重置,什麼訊息都沒有。入口檢查完全來不及。

唯一的辦法是讓硬體在推入之前就換好 stack,這要靠 **TSS**(task state segment)與**工作閘**(task gate):

- TSS 是一個存放整組暫存器的結構。
- 把 #DF 的 IDT 項目設成工作閘後,觸發時 CPU 不推入 stack,而是做一次硬體工作切換:把當下的暫存器存進目前的 TSS(`main_tss`),再從另一個 TSS(`df_tss`)載入整組暫存器,包含全新的 `esp` 與 `eip`。
- 處理程式在乾淨的 stack 上執行,並可以從 `main_tss` 讀到出錯當下的 `eip`、`esp`、`ebp`。

一個容易漏掉的地方:工作切換也會載入 TSS 裡的 `CR3`。如果沒有填入有效的頁表位址,切換的瞬間就 triple fault。所以分頁開啟時要把頁表位址寫進 TSS。

TSS 之後還有另一個用途:使用者模式進入 kernel 時,CPU 從 TSS 的 `esp0` 取得 kernel stack 的位置。

### 回報

三個架構的致命例外路徑都會呼叫 `sched_report_overflow()`,依據出錯當下的 stack pointer 與出錯位址判斷是否為溢位:

```
*** KERNEL STACK OVERFLOW in thread 1 (overflow) ***
  stack 0x00123000-0x00127000, guard page 0x00122000, sp=0x00122e30

*** EXCEPTION: Double fault (#DF) ***
  eip   =0x001026e3   (發生例外的指令位址)
  esp   =0x00122e30  ebp=0x00123048
  cr2   =0x00122e2c   (出錯的記憶體位址)
backtrace:
  #0 pc=0x001026e3
  #1 ra=0x00102718
  #2 ra=0x00102718
```

出錯位址 `0x00122e2c` 落在保護頁 `0x00122000`~`0x00123000` 內,backtrace 是同一個遞迴函式重複出現,符合預期。RISC-V 回報為 Store page fault,AArch64 為 Data abort。

### 效能:stack 快取

保護頁讓建立與結束執行緒變貴了:建立時要移除保護頁的對映並清 TLB,結束時要對映回來再清一次(否則下一個拿到這頁的人無法使用)。

所以結束的執行緒不把 stack 還給 PMM,而是放進快取(最多 32 條),下次建立直接取用。保護頁維持未對映的狀態,頁面配置、頁表修改、TLB 清除全部省下。記憶體不足或測試需要核對用量時,`sched_trim()` 會把快取清空。

實測:循序建立並等待 1000 條執行緒結束(同時只有一條存活),單位為 tick(1ms),各跑 3 次:

| 架構 | 無快取 | 有快取 |
|---|---|---|
| i386 | 6~8 | 2~3 |
| riscv64 | 26~28 | 4 |
| aarch64 | 8 | 4~5 |

快取使這個情境快了約 2 到 7 倍。這是在 QEMU 上量的,RISC-V 的差距特別大應與 QEMU 處理 TLB 清除的方式有關,實機上的倍數會不同;但省掉的工作(頁表修改與 TLB 清除)在實機上同樣是相對昂貴的操作。

快取幫不上忙的情況:1000 條執行緒**同時**存在時,每條都需要自己的 stack。`stress` 的第一階段就是這種情況,耗時 27~61 個 tick,比沒有保護頁時慢,這是安全性的代價。

### 驗證

新增開機參數 `crash=mainstack`(讓 main 執行緒使用的開機 stack 溢位);原有的 `crash=stack` 是執行緒的 stack 溢位。兩者在三個架構都回報 `KERNEL STACK OVERFLOW`,並指出正確的執行緒、stack 範圍與保護頁位置。

回歸矩陣(QEMU 結束碼),三個架構各 15 個情境,共 45 個,結果全部符合預期;另外三個架構各連續執行 10 次 `stress memtest`,30 次全部通過。

| 架構 | 無 | `memtest` | `stress` | `stack` | `mainstack` | `null` | `wtext` | `exec` | `relock` | `thread` | `dfree` | `break` | `panic` | `illegal` | `fault` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| i386 | 0 | 0 | 0 | 3 | 3 | 3 | 3 | 0(略過) | 3 | 3 | 3 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |

一般開機(含所有示範)耗時 0.31~0.45 秒,與上一步相同。

### 踩坑:測試的基準沒有算到快取

加入快取後,`stress` 結束時的記憶體核對不符,差了剛好 15 頁。印出數字後發現不是洩漏:測試開始時,快取裡還留著前面示範用過的 3 條 stack(各 5 頁),它們不算空閒;測試結束時清空快取,這 15 頁回到 PMM,結果比基準「多」了 15 頁。修正是量基準之前先 `sched_trim()`。

另外,保護頁會把所在的大分頁拆開,多出來的頁表不會歸還,所以核對時改為比較「空閒頁數加頁表頁數」。

### 已知限制

- 只有一條共用的例外 stack,而且只用在必定以 panic 收場的溢位情況。將來若要從 page fault 恢復執行(需求分頁),或支援多核心,每顆 CPU 需要自己的例外 stack。
- 一次跳過整個保護頁的存取偵測不到,例如宣告一個超過 4KB 的區域陣列卻只寫它的最低位址。編譯器的 stack probing(`-fstack-clash-protection`)可以防這種情況,目前沒有開啟。
- i386 的 #DF 一律走工作閘,不是 stack 溢位造成的 double fault 也會進到這裡;處理程式會註明原因不是溢位。
- 例外 stack 本身沒有保護頁。
- 借用 `TPIDR_EL1`、`TPIDRRO_EL0` 與 `sscratch` 當暫存空間,之後使用者模式需要它們時要重新安排。 **〔後續:已解決:第 15 步為使用者模式調整了 `sscratch` 的用法;第 16 步再改為 `TPIDR_EL1` / `tp` 固定指向每顆 CPU 的結構,暫存改放在該結構內。〕**

---

## 第 14 步:優先權排程與優先權繼承

到上一步為止所有執行緒平等輪流。這在工作性質都相同時沒問題,但實際的系統裡,等鍵盤輸入的工作和做大量計算的工作不該被同等對待:前者大部分時間在睡,一旦醒來應該馬上處理,否則系統反應遲鈍。這一步加入優先權。

### 規則

優先權 0(最低)到 7(最高),預設為 4(`PRIO_NORMAL`):

- 永遠執行可執行的執行緒中優先權最高的那一條。
- 相同優先權之間輪流,每次一個時間片(10ms)。
- 較高優先權的執行緒一變成可執行(被喚醒或被建立),**立刻**搶佔目前的執行緒,不等時間片用完。

這稱為固定優先權搶佔式排程,即時作業系統多半採用,因為行為完全可預測。

### 資料結構:每級一條佇列,加一個位元圖

```
rq_bitmap = 0b01010100        (第 2、4、6 級有人在等)
rq_head[6] -> H
rq_head[4] -> N1 -> N2
rq_head[2] -> L
```

選下一條執行緒:找位元圖中最高的 1,取那條佇列的頭。成本只和級數有關(固定 8 級),和執行緒總數無關。這是上次討論效率時提到的做法。

找最高位元沒有用 `__builtin_clz`:RISC-V 的基本指令集沒有對應的指令,編譯器會改呼叫 libgcc 的函式,而我們沒有連結 libgcc(和 i386 的 64 位元除法是同一類問題)。8 級用迴圈檢查就夠了。

### 三個需要仔細處理的地方

**1. 被搶佔的人不該吃虧**

執行緒 A 的時間片用到一半,被更優先的 B 搶佔。B 做完之後,A 應該繼續用完剩下的時間片,而不是排到同級的最後面重新等一輪。所以 `schedule()` 依剩餘時間片決定放回佇列的位置:

| 情況 | 剩餘時間片 | 放回位置 |
|---|---|---|
| 被更優先的搶佔 | 大於 0 | 同級佇列的**最前面**,保留剩餘時間片 |
| 時間片用完 | 0 | 同級佇列的最後面 |
| 主動 `thread_yield()` | 設為 0 | 同級佇列的最後面 |

**2. 喚醒別人時的搶佔點**

`sem_post()` 喚醒了一條更優先的執行緒,它應該立刻執行。但喚醒有兩種場合:

- 在一般執行緒中:可以馬上切換。
- 在中斷處理程式中:不能在這裡切換,要等中斷處理完、通知過中斷控制器之後(第 10 步的 EOI 問題)。

所以喚醒只「記下需要重新排程」,實際切換由 `sched_preempt_point()` 執行;它在中斷中呼叫時不做事,留給中斷返回前的 `sched_irq_exit()`。為了分辨是否在中斷中,各架構的中斷處理程式開頭多呼叫一個 `sched_irq_enter()`。

**3. 等待佇列喚醒優先權最高的**

semaphore 和 mutex 喚醒等待者時,原本是喚醒等最久的,現在改為喚醒優先權最高的(相同時才比先後)。否則低優先權的等待者會先拿到資源,高優先權的反而繼續等。

### 優先權反轉與優先權繼承

有了優先權,會出現一個不直覺的問題。三條執行緒:

- **low** 拿到一把鎖,還要工作一小段時間才會解鎖
- **high** 要拿同一把鎖,只好等 low
- **medium** 和這把鎖毫無關係,只是需要 CPU

medium 比 low 優先,所以 CPU 一直給 medium,low 輪不到、解不了鎖,high 就被卡住。結果是最高優先權的執行緒,被一個中等優先權、而且根本沒碰那把鎖的執行緒無限期擋住,優先權的意義被顛倒了。1997 年火星探路者號反覆重置就是這個原因。

**解法:優先權繼承**。high 在等鎖時,把鎖的持有者 low 的優先權暫時提高到和自己一樣。low 於是能搶過 medium,盡快做完並解鎖;解鎖時優先權恢復原狀。實作在 `mutex_lock()` / `mutex_unlock()`,每條執行緒分別記錄原本的優先權(`base_prio`)與目前有效的優先權(`prio`)。

### 驗證

```
prio: three threads created as low, normal, high
  run order: HNL (expected HNL)
prio: sleep(5 ms) while another thread spins: same priority woke after 10 ticks, high priority after 6
prio: inversion: high waited 20 ticks for the lock without inheritance, 3 with
```

- **順序**:依低、中、高的順序建立三條執行緒,實際執行順序是高、中、低。
- **搶佔延遲**:一條執行緒睡 5ms(換算為 6 個 tick),期間另一條在空轉。兩者同優先權時,醒來要等對方的時間片用完,共 10~11 個 tick;設為高優先權時 6~7 個 tick 就醒來,等於沒有延遲。
- **優先權反轉**:沒有繼承時 high 等了 20~21 個 tick,正是 medium 佔用 CPU 的時間;有繼承時只等 2~4 個 tick。

三個架構結果一致。回歸矩陣 45 個情境結果不變;三個架構各連續執行 20 次全部測試,60 次全部通過。

### 自我測試改為由開機參數啟用

示範越加越多,每次開機都執行會拖慢開機。現在預設只開機,加上 `selftest` 參數才執行各子系統的自我測試與示範(`cmake --build ... --target run` 會自動帶上)。

| 架構 | 只開機 | 加 `selftest` | 加 `selftest memtest stress` |
|---|---|---|---|
| i386 | 0.29 秒 | 0.59 秒 | 0.68 秒 |
| riscv64 | 0.23 秒 | 0.47 秒 | 0.60 秒 |
| aarch64 | 0.20 秒 | 0.44 秒 | 0.52 秒 |

「只開機」的時間幾乎都是 QEMU 本身的啟動。

### 踩坑:示範的對照組失效

為了縮短測試時間,曾把搶佔延遲示範中「空轉」的時間由 30ms 縮為 15ms。結果 30 次執行有 10 次斷言失敗。原因是對照組(同優先權)的執行緒要先等對方一個時間片才開始睡,醒來時對方已經空轉完了,CPU 是閒的,於是和高優先權那組一樣立刻醒來,兩組沒有差別。這是示範的時間安排錯誤,不是排程器的錯誤;恢復為 30ms 並在程式裡註明這段時間的下限怎麼算。

這類偶發失敗只跑一兩次很容易錯過,所以每次改動後都重複執行數十次。

### 已知限制

- **飢餓**:高優先權的執行緒若一直不睡,較低的永遠輪不到。目前沒有防護(例如老化機制:等太久的逐漸提高優先權),使用時要自行確保高優先權的工作大部分時間在等待。 **〔後續:已解決:第 15.5 步加入防飢餓機制。〕**
- **優先權繼承只處理單層**:解鎖時直接回到原本的優先權。若一條執行緒同時持有兩把鎖、兩邊都有人在等,解開其中一把後會提早失去另一邊帶來的提升。也不會沿著「A 等 B、B 等 C」的鏈往下傳遞。 **〔後續:已解決:第 15.5 步改為完整版,支援多把鎖與連鎖等待。〕**
- semaphore 沒有擁有者的概念,無法做優先權繼承;需要保護共用資料時應使用 mutex。
- 等待佇列喚醒時要走訪整條佇列找最高優先權,成本與等待者數量成正比。
- 仍然只支援單一 CPU。 **〔後續:已解決:見第 16~18 步。〕**

---

## 第 15 步:使用者模式、系統呼叫與行程

到上一步為止,所有程式都在 kernel 權限下執行:任何一條執行緒寫錯一個指標,就可能毀掉整個系統。這一步加入使用者模式,讓程式在低權限、各自獨立的位址空間裡執行,做錯事只會害到自己。

| 檔案 | 內容 |
|---|---|
| `kernel/proc.c`、`include/proc.h` | 行程管理、程式載入、系統呼叫 |
| `kernel/vm.c` | 位址空間(`vm_space_*`):每個行程一份頁表 |
| `arch/<名稱>/trap.S`、`trap.c` | 使用者模式與 kernel 之間的進出 |
| `user/init.c`、`usys.h`、`user.ld` | 使用者程式本身 |
| `kernel/userimg.S` | 把使用者程式的二進位嵌入 kernel |

### 三個要素

**1. 權限等級**。CPU 有(至少)兩種模式。使用者模式不能執行特權指令(關中斷、改頁表、存取裝置),也不能存取沒有標記為「使用者可用」的頁。

| | kernel 模式 | 使用者模式 |
|---|---|---|
| i386 | ring 0 | ring 3 |
| RISC-V | S-mode | U-mode |
| AArch64 | EL1 | EL0 |

**2. 位址空間**。每個行程一份自己的頁表,分成兩部分:

- **kernel 部分**:所有行程共用同一份對映(建立時複製 kernel 頁表最上層的項目,下層表格共用)。進入 kernel 時因此不必換頁表。這些頁沒有使用者權限,使用者模式碰不到。
- **使用者部分**:每個行程各自獨立。行程 A 的頁表裡根本沒有行程 B 的頁,這就是行程之間的隔離。

| | 使用者範圍 |
|---|---|
| i386 | 1GB ~ 2GB(`0x40000000`) |
| RISC-V、AArch64 | 4GB ~ 8GB(`0x100000000`) |

選這些位置是因為它們和主記憶體、裝置落在最上層表格的不同項目,複製 kernel 部分時不會互相干擾。

**3. 系統呼叫**。使用者程式要做需要權限的事,只能請 kernel 代勞:把編號與參數放進約定的暫存器,執行一條會觸發例外的指令。

| | 指令 | 編號 | 參數 | 回傳值 |
|---|---|---|---|---|
| i386 | `int $0x80` | `eax` | `ebx` `ecx` `edx` | `eax` |
| RISC-V | `ecall` | `a7` | `a0` `a1` `a2` | `a0` |
| AArch64 | `svc #0` | `x8` | `x0` `x1` `x2` | `x0` |

目前有 8 個:`write`、`exit`、`getpid`、`yield`、`sleep`、`thread_create`、`thread_exit`、`gettid`。

### 進入 kernel 時一定要換 stack

從使用者模式進入 kernel(系統呼叫、中斷、例外)時,kernel **不能繼續使用使用者的 stack**。那個 stack pointer 是使用者程式可以任意設定的值,可能指向沒有對映的位址,也可能指向 kernel 的資料。每條執行緒因此有兩條 stack:使用者模式用的,和進入 kernel 後用的 kernel stack。

三個架構提供的機制不同:

| | 機制 | 需要我們做的事 |
|---|---|---|
| i386 | CPU 從 TSS 的 `esp0` 取得 kernel stack,並自動把使用者的 `ss`、`esp` 推上去 | 每次切換執行緒時更新 `esp0` |
| RISC-V | 硬體不做任何事 | 用 `sscratch` 的約定自己換 |
| AArch64 | 每個等級有自己的 stack pointer(`SP_EL0`、`SP_EL1`),硬體自動切換 | 把 `SP_EL0` 當作被打斷程式的一部分保存 |

RISC-V 的約定是:`sscratch` 在使用者模式執行期間存放 kernel stack 頂端,在 kernel 執行期間為 0。例外入口把 `sp` 與 `sscratch` 交換後看 `sp`:非 0 表示從使用者模式來,此時 `sp` 已是 kernel stack;為 0 表示從 kernel 來,再換回去。這也取代了第 13 步借用 `sscratch` 當暫存空間的做法(現在是先用完再歸 0)。

第 13 步為了 double fault 建立的 TSS,這一步正好用上它的 `esp0`。

### 第一次進入使用者模式

三個架構都沒有「降低權限」的指令。做法是偽造:把狀態擺成「好像剛從使用者模式被打斷」的樣子,再執行從例外返回的指令(`iret`、`sret`、`eret`),CPU 就會「返回」到使用者模式。這和第 10 步讓新執行緒從 `thread_entry` 開始的手法相同。

進入之前,除了第一個參數以外的暫存器全部清為 0。kernel 剛才用過的暫存器裡可能留著 kernel 的位址或資料,不清掉就等於洩漏給使用者程式。

### 使用者程式怎麼來的

目前沒有檔案系統,所以使用者程式嵌在 kernel 裡:

1. `user/init.c` 用同一個編譯器編譯,但**獨立連結**,位址從使用者範圍的起點開始。它和 kernel 沒有任何共用的符號,連 `kprintf` 都沒有,輸出要自己組字串再呼叫 `write`。
2. `objcopy` 轉成原始二進位。linker script 在開頭放了一個 20 位元組的標頭,記錄進入點與各區段的範圍。
3. `kernel/userimg.S` 用 `.incbin` 把它放進 kernel 的唯讀資料區。
4. `proc_spawn()` 依標頭把內容複製到新配置的頁面,並對映到行程的位址空間。

使用者程式同樣遵守 W^X:程式碼唯讀可執行,資料與 stack 可寫不可執行。新配置的頁面先清為 0 再交給行程,避免把別人用過的資料交出去。

### 安全:系統呼叫是 kernel 的對外邊界

系統呼叫的參數全部來自不可信任的程式。最危險的是指標:使用者可以傳任何數值,包括 kernel 內部的位址。`write(kernel 的位址, 16)` 如果 kernel 不檢查就照著讀出來印,使用者就繞過了所有保護。

做法是 `copy_from_user()`:不直接解參考使用者傳來的指標,而是逐頁透過該行程的頁表查出實體位址,並確認那一頁有使用者權限,再經由 kernel 自己的恆等對映去讀。任何一頁不合格就拒絕整個呼叫。

這個做法還有一個好處:RISC-V 的 S-mode 預設不能存取使用者頁(需要另外設定 `sstatus.SUM`),走實體位址就不需要打開它,kernel 意外解參考使用者指標時仍會出錯而不是默默成功。

其他的檢查:不認得的系統呼叫編號回傳錯誤;`sleep` 與 `write` 的長度有上限;`thread_create` 的進入點必須是使用者可存取的位址。i386 另外把系統呼叫以外的中斷向量都設為只有 ring 0 能觸發,使用者程式執行 `int $14` 想偽造 page fault 會得到 #GP。

### 行程出錯怎麼辦

各架構的例外處理程式現在先判斷例外來自哪裡。來自 kernel 的仍然是 panic;來自使用者模式的呼叫 `user_fault()`,印出一行訊息後只終止那個行程:

```
[pid 2 init] killed: Load page fault at pc=0x00000001000007a0 addr=0x0000000080200000
```

### 終止行程:不強行打斷,而是請它自己結束

行程可能有多條執行緒,其中一條呼叫 `exit` 或出錯時,其他執行緒也要結束。但它們可能正在 kernel 裡執行系統呼叫、手上拿著鎖,強行中止會留下不一致的狀態。

做法是把同行程的執行緒標記為「應終止」。被標記的執行緒繼續把手上的事做完,在**下一次要從 kernel 返回使用者模式之前**檢查這個標記並自行結束(`user_return_check()`)。在使用者模式空轉的執行緒也逃不掉,因為計時器中斷每 1ms 就會讓它進一次 kernel。最後一條執行緒結束時釋放整個位址空間。

這也補上了先前列為缺口的「強制終止」:`proc_kill()`。

### 排程器的配合

- 每條執行緒記錄它使用的位址空間,`schedule()` 切換時一併換頁表。
- **換頁表很貴**(TLB 會被清空),所以只在必要時才換:純 kernel 執行緒不需要特定的位址空間,直接沿用上一個行程的頁表,因為 kernel 部分在每份頁表裡都相同。
- 每次切換呼叫 `arch_set_kstack()` 更新 i386 的 `esp0`。

### 驗證

```
user: running a program in user mode
  [user] hello from user mode, pid=1
  [user] thread tid=33 computed 5050
  [user] thread tid=34 computed 20100
  [user] both threads finished, total=25150
user: hello, 2 threads, exit(42) -> exit code 42
  [user] reading kernel memory...
[pid 2 init] killed: Load page fault at pc=0x00000001000007a0 addr=0x0000000080200000
user: read kernel memory -> exit code -1
...
```

`selftest` 會依序執行以下情境,每一個都檢查結束碼:

| 情境 | 預期 | 驗證的事 |
|---|---|---|
| 輸出、建立 2 條執行緒各自計算、`exit(42)` | 結束碼 42 | 使用者模式、系統呼叫、行程內多執行緒共用記憶體 |
| 讀取 kernel 記憶體 | 被終止 | 使用者模式看不到 kernel |
| 寫入自己的程式碼 | 被終止 | 使用者程式的 W^X |
| 執行特權指令 | 被終止 | 權限等級確實生效 |
| 解參考 NULL | 被終止 | |
| 使用者 stack 溢位 | 被終止 | stack 下方沒有對映 |
| 把 kernel 位址傳給 `write` | 呼叫被拒絕,行程繼續,結束碼 7 | 系統呼叫的指標檢查 |
| 不存在的系統呼叫編號 | 回傳錯誤,結束碼 9 | |
| 無窮迴圈,20ms 後 `proc_kill` | 結束碼 99 | 使用者程式會被搶佔;強制終止 |
| 3 個行程同時寫同一個虛擬位址 | 各自的值不受影響 | 行程之間的隔離 |

「被終止」的情境中 kernel 都繼續正常運作並執行下一個測試。全部結束後,核對空閒頁數、頁表頁數、heap 用量都回到測試前的值,確認行程的頁面、頁表、stack 都有歸還。

三個架構結果一致。回歸矩陣 45 個情境結果不變;三個架構各連續執行 20 次全部測試,60 次全部通過。耗時:只開機 0.19~0.31 秒,加 `selftest` 為 0.52~0.70 秒。

### 踩坑:`kprintf` 不支援的格式

測試輸出用了 `%-34s`(靠左對齊),而 `kprintf` 不支援 `%-`。結果格式字串被原樣印出,後面的參數全部錯位,結束碼印成一個很大的數字。程式裡的判斷用的是變數本身所以沒有受影響,但輸出是錯的。改為不用對齊。這個限制從第 3 步就記在文件裡,還是踩到了。

### 已知限制

- **只有一個內建的使用者程式**,以參數選擇行為。沒有檔案系統,也沒有 `exec`、`fork`。 **〔後續:已解決大部分:第 19 步加入檔案系統,第 20 步加入多個程式、`exec` 與從使用者程式建立行程;`fork` 仍未實作。〕**
- **i386 的使用者資料頁可以執行**(沒有 NX)。有了使用者程式之後這個弱點更實際,補上需要改用 PAE 分頁。 **〔後續:已解決:第 15.5 步。〕**
- 沒有使用 ASID(Address Space Identifier) / PCID(Process-Context Identifier),每次換位址空間都清空整個 TLB。 **〔後續:已解決:第 15.5 步在 RISC-V 與 AArch64 使用 ASID,三個架構的 kernel 頁標為全域頁;i386 仍未使用 PCID。〕**
- 行程內的執行緒共用記憶體,但還沒有提供使用者模式的同步機制(mutex 的系統呼叫或 futex);示範中靠輪詢等待。 **〔後續:已解決:第 15.5 步加入 futex 與使用者模式的 mutex。〕**
- 使用者 stack 固定為 16KB,不會自動成長;每個行程最多 16 條執行緒,stack 的位置不重複使用。 **〔後續:第 22 步起 stack 會自動成長,上限是每格 1MB(扣掉一頁保護頁)。〕**
- 被終止的執行緒若正睡在 kernel 裡,要等它醒來才會結束。 **〔後續:已解決:第 21 步先處理了等待主控台輸入的情況;第 23 步改為一般化的做法,終止行程時把它所有正在睡的執行緒直接叫醒。〕**
- kernel 之後若在全新的最上層項目建立對映,已存在的行程看不到(目前所有 kernel 對映都在行程建立前就位)。
- 沒有儲存浮點暫存器,使用者程式也以不使用浮點的方式編譯。
- 「禁止 kernel 意外存取或執行使用者頁」的硬體保護只有 RISC-V 具備(`sstatus.SUM` 預設為 0,S-mode 也永遠不能執行使用者頁)。AArch64 的 PAN(Privileged Access Never) 與 i386 的 SMEP(Supervisor Mode Execution Prevention) / SMAP(Supervisor Mode Access Prevention) 都還沒有啟用。 **〔後續:已解決:第 15.5 步啟用 i386 的 SMEP / SMAP 與 AArch64 的 PAN。〕**

---

## 第 15.5 步:補強前幾步留下的弱點

進入多核心之前,先把第 12~15 步「已知限制」裡屬於弱點的項目解決。多核心會改動每一個子系統,帶著已知的弱點進去只會讓問題更難分辨。

| 弱點 | 來自 | 處理方式 |
|---|---|---|
| 高優先權不睡,低優先權永遠輪不到 | 第 14 步 | 防飢餓(老化,aging) |
| 優先權繼承(priority inheritance)只有單層 | 第 14 步 | 改為完整版:多把鎖、連鎖等待 |
| i386 的資料頁可以執行 | 第 12 步 | 改用實體位址延伸(Physical Address Extension,PAE)分頁,取得禁止執行位元(No-eXecute,NX) |
| kernel 可能被誘導去存取或執行使用者的頁 | 第 15 步 | i386 啟用 SMEP、SMAP;AArch64 啟用 PAN |
| 每次換位址空間都清空 TLB | 第 15 步 | 位址空間編號(Address Space Identifier,ASID)與全域頁(global page) |
| 使用者模式沒有同步機制 | 第 15 步 | 快速使用者空間互斥鎖(Fast Userspace Mutex,futex) |
| 行程建立很慢 | 這一步量測時發現 | 以 `-O2` 編譯;`memcpy` / `memset` 一次處理一個字組 |

### 1. 防飢餓(starvation guard)

固定優先權排程的弱點是:只要有一條高優先權的執行緒一直不睡,所有較低的執行緒就完全停住。

做法是老化(aging):每 10ms 檢查一次各級執行佇列的頭(同一條佇列裡頭是等最久的)。可執行卻等了超過 100ms 的執行緒,暫時提升到最高優先權,執行一次之後(離開 CPU 時)就收回。

這保證低優先權的執行緒仍能緩慢前進:最差情況下每等 100ms 得到一個 10ms 的時間片,約佔 9% 的 CPU。高優先權的工作仍然拿走絕大部分,但系統不會有人完全停擺。

為了支援「暫時提升」,每條執行緒的優先權拆成三個來源,有效優先權由它們算出:

```
有效優先權 = 正處於防飢餓提升 ? 最高 : max(自己的優先權, 繼承來的優先權)
```

### 2. 完整的優先權繼承

第 14 步的版本在解鎖時直接回到原本的優先權,有兩種情況處理不對。新的定義是:

> 一條執行緒「繼承來的優先權」= 它持有的**所有** mutex 上,所有等待者中最高的有效優先權。

每條執行緒記錄自己持有的 mutex 串列,以及正在等哪一把 mutex。這個定義自然涵蓋:

- **同時持有多把鎖**:L 持有 A 和 C,H 在等 A。L 解開 C 時依「剩下持有的鎖」重新計算,A 上還有 H 在等,所以提升保留。舊版會在這裡提早掉回低優先權。
- **連鎖等待**:H 等 M 持有的鎖,M 又在等 L 持有的鎖。H 的優先權先傳給 M,再沿著「M 正在等的鎖」傳給 L,否則 L 還是輪不到。傳遞最多 8 層,同時避免死結(deadlock)時無窮迴圈。

驗證(三個架構相同):

```
prio: chained inheritance: low thread ran at priority 6, 6 after releasing an unrelated lock, 2 after releasing all
prio: starvation guard: low thread got 3 boosts and made progress (3241767 iterations) under a 150 ms high-priority hog
```

低優先權(2)的執行緒經過兩層傳遞後以高優先權(6)執行;解開無關的鎖後仍是 6;全部解開後回到 2。防飢餓方面,高優先權的執行緒連續佔用 CPU 150ms 的期間,低優先權的執行緒得到 3 次提升並持續有進展。

### 3. i386 改用 PAE 分頁

傳統的 32 位元頁表項目沒有地方放「不可執行」,所以第 12 步的 i386 上資料頁一律可以執行。PAE 把項目加寬為 64 位元,最高位元就是 NX。

| | 傳統 32 位元分頁 | PAE |
|---|---|---|
| 層數 | 2 | 3 |
| 最上層 | 頁目錄(page directory),1024 項 | 頁目錄指標表(Page Directory Pointer Table,PDPT),只有 4 項 |
| 項目大小 | 32 位元 | 64 位元 |
| 大分頁 | 4MB | 2MB |
| NX 位元 | 沒有 | 有(bit 63) |

改成 PAE 之後 i386 的結構(3 層、每層 9 位元、2MB 大分頁)和 RISC-V、AArch64 幾乎相同,共用的頁表走訪程式不需要修改。需要個別處理的只有三點:

- **PDPT(Page Directory Pointer Table) 的項目格式不同**:只允許「存在」位元,可寫、使用者位元都是保留位元,設了會出錯。產生表格項目的函式因此多一個「在第幾層」的參數。
- **PDPT 會被 CPU 快取**:寫入 `CR3` 時 CPU 把 4 個 PDPT 項目讀進內部暫存器,之後修改 PDPT 必須重新寫入 `CR3` 才生效。在最上層新增項目時要通知架構層。
- **NX 要先啟用再使用**:要設定延伸功能啟用暫存器(Extended Feature Enable Register,EFER(Extended Feature Enable Register))的 NXE 位元。沒啟用就在項目裡設 bit 63,CPU 會把它當成保留位元而觸發 page fault。所以功能偵測移到建立頁表之前。

所有功能都先用 `CPUID` 指令偵測,CPU 不支援就退回沒有該保護的模式。QEMU 預設的 `qemu32` 型號沒有 NX,所以啟動腳本改用 `-cpu max`(提供 QEMU 能模擬的所有功能);可用環境變數 `QEMU_CPU` 改回去測試退回路徑。

### 4. 禁止 kernel 碰使用者的頁

kernel 讀寫使用者資料一律透過 `copy_from_user()`,不直接解參考使用者的指標。但如果某處程式寫錯了,硬體應該擋下來,因為「誘導 kernel 去存取或執行攻擊者控制的記憶體」是提權攻擊最常用的手法。

| | 禁止 kernel **執行**使用者的頁 | 禁止 kernel **讀寫**使用者的頁 |
|---|---|---|
| i386 | 管理者模式執行保護(Supervisor Mode Execution Prevention,SMEP) | 管理者模式存取保護(Supervisor Mode Access Prevention,SMAP) |
| AArch64 | 特權不可執行位元(Privileged eXecute Never,PXN),第 15 步已設定 | 特權永不存取(Privileged Access Never,PAN) |
| RISC-V | S-mode 永遠不能執行 U 頁,硬體內建 | `sstatus` 的 SUM 位元(permit Supervisor User Memory access)預設為 0 |

SMAP 啟用後,唯一需要直接寫使用者頁的地方是 i386 第一次進入使用者模式時在使用者 stack 上放參數,那裡用 `stac` / `clac` 兩條指令暫時解除再恢復。

### 5. 換位址空間時不再清空整個 TLB

轉譯後備緩衝區(Translation Lookaside Buffer,TLB)是頁表查詢結果的快取。第 15 步每次切換行程都把它清空,切回來之後所有位址都要重新查頁表。

**ASID(RISC-V、AArch64)**:TLB 的每筆紀錄附帶一個編號,只有編號與目前相同的才會被使用。每個行程一個編號,切換時只要換編號,原本的紀錄留在 TLB 裡,切回來立刻可用。

- 編號 1~254 分配給行程,0 給 kernel 自己的頁表,255 是編號用完時大家共用的(用這個編號的行程切換時仍要清空)。
- 行程結束時先清掉 TLB 裡屬於它的紀錄再歸還編號。這一步是安全上必要的:否則下一個拿到同一編號的行程會看到上一個行程的對映。
- RISC-V 的規範允許硬體實作少於 16 位元的 ASID,開機時把 ASID 欄位全寫 1 再讀回來偵測;不夠用就退回每次清空。

**全域頁(三個架構)**:kernel 的頁在每份頁表裡都相同,標成全域之後不屬於任何編號,切換時不受影響。i386 的 32 位元模式沒有 ASID(行程上下文識別碼 Process-Context Identifier,PCID 只存在於 64 位元模式),但寫入 `CR3` 時不會清掉全域頁的紀錄,所以 kernel 的部分同樣保留下來,只有使用者的頁需要重查。

`stress` 新增連續建立並結束 300 個行程的階段。可用的編號只有 254 個,所以一定會重複使用;每個行程都要正常執行並得到正確的結束碼,確認沒有受到前一個使用同編號者的殘留影響。

這項改動的效益沒有單獨量測:QEMU 的 TLB 是軟體模擬的,行為與實機差異很大,量出來的數字不具代表性。

### 6. futex:使用者模式的同步機制

第 15 步的使用者程式要等其他執行緒時只能輪詢,也沒有辦法保護共用資料。

futex 的想法是:鎖本身只是使用者記憶體裡的一個整數。沒有競爭時,上鎖與解鎖各只是使用者模式下的一條原子指令,**完全不進 kernel**。只有在「拿不到鎖要睡覺」或「解鎖時有人在等要叫醒」這兩種情況才需要系統呼叫。多數情況下鎖沒有競爭,所以這比每次都進 kernel 的做法快得多。

kernel 提供兩個系統呼叫:

- `futex_wait(addr, val)`:如果 `*addr` 仍然等於 `val`,就讓呼叫者睡在這個位址上。
- `futex_wake(addr)`:喚醒睡在這個位址上的執行緒。

`futex_wait` 要連數值一起傳的原因,和第 11 步的 lost wakeup 相同:使用者程式剛看到鎖被佔用、還沒睡下去,對方就解鎖並喚醒(此時沒人在睡),之後才睡下去就再也沒人叫了。把「再確認一次數值」交給 kernel 在關中斷之下做,檢查與睡下去之間就沒有空檔;數值若已改變,代表情況變了,直接返回讓使用者程式重試。

使用者端的互斥鎖寫在 `user/usys.h`,用三個數值表示狀態:0 沒人持有、1 有人持有、2 有人持有而且有人在等。解鎖時只有在原本是 2 的情況才呼叫 `futex_wake`。原子操作用編譯器內建的 `__sync_*` 函式(i386 的 `lock cmpxchg`、RISC-V 的 `lr` / `sc`、AArch64 的 `ldxr` / `stxr`)。

行程被終止時,kernel 會主動喚醒它所有睡在 futex 上的執行緒,否則這些執行緒不會醒來,行程也就永遠結束不了。

驗證:3 條執行緒各對共用計數器做 20000 次「上鎖、讀出、加 1、寫回、解鎖」,期間會被計時器搶佔。

```
  [user] 3 threads x 20000 locked increments = 60000
user: user-mode mutex (futex), 3 threads -> exit code 11
```

### 7. 效能:最佳化編譯與字組複製

`stress` 加入「連續建立 300 個行程」之後,量到每個行程要 1.6~2ms,慢得不合理(建立一條 kernel 執行緒只要幾微秒)。原因有兩個:

- **整個 kernel 一直是以不最佳化的方式編譯的**。建置設定從第 1 步起就沒有指定最佳化等級,編譯器預設是 `-O0`。
- **`memcpy` / `memset` 逐位元組處理**。建立一個行程要清空或複製十來個頁面(程式、stack、頁表),約 40KB。

修正:預設以 `-O2` 編譯(要用 GDB 逐行追蹤時可用 `-DWELYOS_OPT=-O0` 改回);`memcpy` / `memset` 在位址對齊時一次處理一個機器字組。

| 連續建立 300 個行程(tick = 1ms) | 修正前 | 修正後 |
|---|---|---|
| i386 | 478 | 55 |
| riscv64 | 565 | 80~90 |
| aarch64 | 605 | 74 |

快了約 7~9 倍。這是 QEMU 上的數字,但兩項改動在實機上同樣有效。

開啟最佳化後出現兩個問題,都在使用者程式:

- 編譯器會自行產生對 `memset` 的呼叫(把結構清為 0)。使用者程式沒有連結任何函式庫,也不能用 kernel 裡的那一份,所以要自己提供。
- 使用者程式裡名為 `main` 的 `static` 函式在取位址時連結失敗(編譯器對 `main` 這個名稱有特殊處理)。改名為 `umain`。

kernel 本身在 `-O2` 下沒有出現任何問題,51 個回歸情境與 60 次重複執行全部通過。這也算是一次額外的驗證:未定義行為或遺漏 `volatile` 之類的錯誤,通常要開了最佳化才會現形。

### 驗證

新增兩個開機參數,確認硬體保護真的生效(而不只是設定了位元):

| 參數 | 動作 | i386 | riscv64 | aarch64 |
|---|---|---|---|---|
| `crash=uaccess` | kernel 直接讀取使用者的頁 | Page fault | Load page fault | Data abort |
| `crash=uexec` | kernel 跳到使用者的頁執行 | Page fault(`err=0x11`,指令擷取) | Instruction page fault | Instruction abort |
| `crash=exec`(第 12 步既有) | kernel 跳到資料頁執行 | **Page fault**(之前只能略過) | Instruction page fault | Instruction abort |

回歸矩陣(QEMU 結束碼),三個架構各 17 個情境,共 51 個,結果全部符合預期;三個架構各連續執行 20 次全部測試,60 次全部通過。

| 架構 | 只開機 | `selftest` | `selftest memtest stress` | `uaccess` | `uexec` | `stack` | `mainstack` | `null` | `wtext` | `exec` | `relock` | `thread` | `dfree` | `break` | `panic` | `illegal` | `fault` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| i386 | 0 | 0 | 0 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 0 | 3 | 3 | 3 |
| riscv64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |
| aarch64 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 1 | 1 | 1 |

耗時:只開機 0.18~0.29 秒;加 `selftest` 為 0.65~0.84 秒(測試項目增加了);全部測試 0.83~1.04 秒。

另外以 `QEMU_CPU=qemu32` 執行 i386 的自我測試,確認在沒有 NX、SMEP、SMAP 的 CPU 上會正確退回並完成所有測試。

### 仍然存在的限制

- **防飢餓的參數是寫死的**(100ms、提升到最高)。被提升的執行緒在那一個時間片內會搶過所有人,包括真正高優先權的工作,對有嚴格時限的即時工作來說是干擾。
- **futex 的等待者依位址分成 16 桶**,同一桶的會一起被喚醒(醒來後各自重新檢查,不影響正確性,但有多餘的喚醒)。使用者模式的 mutex 沒有優先權繼承。
- **ASID 用完後不會回收重整**:第 255 個以後同時存在的行程共用一個編號,切換時要清空 TLB。
- **PAE 的 64 位元項目在 32 位元 CPU 上要分兩次寫入**,不是不可分割的。單一 CPU 且關中斷時沒有問題,多核心下需要注意寫入順序。
- **i386 沒有啟用 PAE 之外的 36 位元實體位址**,仍只使用 4GB 以下的記憶體。
- 還沒有檔案系統、`exec`、`fork`,使用者程式仍只有內建的一個。這些是尚未實作的功能,列在後續步驟。 **〔後續:第 19、20 步加入檔案系統、多個程式與 `exec`;`fork` 仍未實作。〕**

---

## 第 16 步:多核心第一段 — spinlock 與每顆 CPU 的狀態

多核心(Symmetric Multiprocessing,SMP)是一大步,分三段做,每一段都能獨立驗證:

| 段 | 內容 | 狀態 |
|---|---|---|
| 一 | spinlock、每顆 CPU 的狀態、把所有「關中斷就安全」的地方改成真正的鎖 | **本步** |
| 二 | 找出並啟動其他 CPU(RISC-V 用 SBI 的 HSM(Hart State Management) 擴充、AArch64 用 PSCI(Power State Coordination Interface)、i386 用 LAPIC(Local Advanced Programmable Interrupt Controller) 的 INIT-SIPI),讓它們進入排程器 | 下一步 |
| 三 | 每顆 CPU 的計時器、處理器間中斷(Inter-Processor Interrupt,IPI)、TLB shootdown | 之後 |

這一段**仍然只有一顆 CPU 在跑**。目的是先把 kernel 改成「就算有多顆 CPU 同時執行也正確」的寫法,並確認在單一 CPU 上行為完全不變。這樣第二段真的啟動其他 CPU 時,出問題的範圍只剩「啟動」本身,不會和上鎖的錯誤混在一起。

### 為什麼「關中斷」不夠了

到第 15.5 步為止,所有共用資料(執行佇列、位元圖、heap 的空閒串列、頁表)都靠同一招保護:

```c
unsigned long flags = arch_irq_save();      // 關中斷
... 修改共用資料 ...
arch_irq_restore(flags);
```

單一 CPU 上這是對的:唯一會打斷這段程式的就是中斷,關掉就沒有人能插進來。多顆 CPU 時,關中斷只管得到**自己這一顆**,別顆 CPU 同一時間可能正在修改同一份資料。

### spinlock(自旋鎖)

`include/spinlock.h`、`kernel/spinlock.c`。鎖是一個整數:0 = 沒人持有,1 = 有人持有。

上鎖用一條**原子**(atomic)指令「把 1 寫進去並取回舊值」:舊值是 0 代表搶到了;是 1 代表別人持有,就原地反覆重試(自旋)。各架構對應的指令由編譯器的 `__atomic_exchange_n` 產生:i386 是 `xchg`,RISC-V 是 `amoswap.w`,AArch64 是 `ldxr` / `stxr` 這一對。

```c
while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
    while (l->locked)            // 搶不到:先用一般的讀取等它變空
        arch_cpu_relax();
}
```

內層多一個迴圈是 **TTAS**(Test and Test-And-Set)寫法:原子交換每次都要獨佔那條快取線(cache line),多顆 CPU 一起反覆交換會互相拖慢;單純讀取可以共享,等待的成本低得多。`arch_cpu_relax()` 是給 CPU 的提示(i386 `pause`、AArch64 `yield`),表示現在只是在空等。

使用時幾乎都用關中斷的版本:

```c
unsigned long flags = spin_lock_irqsave(&lock);     // 先關中斷,再上鎖
...
spin_unlock_irqrestore(&lock, flags);
```

**上鎖的同時必須關中斷**。否則持有鎖的時候進來一個中斷,而中斷處理程式也要拿同一把鎖,它就會永遠等待被它自己打斷的那段程式。

### 安全:把「永遠卡住」變成「立刻回報」

spinlock 用錯的典型後果是整台機器靜靜地卡死,而且中斷是關的,連計時器都進不來,非常難查。所以每把鎖多記兩樣東西(持有者的 CPU 編號、名稱),每顆 CPU 記自己持有幾把鎖,用來在出錯的當下直接 panic:

| 錯誤 | 不檢查的後果 | 現在 |
|---|---|---|
| 同一顆 CPU 對同一把鎖上鎖兩次 | 自己等自己,永遠卡住 | `spinlock "名稱": recursive acquire on cpu N` |
| 解開不是自己持有的鎖 | 別人的臨界區失去保護,資料悄悄損壞 | `spinlock "名稱": released on cpu N but not held by it` |
| 持有 spinlock 時切換執行緒 | 別顆 CPU 關著中斷一直等這把鎖,系統卡住 | `schedule: cpu N would switch threads while holding K spinlock(s)` |
| 沒持有排程器的鎖就操作等待佇列 | 佇列損壞 | `KASSERT(spin_held(&sched_lk))` |

檢查的成本是每次上鎖、解鎖多幾條指令,換來的是這類錯誤在第一次發生時就被指出來。

還有一個細節:**panic 本身也要印訊息**,而出事的當下可能正持有主控台的鎖(例如在 `kprintf` 裡面出錯)。所以 panic 一開始就呼叫 `spin_panic_mode()`,之後所有 spinlock 操作直接放行。各架構的致命例外路徑(印 `*** EXCEPTION ***` 的地方)也改為先呼叫 `panic_begin()`。

### 每顆 CPU 的狀態(per-CPU)

「目前正在執行的執行緒」「是否在中斷中」「需不需要重新排程」「載入的是哪個位址空間」這些資訊每顆 CPU 都不同,不能再是單一個全域變數。它們集中到 `struct cpu`(`include/cpu.h`),每顆 CPU 一份:

```c
struct cpu {
    uintptr_t kstack_limit;      // 以下三項由例外入口(組合語言)以固定位移存取
    uintptr_t kstack_top;
    uintptr_t scratch[2];
    int id, online, in_irq;
    volatile int need_resched;
    int nlocks;                  // 持有幾把 spinlock
    struct thread *current, *idle, *zombie;
    struct vm_space *loaded_space;
};
extern struct cpu cpus[MAX_CPUS];   // MAX_CPUS = 8
```

問題是:一段程式怎麼知道「我在哪一顆 CPU 上」?三個架構的做法不同:

| 架構 | `this_cpu()` 的來源 | 進出使用者模式時 |
|---|---|---|
| RISC-V | `tp` 暫存器固定指向自己的 `struct cpu` | 使用者可以任意改 `tp`,所以入口要換回來(見下) |
| AArch64 | 系統暫存器 `TPIDR_EL1` | 不必處理:使用者模式碰不到這個暫存器 |
| i386 | 讀工作暫存器 TR(Task Register),換算成編號 | 不必處理:TR 不受使用者模式影響 |

**RISC-V**:`sscratch` 的約定跟著改。之前它在使用者模式期間存放 kernel stack 的頂端;現在存放 `struct cpu` 的位址(kernel 中仍為 0)。入口把 `tp` 與 `sscratch` 交換,非 0 就是從使用者模式來,此時 `tp` 已經是 `struct cpu`,再從裡面讀出 `kstack_top` 換 stack。每顆 CPU 有自己的 `sscratch`,所以各自拿到的是自己那一份。

**AArch64**:`TPIDR_EL1` 原本被例外入口借來暫放 `x0`,現在要讓出來。入口改成:`x0` 暫放在 `TPIDRRO_EL0`,`x1` 暫放在 `struct cpu` 裡的空位。整個入口 19 條指令,仍在每個向量 32 條的限制內。

**i386**:沒有多餘的暫存器。常見做法是用 `fs` / `gs` 區段指向每顆 CPU 的資料,但那樣每次進出使用者模式都要保存、切換區段暫存器,多一個容易出錯的地方。這裡利用一個現成的事實:**每顆 CPU 本來就必須有自己的 TSS**(裡面記錄該 CPU 的 kernel stack),所以 TR 的值每顆 CPU 都不同。用 `str` 指令讀出 TR 就能換算出編號,不需要動到區段暫存器,也不需要更改例外入口。代價是 `str` 比讀一個區段暫存器慢幾個週期。

### 一個只有多核心才會出現的陷阱:`this_cpu()` 的結果會過期

```c
struct cpu *c = this_cpu();
// <-- 如果這裡被搶佔,之後在「另一顆 CPU」上被切回來
c->current ...                 // 讀到的是原本那顆 CPU 的資料
```

規則:**`this_cpu()` 的結果只能在中斷關閉時使用**。受影響的地方有三處,都已處理:

1. `thread_current()`、`thread_id()` 這類在中斷開著時呼叫的函式,改為先關中斷再讀。
2. `schedule()` 切換回來之後,不可再使用切換前取得的 `c`(那時可能已經換了 CPU)。
3. **RISC-V 的例外返回路徑不還原 `tp`**。例外入口會把所有暫存器存進 trap frame、返回前還原;但被打斷的 kernel 執行緒可能在這次中斷裡被搶佔,之後在另一顆 CPU 上被切回來,若把 `tp` 還原成當初存的值,它就指向原本那顆 CPU 了。`tp` 在 kernel 中代表「現在這顆 CPU」,不屬於被打斷程式的狀態,所以返回 kernel 時跳過它(返回使用者模式時才還原使用者的 `tp`)。

這三處在單一 CPU 上都測不出問題,是讀程式推導出來的,第二段啟動多顆 CPU 後才會真正被考驗。

### 排程器:一把鎖,而且鎖要「跨過切換」

排程器的所有資料(執行佇列、休眠串列、所有等待佇列、每條執行緒的狀態與優先權)由**同一把鎖** `sched_lk` 保護;semaphore、mutex、futex、行程的結束狀態也共用它:

```c
unsigned long flags = sched_lock();
while (條件不成立)
    wq_sleep(&佇列);
...
sched_unlock(flags);
```

和之前「關中斷」的寫法一一對應,所以 `sync.c` 的邏輯完全沒變,只是換了保護的方式。

關鍵在 `schedule()`:**鎖在切換前由舊執行緒取得,切換後由新執行緒釋放**。

```
舊執行緒:sched_lock() → schedule() → 把自己放回執行佇列 → arch_switch ─┐
新執行緒:                                    ┌──────────────────────────┘
                                             └→ 從它上次的 arch_switch 返回 → sched_unlock()
```

為什麼不能在切換前就解鎖:舊執行緒在切換完成前就已經被放回執行佇列。如果這時鎖已釋放,別顆 CPU 可能立刻把它取出來執行,而原本的 CPU 還沒離開它的 stack,**兩顆 CPU 同時踩在同一條 stack 上**。鎖一直持有到切換完成,別顆 CPU 在那之前拿不到佇列裡的任何東西。新建立的執行緒第一次執行時沒有「上次的 `sched_unlock`」可以返回,所以 `thread_entry()` 開頭要自己解鎖。

其他配合的修改:

- **回收已結束的執行緒**移到解鎖之後。它要釋放 stack 與結構,會用到 heap、頁表、頁面配置器的鎖,不該被排程器的鎖包住。每顆 CPU 有自己的 `zombie` 欄位:執行緒結束時把自己記在上面,由同一顆 CPU 上切換過去的下一條執行緒在 `sched_unlock()` 時釋放。
- **喚醒時要挑一顆 CPU**(`kick()`):找「正在執行的優先權最低、且低於被喚醒者」的 CPU(閒置的最優先),設定它的 `need_resched`。已經被要求重新排程的 CPU 略過,讓同時被喚醒的多條執行緒分散到不同的 CPU。只有一顆 CPU 時,結果和原本的判斷相同。
- **每顆 CPU 一條 idle 執行緒**:一條執行緒不能同時在兩顆 CPU 上執行。
- **計時器 tick**:每顆 CPU 都會有自己的計時器中斷,但系統時間只由第 0 顆 CPU 推進(否則 N 顆 CPU 時間會走快 N 倍),喚醒休眠者與防飢餓檢查也只由它負責;時間片則各算各的。

### 決策:為什麼是一把大鎖,而不是每顆 CPU 一條佇列

第 10 步的紀錄提過,要讓排程器隨 CPU 數量擴展,需要每顆 CPU 自己的執行佇列加上 work stealing。這裡刻意先不做:

| | 一把鎖、一組佇列(本步) | 每顆 CPU 一組佇列 |
|---|---|---|
| 正確性 | 所有狀態在同一把鎖下,容易確認 | 喚醒、搬移、優先權繼承都會跨佇列,要同時持有多把鎖並注意順序 |
| 優先權語意 | 天然滿足「N 顆 CPU 執行優先權最高的 N 條」 | 各佇列各自排序,要另外平衡才能接近 |
| 效能 | 排程操作互相等待;CPU 越多越明顯 | 平常互不干擾 |

以目前的規模(模擬器上最多幾顆 CPU、臨界區只有幾十條指令),鎖的競爭不是瓶頸;先求正確,等第二、三段完成、能實際量測之後,再決定值不值得換。這個取捨列在已知限制。

### 其他子系統的鎖與順序

| 鎖 | 保護的資料 | 持有時可以再拿 |
|---|---|---|
| `sched_lk` | 排程器、等待佇列、semaphore / mutex / futex、行程狀態 | `console` |
| `vm_lk` | kernel 頁表、所有位址空間的頁表、ASID 的分配 | `pmm`、`console` |
| `heap_lk` | kernel heap 的空閒串列 | `pmm`、`console` |
| `pmm_lk` | 頁面位元圖 | `console` |
| `stack_lk` | 閒置 stack 的快取 | `console` |
| `console_lk` | 主控台輸出(一次 `kprintf` 不被插入) | 無(最內層) |

需要同時持有兩把鎖時,只會依表中的方向取得(例如 heap 擴充時 `heap_lk` → `pmm_lk`,從不反過來),所以不會出現兩顆 CPU 以相反順序各拿一把而互相等待的死結。`sched_lk` 持有期間刻意不呼叫任何會配置或釋放記憶體的函式。

`proc.c` 原本大量依賴「關著中斷一路做完」(例如行程結束時遞減計數、釋放位址空間、喚醒等待者是同一段)。現在拆成:需要不可分割的狀態變更在 `sched_lk` 下做,花時間的工作(建立執行緒、釋放位址空間、`kfree`)移到鎖外。

### 順帶修正:`proc_wait` 的懸空指標

改寫 `proc_wait` 時發現一個既有的錯誤(單一 CPU 也會發生):兩條執行緒同時等同一個行程時,兩者都被喚醒,第一條回收並釋放了 `struct proc`,第二條醒來後仍用睡前拿到的指標讀 `p->zombie`,讀的是已釋放的記憶體。改為每次醒來都以 pid 重新尋找(pid 不會重複使用),找不到就代表已被別人回收。

### 驗證

行為應與上一步完全相同,所以主要的驗證是回歸測試;另外加了三項針對這一步的測試:

| 測試 | 內容 | 結果(三個架構相同) |
|---|---|---|
| `selftest` 中的 spinlock 自我測試 | 上鎖 / 解鎖的記帳、巢狀持有兩把不同的鎖、解鎖後中斷恢復 | `spinlock: selftest ok (cpu 0 of 1 online, max 8)` |
| `crash=spin` | 同一把 spinlock 上鎖兩次 | `KERNEL PANIC: spinlock "test": recursive acquire on cpu 0` |
| `crash=spinsleep` | 持有 spinlock 時呼叫 `thread_yield()` | `KERNEL PANIC: schedule: cpu 0 would switch threads while holding 1 spinlock(s)` |

回歸矩陣:三個架構各 19 個情境(上一步的 17 個加上面兩個),共 57 個,結束碼全部符合預期(成功為 0;故意出錯的情境 i386 為 3、其餘為 1)。三個架構各連續執行 20 次 `autoexit selftest stress memtest`,60 次全部通過。`QEMU_CPU=qemu32` 的退回路徑也通過。

第一次建置後就全部通過,過程中沒有踩到坑;因為每一處修改都對應到原本的一個關中斷區段,邏輯沒有變。

耗時與上一步相比沒有可辨識的差異(單一 CPU 上鎖永遠一次就拿到):

| | 只開機 | `selftest` | 全部測試(20 次的範圍) |
|---|---|---|---|
| i386 | 0.30 秒 | 0.86 秒 | 1.00~1.09 秒 |
| riscv64 | 0.23 秒 | 0.75 秒 | 0.90~0.98 秒 |
| aarch64 | 0.19 秒 | 0.70 秒 | 0.83~0.93 秒 |

壓力測試的數字也相近:1000 條執行緒 23~65 tick,300 個循序行程 60~89 tick。

### 已知限制(這一段刻意留下,由後兩段處理)

- **還沒有第二顆 CPU**。上面所有「多核心下才會出錯」的推論(鎖跨過切換、`tp` 不還原、`this_cpu()` 過期)都還沒有被真正的並行執行驗證過,只確認了單一 CPU 上沒有退步。 **〔後續:部分解決:第 17 步啟動了其他 CPU,並以並行測試驗證 spinlock、`this_cpu()` 與每顆 CPU 的例外處理;「鎖跨過切換」與「`tp` 不還原」要等其他 CPU 參與排程才會被驗證。〕**
- **沒有 IPI**。`kick()` 選到別顆 CPU 時只設定旗標,對方要到自己的下一個計時器 tick 才會發現(最多 1ms)。panic 時也還不會讓其他 CPU 停下來。 **〔後續:已解決:第 18 步。〕**
- **TLB 失效只作用在自己這顆 CPU**。多核心下修改頁表後要通知其他 CPU(TLB shootdown);ASID 的回收也有同樣的問題。 **〔後續:已解決:第 18 步。〕**
- **kernel 執行緒沿用上一個行程的頁表**(第 15 步的最佳化)在多核心下不安全:某顆 CPU 的 idle 執行緒可能還載入著一個已經在別顆 CPU 上被釋放的位址空間。第二段要先處理這一點。 **〔後續:已解決:第 18 步在釋放位址空間前通知所有 CPU 換回 kernel 頁表。〕**
- **i386 的 PAE 項目分兩次寫入**(第 15.5 步列過)在多核心下需要注意順序。
- **例外專用的 stack 全系統只有一條**。兩顆 CPU 同時發生 kernel stack 溢位時會互相覆蓋;結果仍是停機,但訊息可能交錯。
- **i386 的 TSS 與 double fault stack 還是只有一份**,GDT 也只配置了第 0 顆 CPU 的項目;`this_cpu()` 的換算方式已經為多顆預留。 **〔後續:部分解決:第 17 步讓每顆 CPU 有自己的 TSS;double fault 用的 TSS 與 stack 仍只有一份。〕**
- **spinlock 不保證公平**:TTAS 寫法下,等最久的不一定先拿到。CPU 很多、競爭激烈時應改用排隊式的鎖(ticket lock)。
- **一把排程鎖**的擴展性限制,如上面的決策所述。

---

## 第 16.1 步:可替換的執行佇列,與無鎖環狀佇列

這一步做兩件事:把執行佇列從排程器裡抽出來,變成一個可以整個換掉的元件;然後照這個介面寫出第二種做法 — 每顆 CPU 一組無鎖(lock-free)環狀佇列(circular queue)加上工作竊取(work stealing)。

先講結論,免得誤會:**目前只有一顆 CPU,而且排程器的大鎖(第 16 步的 `sched_lk`)還在**,所以無鎖佇列現在帶不來任何加速,單一 CPU 上反而每次操作多做一點事。這一步的價值是把結構擺好、把演算法在真正的並行下驗證過;效益要等多顆 CPU 啟動、並把大鎖的範圍縮小之後才看得到。

### 介面:`include/runq.h`

```c
void            runq_init(void);
const char     *runq_name(void);
void            runq_push(struct rq_node *n, int prio, int front);   // 放入
struct rq_node *runq_pop(void);                                      // 取出優先權最高的
void            runq_reprio(struct rq_node *n, int prio);            // 佇列中的節點改優先權
int             runq_highest(void);                                  // 目前最高的優先權(參考值)
int             runq_peek(int prio, struct rq_node **out, int max);  // 等最久的候選者(防飢餓用)
```

為了讓實作真的能換,定了三條界線:

1. **介面只認得 `struct rq_node`**(嵌在 `struct thread` 裡的一小塊),不認得 `struct thread`。實作因此不依賴排程器的內部結構,也可以單獨拿出來測試。
2. **上鎖由實作自己負責**。所有函式都可以由任何 CPU 同時呼叫,呼叫者不必持有任何鎖。串列版自己帶一把鎖,環狀版幾乎不用鎖,排程器不需要知道差別。
3. **策略留在排程器**。優先權怎麼算、何時搶佔、防飢餓的門檻都在 `sched.c`;執行佇列只負責存放與取出。

選擇實作是建置時的選項,排程器的程式一行都不用改:

```
cmake --preset riscv64 -DWELYOS_RUNQ=ring     # 預設
cmake --preset riscv64 -DWELYOS_RUNQ=list
```

要嘗試第三種做法(例如每顆 CPU 一把鎖的串列),只要新增 `kernel/runq_<名稱>.c` 實作這七個函式。開機時會印出目前用的是哪一個(`sched: run queue = ...`)。

### 實作一:`runq_list.c`(對照與退路)

就是第 14 步的做法搬出來:每個優先權一條串列加一個位元圖,全系統共用,以一把鎖保護。順便改成雙向串列,「從中間移除」(改優先權時需要)由走訪整條變成固定成本。順序最嚴格,但所有 CPU 搶同一把鎖。

### 實作二:`runq_ring.c`

#### 無鎖環狀佇列(`include/ring.h`)

一個固定大小(64 格)的陣列,加上兩個只增不減的計數器:

```
       head                 tail
        v                    v
  [ ][ ][A][B][C][D][ ][ ][ ]        索引 = 計數器 % 大小,用到尾端就繞回開頭
```

- `tail`:下一個要放入的位置。**只有擁有這條佇列的 CPU 會放入**。
- `head`:下一個要取出的位置。擁有者取自己的工作、別顆 CPU 來竊取,走同一個函式,用 CAS(Compare-And-Swap)競爭:「如果 `head` 還是我剛才看到的值,就把它加一」。成功的人拿走那一格,失敗的重來。

這種「單一放入者、多個取出者」的組合稱為 SPMC(Single Producer Multiple Consumer),Go 語言排程器每顆 CPU 的本地佇列就是這個結構。不需要鎖的理由:

- 放入者只寫 `tail` 和 `tail` 指到的那一格,而那一格在 `tail` 前進之前沒有人會讀。
- 取出者先讀格子的內容、再用 CAS 確認 `head` 沒變。那一格要被新資料覆蓋,`head` 必須先越過它;所以 CAS 成功就保證剛才讀到的內容有效,失敗時讀到的東西直接丟掉。
- 計數器只增不減(32 位元,繞一圈要 40 億次操作),不會遇到 ABA 問題。

為了維持「只有擁有者放入」,**被喚醒的執行緒放進「喚醒者」這顆 CPU 的佇列**,再由 `kick()` 通知該讓出的 CPU 來取(竊取)。

#### 每顆 CPU 的結構與取出順序

| 每顆 CPU | 用途 |
|---|---|
| `ring[p]` | 每個優先權一個環 |
| `front[p]` | 每個優先權一格「插隊位」:被搶佔、時間片還沒用完的工作放這裡,最先取出 |
| `mask` | 哪些優先權可能有工作,給別顆 CPU 看的提示 |

全系統另有一組**溢出串列**(有鎖):環放不下時進這裡,也是各 CPU 之間自然的平衡點。

取出時由高優先權到低,同一優先權內依序找:自己的插隊位 → 自己的環 → 溢出串列 → 別顆 CPU 的插隊位與環。**先把同一優先權在所有地方找過,才往下一級**,所以取到的幾乎一定是全系統優先權最高的,只差在別顆 CPU 同時放入的那一瞬間。這比常見的「自己的佇列空了才去偷」更接近嚴格的優先權排程,代價是每次取出要多看幾個提示。

#### 難題:無鎖結構不能從中間移除

優先權繼承與防飢餓都需要把一條**已經在排隊**的執行緒換到別的優先權。環狀佇列做不到「從中間拿掉」。解法是改變格子的意義:

> 環裡的每一格不是「這個工作在這裡」,而是一張**號碼牌**:「這個工作可能可以執行」。
> 真正決定誰能執行它的,是節點上的旗標 `queued`。

- **放入**:`queued = 1`,然後放一張號碼牌。
- **取出**:拿到一張牌之後,用 CAS 把 `queued` 從 1 改成 0。成功才算拿到;失敗表示這張牌已經**作廢**(工作早被別人經由另一張牌取走),丟掉再拿下一張。

於是同一個工作可以有好幾張牌,只有第一個兌換的人成功:

| 動作 | 做法 |
|---|---|
| 提高優先權 | 不動舊的牌,在新優先權的環再放一張。舊的那張之後被拿到時作廢 |
| 降低優先權 | 什麼都不做。之後從舊的(較高的)環被拿到時,發現優先權不符,重新放到正確的環 |

這帶出兩個必須處理的後果:

**1. 作廢的牌可能指向已經結束的執行緒。** 拿到牌之後要去讀它的 `queued`,如果那塊記憶體已經還給 heap、被拿去做別的用途,就是讀寫不該碰的記憶體。所以 `struct thread` 改由排程器自己的池子管理:結束後放回池子重複使用,**永遠不還給 heap**(type-stable memory)。如果那塊記憶體已經被新的執行緒使用,而新執行緒正好也在排隊,經由舊牌把它取走也是正確的 — 它本來就該被執行,只是提早了一點。順帶的好處是建立執行緒不必每次配置與釋放結構。

**2. 作廢的牌會擋住防飢餓的檢查。** 防飢餓只看每條佇列的頭(等最久的)。如果頭是一張作廢的牌,而這條低優先權的環又一直輪不到(這正是飢餓的情況),後面真正在等的執行緒就永遠不會被發現。所以 `runq_peek` 看到頭是作廢的牌時會順手把它清掉。

#### 溢出串列與號碼牌的配合

溢出串列用節點自己的指標串起來,所以「已經被取走執行的節點還留在串列上」是不允許的。但節點可能同時有一張環裡的牌,被別顆 CPU 經由那張牌取走。兩邊用對稱的順序配合:

```
串入的一方:(持有鎖)串入並標記「在串列上」 → 再讀 queued,若已是 0 就自己拆下來
兌換的一方:CAS 把 queued 改成 0             → 再讀標記,若在串列上就(持有鎖)拆下來
```

兩邊都是「先寫自己的、再讀對方的」,並使用最嚴格的記憶體順序,所以不可能兩邊都沒看到對方,至少有一邊會負責拆下來。

### 順帶修正:防飢餓提升後不肯下來

換成環狀版後,`selftest` 在防飢餓測試卡住。原因是一個既有的規則漏洞,只是之前被巧合遮住:

- 被提升的執行緒此刻是最高優先權;時間片用完時的規則是「有優先權不低於自己的人在等才換人」,而沒有人比它高,所以它**一直執行下去**,反過來霸佔 CPU。
- 之前沒出事,是因為同時在等的 main 執行緒過了 100ms 也被提升到最高,才把它換下來。環狀版裡 main 在插隊位上,防飢餓檢查沒看那裡,巧合就消失了。

兩處都修了:靠提升上來的執行緒時間片用完時**無條件**讓出(這才符合「只給一個時間片」的原意);`runq_peek` 也把插隊位列為候選。這是「換實作」帶來的額外收穫:兩個實作行為不一致的地方,往往就是原本靠運氣的地方。

### 驗證

**(一)主機上的並行測試**(`tests/host/runq_test.c`,以 `bash scripts/host-test.sh` 執行)

kernel 裡只有一顆 CPU,測不到無鎖結構最容易出錯的「多方同時操作」。因為介面不依賴 kernel,同一份 `runq_ring.c` / `runq_list.c` 可以直接編譯成一般的 Linux 程式,讓 4 條 pthread 各扮演一顆 CPU,同時隨機地取出、放回(隨機優先權與插隊)、暫時留在手上(模擬睡著)、更改任意節點的優先權。檢查:

- **互斥**:同一個節點不會同時交給兩條執行緒(在 kernel 裡就是同一條執行緒同時在兩顆 CPU 上執行)
- **不遺失**:結束後把佇列取空,每個節點恰好出現一次,而且由高優先權到低

| 實作 | 3000 個節點(大量使用溢出串列) | 200 個節點(幾乎只用環與竊取) | ThreadSanitizer |
|---|---|---|---|
| ring | 通過(每條 300 萬次操作,共約 720 萬次取出) | 通過 | 沒有回報資料競爭 |
| list | 通過 | 通過 | 沒有回報資料競爭 |

以上連續執行三輪結果相同。ThreadSanitizer(TSan)是編譯器的工具,會檢查每一次記憶體存取有沒有資料競爭。

也確認了測試本身有效:把「兌換號碼牌」的 CAS 故意拿掉後,測試立刻回報大量「同一節點交給兩顆 CPU」。但另一處故意改壞(拿掉溢出串列「串入後再檢查一次」)即使跑 2000 萬次也**沒有**被測到 — 那個時間窗極小。所以那段邏輯的正確性目前靠的是推導,不是測試。

**(二)kernel 回歸測試**

| | ring(預設) | list |
|---|---|---|
| 57 個情境(三個架構各 19 個)的結束碼 | 全部符合預期 | 全部符合預期 |
| `autoexit selftest stress memtest` 每個架構 20 次 | 60 次全部通過 | (未重複執行) |
| `QEMU_CPU=qemu32` 退回路徑 | 通過 | (未執行) |

**(三)耗時**

kernel(單一 CPU,ring):只開機 0.22~0.31 秒,`selftest` 0.68~0.86 秒,全部測試 0.87~1.40 秒。和第 16 步相比最小值相近,最大值偏高(riscv64 有一次 1.40 秒);單次量測的雜訊不小,無法斷言是環狀版造成的,但也沒有變快的理由。

主機上 4 條執行緒各做 500 萬次操作的總耗時(僅供參考,測試程式的鎖在搶不到時會讓出 CPU,和 kernel 裡的行為不同):

| 節點數 | ring | list |
|---|---|---|
| 3000 | 1.37 秒 | 1.39 秒 |
| 200 | 1.44 秒 | 1.57 秒 |
| 60 | 1.34 秒 | 1.67 秒 |

節點多時兩者沒有差別:環只有 64 格,多數工作其實是經由有鎖的溢出串列進出。節點少時環狀版快一到兩成。這個差距不大,而且測試裡有 15% 的操作是隨機改優先權(會產生大量作廢的牌),比實際的排程負載不利得多;真正的評估要等多核心啟動後,在 kernel 裡量。

### 已知限制

- **目前沒有效益**。排程器仍在 `sched_lk` 之下呼叫執行佇列,環狀版「不用鎖」的特性被外層的鎖蓋住了。要讓它發揮,得先縮小 `sched_lk` 的範圍,而那需要處理另一個問題:執行緒被放回佇列時還踩在自己的 stack 上,必須等切換完成才能讓別顆 CPU 執行它(目前靠大鎖跨過切換來保證,縮小鎖之後要改用每條執行緒的「仍在 CPU 上」旗標)。這是多核心啟動之後的工作。 **〔後續:第 18 步在多核心下實測:環狀版比串列版多完成約三成工作,主要來自執行緒較少換 CPU;大鎖仍在。〕**
- **真正的並行只在主機測試中驗證過**,kernel 裡還沒有第二顆 CPU。 **〔後續:部分解決:第 17 步在 kernel 裡以多顆 CPU 同時取出,測試了環狀佇列本身;完整的執行佇列(號碼牌、溢出串列)仍只在主機測試中驗證。〕**
- **環的容量固定為 64**。同一優先權、同一顆 CPU 上超過 64 個可執行的工作時走溢出串列,那時的行為和串列版相同(有鎖)。壓力測試的 1000 條執行緒就是這種情況。
- **多顆 CPU 之間不保證嚴格的先進先出**:同優先權的工作分散在各自的環與溢出串列裡。
- **作廢的號碼牌會佔用環的空間**,直到被取出時丟掉。優先權頻繁變動時,有效容量會變小。
- **`runq_highest()` 是參考值**:別顆 CPU 的提示可能過期(它下次取出時才更新),偶爾會造成一次不必要的重新排程,不影響正確性。
- **竊取一次只拿一個**。Go 的做法是一次拿走一半,可以減少竊取的次數,之後量測有需要再改。
- **`struct thread` 的記憶體不歸還 heap**。同時存在過的執行緒數量上限決定了佔用量(1000 條約 200KB)。
- **溢出串列「串入後再檢查」的競爭沒有被測試涵蓋**,如上所述。

---

## 第 17 步:多核心第二段 — 啟動其他 CPU

這一步讓其他 CPU 真的開始執行 kernel 的程式。三個架構都能啟動到 8 顆(`MAX_CPUS`),每顆完成自己的初始化、通過並行測試,然後**待命**。讓它們參與排程是第三段的工作,因為那需要每顆 CPU 自己的計時器與處理器間中斷(IPI)。

為什麼不一次做到底:排程需要搶佔,搶佔需要計時器中斷,而其他 CPU 的計時器在三個架構上是三套不同的東西(尤其 i386 要改用 LAPIC 的計時器)。先確認「啟動」本身正確,下一段出問題時範圍才小。

### 名詞:BSP 與 AP

開機時只有一顆 CPU 在執行,稱為 BSP(Bootstrap Processor);其他的稱為 AP(Application Processor),停在韌體裡等 kernel 叫醒。

### 共用的流程:`kernel/smp.c`

```
smp_init()
  1. arch_smp_discover()   問架構層:除了自己還有哪些 CPU(硬體編號)
  2. 對每一顆:
       配置它的 struct cpu 與自己的 stack(16KB)
       arch_smp_start()    叫醒它
       等它回報(逾時 200ms 就放棄這一顆,繼續下一顆)
  3. arch_smp_done()       收拾啟動用的暫時資源
  4. selftest 時:所有 CPU 一起跑並行測試;否則直接待命
```

AP 醒來後由架構層的組合語言設定好 stack 與「自己的 `struct cpu`」,再依序做三件每顆 CPU 都要各自做一次的事:安裝例外入口、開啟分頁、呼叫 `smp_ap_main()` 回報。

兩個設計上的選擇:

- **一次只啟動一顆**,等它回報才啟動下一顆。啟動過程中有些資料是所有 AP 共用的(例如 i386 跳板要讀的 stack 位址),同時啟動會互相覆蓋。代價是啟動時間與 CPU 數成正比,但每顆只需不到 1ms。
- **頁表共用、設定各自做**。kernel 的頁表全系統只有一份,但「開啟分頁」與 NX、SMEP、PAN 這些功能位元存在每顆 CPU 自己的控制暫存器裡,每顆都要設定一次(`vm_enable_secondary()`)。漏掉的話那顆 CPU 就沒有這些保護。

### 三個架構怎麼找到、怎麼叫醒其他 CPU

| | RISC-V | AArch64 | i386 |
|---|---|---|---|
| 有哪些 CPU | 裝置樹 `/cpus/cpu@N` 的 `reg`(hartid) | 裝置樹 `/cpus/cpu@N` 的 `reg`(MPIDR) | ACPI 的 MADT(LAPIC ID) |
| 叫醒的方法 | SBI 的 HSM 擴充 `hart_start` | PSCI 的 `CPU_ON`(以 `hvc` 呼叫) | LAPIC 送出 INIT 與 SIPI |
| AP 醒來時的狀態 | S-mode、分頁關閉 | EL1、分頁關閉 | **真實模式**(16 位元) |
| 能否傳參數給 AP | 可以(`a1`) | 可以(`x0`) | 不行 |

RISC-V 與 AArch64 很直接:韌體提供一個呼叫,指定「哪一顆、從哪裡開始、帶什麼參數」。參數就傳那顆 CPU 的 `struct cpu` 位址,AP 的入口(`boot.S` 的 `ap_entry`)只有四五條指令:把它放進 `tp` / `TPIDR_EL1`、從裡面取出 stack、跳進 C。

裝置樹的解析(`kernel/fdt.c`)加了 CPU 清單:`/cpus` 底下名稱以 `cpu@` 開頭的節點,其 `reg` 屬性就是硬體編號,長度由 `/cpus` 的 `#address-cells` 決定。

#### 踩坑(預先避開):RISC-V 開機的不一定是 0 號 hart

原本的 `boot.S` 寫著「只讓 hart 0 往下執行,其他休眠」。單核心時沒問題,但多核心時 OpenSBI **隨機挑一顆** hart 進入 kernel,實測 4 顆時是 2 號、8 顆時是 5 號。照原本的寫法,那顆 hart 會直接休眠,整台機器開不了機。

改為「第一顆到達的 hart 繼續」:用一條原子交換指令搶一個旗標,搶到的往下走。正常情況下只有一顆會到達(其他停在韌體裡),這個旗標只是防範舊版韌體讓所有 hart 同時進來。旗標放在 `.data` 而不是 `.bss`,否則會被開機流程自己清掉。

由此也確立一個原則:**kernel 內的 CPU 編號(`cpu->id`)和硬體編號(`cpu->arch_id`)是兩回事**。開機的那顆永遠是 0 號,不論它在硬體上是幾號。

#### i386:三個額外的麻煩

**1. 沒有裝置樹,要讀 ACPI。** 在 `0xE0000`~`0xFFFFF` 之間找 `"RSD PTR "` 開頭的 RSDP → 它指向 RSDT → RSDT 列出其他表格的位址 → 找到簽章為 `"APIC"` 的 MADT → 裡面類型 0 的紀錄各代表一顆 CPU。每張表都有檢查碼(所有位元組相加為 0),長度也設了上限,不合格就放棄並維持單核心。這些表格位在 BIOS 保留的區域,不屬於 kernel 管理的記憶體,平常沒有對映,讀之前先以**唯讀**對映。

**2. AP 醒來時是 16 位元的真實模式**,只能執行 1MB 以下的程式,而 kernel 在 1MB 以上、是 32 位元的。所以需要一段跳板(trampoline,`arch/i386/ap_tramp.S`):

```
kernel 把跳板複製到 0x8000
AP 從 0x8000 開始(真實模式)
  → 載入臨時的 GDT,設定 CR0.PE,遠跳躍       進入 32 位元保護模式
  → 載入與 BSP 相同的 CR4、EFER、CR3、CR0     開啟分頁(含 NX、SMEP、SMAP)
  → 換到 kernel 準備的 stack,跳進 arch_ap_start()
```

幾個細節:

- 跳板被組譯時在 kernel 映像裡,執行時卻在 `0x8000`,所以它引用自己內部的位址時要手動換算。
- 開啟分頁後的下一條指令仍在 `0x8000`,所以這一頁必須在頁表中對映為可執行。做法是先對映成可寫、複製完改成唯讀可執行(維持 W^X),所有 AP 啟動後**移除對映**,不在低位址留下一頁可執行的程式。
- CR3 用的是 kernel 的頁表,不是 BSP 當下的 CR3:後者可能是某個行程的頁表(第 15 步的延遲切換)。
- 放跳板前先確認 `0x8000` 沒有和 bootloader 留下的資料重疊(QEMU 把 multiboot 資訊放在 `0x9500`,很近)。

**3. 每顆 CPU 要有自己的 TSS。** 第 16 步選擇用工作暫存器 TR 來辨識 CPU,這裡補上對應的實作:GDT 擴充到 21 項,第 7 項起每兩項屬於一顆 AP。AP 進入 C 之後第一件事是載入 kernel 的 GDT、IDT 與**自己的** TSS(`trap_init_ap`),在那之前 `this_cpu()` 還不能用。TSS 裡的 `esp0`(從使用者模式進入 kernel 時用的 stack)也因此變成每顆 CPU 各自一份。

INIT 與 SIPI 的時序依規範:INIT 之後等 10ms 再送 SIPI,SIPI 送兩次。

### 效能:開機時間

使用者在意開機耗時,所以特別量了這一步的影響(`autoexit`,5 次取最快):

| | 1 顆 CPU | 4 顆 CPU | 8 顆 CPU |
|---|---|---|---|
| i386(第一版) | 0.30 秒 | **0.46 秒** | — |
| i386(調整後) | 0.30 秒 | 0.31 秒 | 0.34 秒 |
| riscv64 | 0.31 秒 | 0.32 秒 | — |
| aarch64 | 0.24 秒 | 0.24 秒 | — |

i386 第一版多出的 0.16 秒幾乎全部來自那 10ms 的等待(模擬器裡閒置等待的實際耗時遠大於 10ms)。兩個調整:

1. **INIT 用「除了自己以外的所有 CPU」一次送出**,只等一次,而不是每顆各等 10ms。
2. **在模擬器 / 虛擬機裡把等待縮短為 1ms**。10ms 是給實體 CPU 完成重設用的,模擬的 CPU 重設是瞬間完成的。判斷依據是 CPUID 的 hypervisor 位元(ECX bit 31),它由虛擬機管理程式設定,實體 CPU 上恆為 0,所以在實機上仍會等足 10ms。

`selftest` 與全部測試在 4 顆 CPU 時比 1 顆慢約 0.05~0.15 秒,來自並行測試本身與模擬器多開的執行緒。

QEMU 的 CPU 數由環境變數 `QEMU_SMP` 控制,預設 4。要最快的開機可用 `QEMU_SMP=1`。

### 驗證

`selftest` 時所有 CPU 同時執行一份並行測試。這是 kernel 裡**第一次有真正同時執行的程式**,所以也順便驗證前兩步只能靠推導的東西:

| 項目 | 做法 | 結果 |
|---|---|---|
| 例外處理 | 每顆 AP 各自觸發一次中斷點例外並返回 | 三個架構每顆 AP 都印出 `[trap] breakpoint ... resuming` |
| `this_cpu()` | 每顆 CPU 反覆確認拿到的是自己的 `struct cpu`(共 2 萬次) | 0 次錯誤 |
| spinlock | 所有 CPU 同時對同一個計數器加一,各 2 萬次 | 總和完全正確(例:8 顆 = 160000) |
| 對照組 | 同樣的遞增但不上鎖 | 偶爾少掉(見下) |
| 無鎖環狀佇列 | BSP 放入 63 個項目,所有 CPU 同時取出 | 每個項目恰好被取走一次,總和正確 |

不上鎖的對照組在不同次執行中少了 0、1、2、20 次不等。少掉代表兩顆 CPU 讀到同一個舊值、各自加一後寫回,只算了一次 — 這正是需要鎖的原因,也證明這些 CPU 確實是同時在執行。偶爾為 0 是因為測試很短,碰巧沒有撞上。

實際啟動的情況:

```
riscv64, 8 顆:  smp: 8 cpus started (hardware ids: 5 0 1 2 3 4 6 7)
aarch64, 8 顆:  smp: 8 cpus started (hardware ids: 0 1 2 3 4 5 6 7)
i386,    8 顆:  smp: 8 cpus started (hardware ids: 0 1 2 3 4 5 6 7)
```

回歸測試(預設 4 顆 CPU):

- 三個架構各 19 個情境,共 57 個,結束碼全部符合預期。
- `autoexit selftest stress memtest`:三個架構 × 2、4、8 顆 CPU 各 7 次,加上稍早 4、8 顆各 10 次,共 123 次全部通過。
- `QEMU_SMP=1`(三個架構)與 `QEMU_CPU=qemu32`(i386 沒有 NX、SMEP、SMAP 的退回路徑,4 顆 CPU)都通過。

這一步三個架構都是第一次執行就成功,沒有踩到坑;RISC-V 開機 hart 的問題是事先讀規範發現的。

### 已知限制

- **AP 啟動後只是待命**(關著中斷休眠),還沒有執行任何執行緒。`cpu_count`(參與排程的 CPU 數)仍是 1。 **〔後續:已解決:第 18 步讓它們進入排程器。〕**
- **AP 沒有計時器、也收不到中斷**。第三段要加上每顆 CPU 的計時器(i386 要改用 LAPIC 的計時器)與 IPI。 **〔後續:已解決:第 18 步。〕**
- **panic 時其他 CPU 不會停下來**。目前它們都在休眠所以沒有影響;參與排程之後需要用 IPI 讓它們停止。 **〔後續:已解決:第 18 步以 IPI 要求它們停止。〕**
- **AP 的 stack 沒有保護頁**,溢位不會被立刻發現(BSP 與所有執行緒的 stack 都有)。
- **i386 的 double fault 處理全系統只有一份**(TSS 與 stack),兩顆 CPU 同時發生時會互相干擾。處理程式已改為從 `link` 欄位找出是哪顆 CPU 出事。
- **i386 找 CPU 只支援 ACPI 1.0 的 RSDT**,不讀 64 位元的 XSDT,也不搜尋 EBDA(延伸 BIOS 資料區);找不到時維持單核心。只認 8 位元的 LAPIC ID(255 顆以下)。
- **「在虛擬機裡縮短 INIT 等待」依賴 CPUID 的 hypervisor 位元**。若某個虛擬機管理程式設了這個位元、卻需要較長的重設時間,AP 會啟動失敗(會印出訊息並繼續以較少的 CPU 執行,不會當機)。
- **啟動失敗的 CPU 不會重試**,它的 stack 也不回收(那顆 CPU 也許只是慢了)。
- 第 16 步列出的多核心限制(TLB 失效只作用在自己這顆、kernel 執行緒沿用上一個行程的頁表、i386 的 PAE 項目分兩次寫入)仍然存在,在 AP 參與排程之前必須處理。

---

## 第 18 步:多核心第三段 — 其他 CPU 參與排程

這一步之後,執行緒真的會同時在多顆 CPU 上執行。需要四樣東西:每顆 CPU 自己的計時器、處理器間中斷(IPI)、跨 CPU 呼叫(用來做 TLB shootdown),以及把前兩步列為「多核心下不安全」的地方補起來。

### 每顆 CPU 自己的計時器

時間片與搶佔靠計時器中斷,而計時器中斷只會送到「那一顆」CPU。所以每顆都要有自己的:

| 架構 | AP 的計時器 | 備註 |
|---|---|---|
| RISC-V | 同樣用 SBI 的 `set_timer`,它本來就是每顆 hart 各自的 | 「下一次的預定時間」由全域變數改成放在 `struct cpu` |
| AArch64 | 同樣用 generic timer,每顆 CPU 各有一個 | 中斷控制器(GIC)有一部分設定是每顆 CPU 私有的,每顆都要設定一次 |
| i386 | **LAPIC 內建的計時器**(向量 48) | 開機的 CPU 繼續用 PIT;PIT 只有一個,而且只接到開機的 CPU |

i386 的 LAPIC 計時器頻率因機器而異,要先校正:讓它從最大值開始倒數,用第 12 步之後校正好的 TSC 等一個 tick 的時間,看它少了多少。校正只在開機的 CPU 做一次,其他 CPU 沿用結果。

**系統時間只由第 0 顆 CPU 推進**(第 16 步已經這樣寫),喚醒休眠者與防飢餓檢查也只由它負責。其他 CPU 的計時器中斷只做一件事:扣自己正在執行的那條執行緒的時間片。

### 處理器間中斷(IPI)

硬體只提供「叫一下對方」,不能附帶內容。所以內容放在共用的記憶體裡:

```c
void smp_send(struct cpu *c, unsigned what)
{
    __atomic_fetch_or(&c->ipi_pending, what, ...);   // 先在對方的 struct cpu 上記下要做什麼
    arch_ipi_send(c);                                // 再叫它
}
```

| 種類 | 用途 |
|---|---|
| `IPI_RESCHED` | 你該重新排程了(有更優先的執行緒可以給你執行) |
| `IPI_CALL` | 執行跨 CPU 呼叫(見下) |
| `IPI_STOP` | 系統 panic 了,立刻停下來 |

三個架構的發送方式:

| 架構 | 發送 | 接收 |
|---|---|---|
| RISC-V | SBI 的 IPI 擴充 `send_ipi` | S-mode 軟體中斷(`scause` 中斷碼 1),處理時要自己清除待處理位元 |
| AArch64 | 寫入 GIC 的 `GICD_SGIR`,產生 SGI(Software Generated Interrupt) | 和計時器一樣從 GIC 認領中斷號,這裡用第 1 號 |
| i386 | 寫入 LAPIC 的 ICR,固定向量 49 | 一般的中斷閘;處理完要寫 LAPIC 的 EOI 暫存器 |

i386 為此新增三個中斷向量:48(LAPIC 計時器)、49(IPI)、255(假中斷:LAPIC 偶爾會發出、不需要處理也不需要回應的中斷)。開機的 CPU 也要啟用自己的 LAPIC 才收得到 IPI,同時確認傳統中斷控制器接的那隻腳(LINT0)維持「外部中斷」模式,PIT 才能照常運作。

有了 `IPI_RESCHED`,第 16 步的 `kick()` 補上最後一塊:選到別顆 CPU 時立刻通知它,不必等它的下一個 tick。

### 跨 CPU 呼叫與一個容易忽略的死結

`smp_call_others(fn, arg)`:讓其他所有 CPU 各執行一次 `fn(arg)`,**全部做完才返回**。用於「必須讓每顆 CPU 都知道」的變更。

實作本身很短(設定函式、對每顆發 `IPI_CALL`、等回應數到齊)。難的是死結:

```
CPU A:持有鎖 L,發起跨 CPU 呼叫,等 B 回應
CPU B:關著中斷,正在等鎖 L          <-- 收不到 IPI,永遠不會回應
```

A 等 B 回應,B 等 A 放鎖。這不是罕見的巧合:頁表的鎖正是這種情況(A 在鎖內改了頁表,需要其他 CPU 清 TLB;B 同時也想改頁表)。

解法是讓**所有「關著中斷等待」的地方主動檢查**有沒有別人的要求(`smp_poll()`):

- spinlock 的等待迴圈:每轉一圈檢查一次。沒有要求時只是一次記憶體讀取。
- 跨 CPU 呼叫自己取得鎖、等待回應的迴圈也一樣(兩顆 CPU 可能同時發起)。

於是 B 在等鎖的同時就把 A 的要求做完了。代價是對 `fn` 的限制:它可能在別人等鎖的半途執行,所以**不可以取得任何鎖**,只能動到那顆 CPU 自己的狀態。目前只有兩個用途(清 TLB、換回 kernel 頁表),都符合。

panic 的 `IPI_STOP` 也經由同一個檢查點處理,所以即使某顆 CPU 正關著中斷卡在等鎖,也停得下來。

### TLB shootdown

每顆 CPU 有自己的 TLB;在一顆 CPU 上執行失效指令只清得到它自己的。其他 CPU 若繼續用舊的紀錄,就會存取到已經移除的對映。最直接的受害者是第 13 步的 stack 保護頁:保護頁被移除對映之後,別顆 CPU 若還留著舊紀錄,寫進去也不會出錯。

| 架構 | 做法 |
|---|---|
| AArch64 | 有會「廣播」的失效指令(`tlbi vaae1is`、`tlbi aside1is`,結尾的 `is` 表示通知所有 CPU),硬體自己處理,不需要 IPI |
| RISC-V、i386 | 以跨 CPU 呼叫請每顆 CPU 各自執行一次 |

**決策:只在「原本有效的對映被移除或更改」時才做。** 新增對映(原本無效)不做:CPU 不會把「無效」這件事存進 TLB,其他 CPU 第一次用到時自然會查到新的內容。這省掉絕大多數的 shootdown — 行程載入時每對映一頁都通知所有 CPU 的話,成本會高到不能接受。(RISC-V 的規範允許硬體快取無效的項目;QEMU 不會,實機上若遇到需要另外處理「多餘的 page fault」。列在已知限制。)

### 補起來的三個洞

**1. kernel 執行緒沿用上一個行程的頁表**(第 16 步列為不安全)。行程的最後一條執行緒在 CPU A 上結束並釋放頁表時,CPU B 的 idle 執行緒可能還載入著同一份。現在釋放之前先以跨 CPU 呼叫問每顆 CPU:「你還載入著它嗎?是的話換回 kernel 的頁表」,順便清掉 TLB 裡屬於這個位址空間編號(ASID)的紀錄。這樣保留了原本的最佳化(平常不必為了切到 kernel 執行緒而換頁表),成本只發生在行程結束時。

**2. 行程結束的順序。** 上面的做法有個前提:釋放時不能還有執行緒「屬於」這個位址空間,否則排程器會再把它載入。原本每條執行緒是先把自己從執行緒數扣掉、最後一條才處理位址空間;多核心下,別的執行緒可能還在另一顆 CPU 上走結束流程,被搶佔再排回來時就會載入已釋放的頁表。改為**每條執行緒先放掉位址空間,再扣數**。

**3. `thread_join` 返回時對方還沒被回收。** 單一 CPU 時,等待者被叫醒、輪到它執行,代表對方已經切換出去並被回收了。多核心下,對方在另一顆 CPU 上剛宣告結束,等待者就在這顆 CPU 上繼續跑,此時對方的 stack 還沒釋放。功能上沒有問題,但核對記憶體用量的測試會看到「還沒還的」。`sched_trim()` 現在會先等到沒有待回收的執行緒。

### AP 怎麼進入排程器

AP 啟動後的流程變成:

```
初始化(例外、分頁、接收 IPI)→ 回報 → [並行測試] → 待命(開著中斷休眠)
   → 收到通知 → 登記進排程器 → 啟動自己的計時器 → 成為這顆 CPU 的 idle 執行緒
```

- **待命改為開著中斷休眠**(第 17 步是關著中斷永久休眠)。這樣不佔用主機的運算資源,又能被 IPI 叫醒。AP 可能剛好在「檢查完、正要休眠」的瞬間錯過那次中斷,所以開機的 CPU 每個 tick 對還沒就緒的再叫一次。
- **AP 目前的執行流程直接登記成它的 idle 執行緒**,就像開機流程被登記成 main 執行緒一樣,不必另外建立。
- 所有 AP 都登記完,才把 `cpu_count` 一次設成總數。在那之前排程器與執行佇列不會把工作派給它們。
- 閒置的 CPU 每個 tick 會看一下有沒有人在排隊(包括別顆 CPU 的佇列),有就去拿。平常有工作時會被 IPI 直接通知,這是保險。

### 為什麼有些測試要在其他 CPU 加入之前跑

第 10~15 步的示範有不少是在檢查「單一 CPU 上的排程行為」:三條不同優先權的執行緒誰先執行、高優先權如何搶佔、防飢餓何時介入。多顆 CPU 時這些問題的答案不同(三條執行緒可以同時執行,沒有「誰先」),測試的前提就不成立了。

所以啟動分成兩個呼叫:`smp_init()` 叫醒其他 CPU 並讓它們待命;跑完單一 CPU 的示範後,`smp_sched_start()` 才讓它們加入。之後的測試(多核心排程示範、使用者模式、壓力測試)都在真正的多核心下執行。沒有 `selftest` 參數時,兩個呼叫緊接著執行。

### 驗證

新增的測試:

| 測試 | 內容 | 結果(4 顆 CPU) |
|---|---|---|
| 多核心排程示範 | 8 條忙碌的執行緒各跑 30ms,記錄各自待過的 CPU | 每顆 CPU 都執行過執行緒;約 66~70 tick 完成(只有一顆 CPU 的話要 240) |
| 同上 | 執行緒是否在 CPU 之間移動 | 每次有 1~2 條換過 CPU |
| mutex | 8 條執行緒同時對同一個計數器加一(各 1000 次) | 8000,完全正確 |
| `crash=smp` | 每顆 CPU 都在忙的時候 panic | 其他 CPU 被叫停,訊息完整,以預期的結束碼結束 |

既有的 `stress`(1000 條執行緒、300 個行程)與使用者模式的測試現在都在多核心下執行,同時考驗了頁表釋放、TLB shootdown、行程結束順序。

回歸測試:

- 三個架構各 20 個情境(原本 19 個加 `crash=smp`),共 60 個,結束碼全部符合預期。
- `autoexit selftest stress memtest`:三個架構 × 2、3、4、8 顆 CPU,共 **270 次全部通過**;串列版的執行佇列另外 24 次也全部通過。
- 單核心(`QEMU_SMP=1`)與 i386 舊 CPU 型號(`QEMU_CPU=qemu32`)通過。

這一步同樣沒有遇到需要除錯的失敗。原因我認為是前兩步把風險拆開了:鎖的規則在單一 CPU 上先確立並加了檢查,啟動流程單獨驗證過,這一步真正新的東西只剩 IPI 與跨 CPU 呼叫。不過「沒測到失敗」不等於沒有錯,並行的錯誤可能要更長時間或不同的時序才會出現。

### 無鎖環狀佇列的第一次實測

第 16.1 步說要等多核心啟動後在 kernel 裡量。用多核心排程示範的數字比較兩種執行佇列(4 顆 CPU、8 條忙碌執行緒、30ms,各量 8 次):

| | ring:完成的迴圈次數 | list:完成的迴圈次數 | ring:換過 CPU 的執行緒 | list |
|---|---|---|---|---|
| i386 | 平均 1,008,830(901,984~1,125,230) | 平均 776,987(726,049~837,338) | 1.4 | 4.9 |
| riscv64 | 平均 590,821(551,891~641,791) | 平均 434,370(388,377~475,524) | 2.2 | 4.8 |
| aarch64 | 平均 467,241(448,826~490,951) | 平均 360,030(337,567~373,060) | 1.9 | 5.6 |

同樣的時間內,環狀版完成的工作多 30%~36%,而且兩者的範圍沒有重疊。主要差別應該是**執行緒留在原本的 CPU 上**:環狀版每顆 CPU 先取自己佇列裡的工作,執行緒換 CPU 的次數只有串列版的三到四分之一。這是「每顆 CPU 各自的佇列」帶來的好處,和「無鎖」關係不大 — 排程器的大鎖還包在外面。

要保留的是:這是模擬器上的數字,而且只量了一種負載(純運算、執行緒數是 CPU 數的兩倍)。趨勢對環狀版有利,但倍數不能當成實機的預期。

**〔後續:第 21 步發現「迴圈次數」這個指標在模擬器上不可靠 — 同樣的 CPU 時間能跑幾圈,會因執行緒的 stack 配置在哪一頁而差到 5 倍。所以上表中「完成的迴圈次數」的差距有多少來自執行佇列、多少來自記憶體配置的巧合,無法分辨,30%~36% 這個數字不應採信。「換過 CPU 的執行緒數」是直接計數,不受影響,仍然成立。〕**

### 效能:多核心的代價

| (4 顆 CPU) | 第 17 步 | 本步 |
|---|---|---|
| 只開機 | 0.24~0.31 秒 | 0.21~0.34 秒 |
| `selftest` | 0.73~0.89 秒 | 0.77~0.96 秒 |
| 1000 次循序「建立+等待結束」 | 2~9 tick | 16~38 tick |
| 300 個循序行程 | 60~93 tick | 89~208 tick |

循序的建立與等待變慢了,這是預期中的:新執行緒被派到閒置的 CPU(要發 IPI 叫醒它),結束時又要叫醒在原本那顆 CPU 上等待的人,一來一回都是跨 CPU 的通知;行程結束時還多了跨 CPU 呼叫。在模擬器上,叫醒一顆休眠的 CPU 特別慢(主機要喚醒對應的執行緒)。真正能平行的工作則是快了:上面的 8 條執行緒只花了單核心四分之一多一點的時間。

### 已知限制

- **排程器的大鎖還在**。所有 CPU 的排程操作仍然互相等待。要縮小它,需要先解決「執行緒被放回佇列時還踩在自己的 stack 上」的問題(第 16.1 步已說明)。這是下一步的候選。
- **新建立或被喚醒的執行緒一律優先派給閒置的 CPU**,不考慮它上次在哪顆 CPU 執行(快取、TLB 還是熱的),也不考慮喚醒者是否馬上就要睡了。循序的建立與等待因此比單核心慢。
- **mutex 一搶不到就睡**。持有者正在別顆 CPU 上執行、馬上就要放鎖時,先自旋一小段會比睡下去再被叫醒快得多(adaptive mutex)。測試中 8 條執行緒搶同一把鎖,每次上鎖都可能觸發一次切換與一次 IPI。
- **跨 CPU 呼叫一次只能有一個在進行**,而且發起者要等所有 CPU 回應。CPU 很多、頁表頻繁變動時會成為瓶頸。 **〔後續:「效能調整:多核心下的 TLB 失效與跨 CPU 通知」讓通知只送給真正需要的 CPU,大幅減少呼叫的次數與對象;一次只能有一個在進行的限制仍在。〕**
- **TLB shootdown 不處理「新增對映」**。x86 與 ARM 上這是正確的;RISC-V 的規範允許硬體快取無效的項目,那種硬體上別顆 CPU 可能多發生一次 page fault,而目前的 page fault 處理不會重試。QEMU 上不會發生。
- **panic 時只「要求」其他 CPU 停止,不等它們真的停下**。它們通常在幾微秒內停止,但在那之前仍可能輸出訊息,和 panic 的訊息交錯。
- **i386 的 AP 計時器不補記遺失的 tick**(開機的 CPU 有)。AP 的計時器只用來算時間片,少算幾個 tick 只會讓時間片略長。
- **AP 的 idle 執行緒用的是啟動時的 stack,沒有保護頁**;例外專用的 stack 與 i386 的 double fault 處理全系統仍只有一份(第 16、17 步列過)。
- **i386 的 PAE 項目分兩次寫入**(第 15.5 步列過)仍未處理。目前所有對映的實體位址都在 4GB 以下,高半部只有 NX 位元,最壞的情況是別顆 CPU 在極短的時間內看到一個少了 NX 的項目。
- **沒有 CPU 親和性(affinity)的設定**,也不能把 CPU 下線。
- 第 10~15 步的排程示範仍只在單一 CPU 上驗證(其他 CPU 加入之前)。優先權、搶佔、優先權繼承在多核心下的行為,目前只有「程式是同一份」這個保證,沒有專門的測試。

---

## 第 19 步:檔案系統 — VFS、記憶體檔案系統、開機檔案包

到上一步為止,kernel 裡沒有「檔案」這個概念:唯一的使用者程式是直接嵌在 kernel 映像裡的一塊資料,輸出是一個專用的系統呼叫。這一步加入檔案系統,並把這兩件事都改成經由檔案:

- 使用者程式從 `/bin/init` 這個**檔案**載入
- 輸出是「寫入 1 號檔案描述符」,而它指向 `/dev/console` 這個**裝置檔案**

目前還沒有磁碟驅動,所以檔案存在記憶體裡。重點是先把介面定下來,之後加入磁碟上的檔案系統時,上層不必改。

### 三層結構

```
系統呼叫(proc.c)      檔案描述符、把使用者的緩衝區與路徑搬進搬出、檢查位址
      │
VFS(vfs.c)            路徑解析、開啟中的檔案、參考計數、上鎖
      │  struct inode_ops(九個函式)
      ▼
實際的檔案系統         目前只有 ramfs(ramfs.c)
```

每一層只認得下一層的介面。VFS(Virtual File System)之所以稱為「虛擬」,是因為它不知道資料實際存在哪裡;它只呼叫 `inode_ops` 裡的函式(查找名稱、建立、移除、讀、寫…)。之後要加入磁碟上的檔案系統,就是再提供一組 `inode_ops`。

兩個核心物件(`include/fs.h`):

| 物件 | 代表 | 說明 |
|---|---|---|
| inode | 一個檔案或目錄**本身** | 內容、大小、類型。和名稱無關:名稱只是目錄裡指向 inode 的一筆紀錄 |
| file | **一次開啟** | 指向一個 inode,加上讀寫位置與開啟方式。同一個 inode 可以同時被開啟很多次,各有各的位置 |

### 記憶體檔案系統(ramfs)

- **目錄**:一條串列,每筆紀錄是「名稱 → 節點」。
- **檔案**:一張表,每一格指向一個 4KB 的頁;第 N 個位元組在第 `N / 4096` 格所指的頁裡。寫到哪裡才配置到哪裡。表不夠大時換一張兩倍大的(複製的只是指標,不是資料)。單一檔案上限 16MB。
- 新配置的頁一定清 0,不讓人讀到這頁之前的內容。

ramfs 自己不上鎖:VFS 呼叫它之前已經鎖住那個 inode。這讓檔案系統的實作很單純,也是把鎖放在 VFS 這一層的原因 — 每個檔案系統不必各自重做一次。

### 路徑解析

`walk()` 從根目錄出發,一段一段往下找:鎖住目前的目錄、查出下一層、放開。每次只鎖一個目錄,所以兩條執行緒解析不同的路徑不會互相擋住,也不會有鎖的順序問題。

建立與刪除操作的對象其實是**上層目錄**(在裡面新增或移除一筆紀錄),所以 `walk()` 有另一個模式:停在最後一段之前,回傳上層目錄與最後一段的名稱。

目前所有路徑都從根目錄開始,行程還沒有「目前的工作目錄」。

### 生命週期:刪除時還有人開著怎麼辦

每個 inode 有兩個計數:

- `nlink`:有幾個名稱指向它
- `refs`:有幾個使用中的參考(被開啟,或路徑解析正走到它)

**兩者都是 0 才釋放。** 所以刪除一個還開著的檔案是允許的:名稱立刻消失(別人再也打不開),內容留到最後一個開啟者關閉為止。這是 Unix 的傳統行為,也是最不容易出錯的選擇 — 另一種做法(刪除時強制讓開啟者失效)需要讓每一次讀寫都檢查「這個檔案還在嗎」。

兩個計數由同一把 spinlock 保護,只包住加減的那幾條指令。

### 上鎖

| 鎖 | 種類 | 保護 |
|---|---|---|
| `inode->lock` | mutex | 一個 inode 的內容(檔案的資料、目錄的紀錄) |
| `file->lock` | mutex | 一次開啟的讀寫位置(同一行程的多條執行緒可能共用) |
| `inode_lk` | spinlock | 所有 inode 的 `refs` / `nlink` 與 file 的參考計數 |
| `fd_lk` | spinlock | 所有行程的檔案描述符表 |

內容用 mutex 而不是 spinlock:讀寫可能很久,等待的人應該讓出 CPU。需要同時鎖兩個 inode 的地方只有刪除(要檢查被刪的目錄是不是空的),順序固定為「先上層、後下層」,目錄是樹狀的,不會形成循環。

`O_APPEND`(附加)的位置是在**鎖住 inode 之後**才決定的,所以兩條執行緒同時附加不會互相覆蓋。測試裡 4 條執行緒往同一個檔案附加固定長度的紀錄,就是在驗證這一點。

### 開機檔案包

檔案系統剛建立時是空的,而 kernel 沒有磁碟驅動,第一批檔案從哪裡來?做法和 Linux 的 initramfs 相同:

```
建置時:  rootfs/ 目錄 + 編譯好的使用者程式  ──tar──▶  rootfs.tar  ──嵌入──▶  kernel 映像
開機時:  kernel/initrd.c 把它解開到 ramfs
```

格式選 tar(ustar):主機上的 `tar` 指令就能產生,不必自己寫打包工具;結構也很單純,一連串的「512 位元組的標頭 + 內容」。解析時每個長度都檢查是否在範圍內。

打包時固定了排序、時間與擁有者,所以同樣的內容每次都產生完全相同的檔案(可重現的建置)。

開機後的檔案樹:

```
/dev/console   主控台(kernel 開機時建立的裝置節點)
/bin/init      使用者程式
/etc/motd
/README.txt
```

### 裝置檔案:把主控台變成一個檔案

`/dev/console` 是類型為「裝置」的 inode:對它讀寫不經過檔案系統,而是直接呼叫驅動程式的函式(目前寫入就是輸出到主控台;讀取因為還沒有輸入驅動,一律回報沒有資料)。

每個行程建立時,0、1、2 號檔案描述符(標準輸入、輸出、錯誤)都指向它。使用者程式的輸出因此只是 `write(1, ...)`。好處是之後要把輸出導向檔案或管線時,只要換掉描述符指向的東西,程式本身不必修改。

系統呼叫的介面也跟著改:`write` 從 `(buf, len)` 變成 `(fd, buf, len)`。

### 檔案描述符

每個行程有一張 16 格的表,「編號 → 開啟中的檔案」。同一行程的執行緒共用它。

一個只在多執行緒下才會出現的問題:執行緒 A 正要用 3 號描述符讀取,執行緒 B 同時把 3 號關閉。如果 A 查到指標之後、開始使用之前,B 就把檔案釋放了,A 用的就是已釋放的記憶體。所以**查表與「多拿一個參考」必須在同一把鎖之下完成**;A 用完再歸還參考。我第一版把這兩步分開寫,檢查時發現並改正(當時還沒有測試會撞到它)。

行程結束時,它還開著的檔案由 kernel 代為關閉。

### 新增的系統呼叫與安全檢查

`open`、`close`、`read`、`readdir`、`mkdir`、`unlink`、`stat`,加上改過的 `write`。

這些呼叫的參數全部來自不可信任的使用者程式,新增了兩種檢查:

- **寫入使用者的緩衝區**(`read`、`readdir`、`stat` 的結果):除了必須是使用者的頁,還必須是**可寫**的頁。否則使用者可以請 kernel 代為寫入它自己不能寫的地方,例如它的程式碼。
- **路徑字串**:逐位元組從使用者位址複製,長度有上限,任何一個位元組的位址不合法就拒絕。

不認得的開啟旗標會被拒絕,而不是忽略。

**程式檔的標頭也是不可信任的輸入。** 之前程式是 kernel 的一部分,現在它是檔案系統裡的檔案,任何人都可能寫一個進去。載入時每個欄位都檢查(各區段的範圍、進入點必須在程式碼內、總大小上限);不合格就建立失敗,而不是載入到一半出事。

### 驗證

**kernel 端**(`fs_demo`,直接呼叫 VFS):

| 項目 | 內容 |
|---|---|
| 開機檔案包 | 列出整棵樹;讀出 `/etc/motd` |
| 讀寫 | 寫入 10000 位元組(跨三頁)再讀回,逐位元組比對;佔用的頁數正好是 3 |
| 附加 | 兩次開啟各寫一段,內容依序相接 |
| 開著時被刪除 | 名稱立刻消失、內容仍可讀;關閉後頁數才減少 |
| 清空 | `O_TRUNC` 之後大小為 0、頁全部歸還 |
| 錯誤 | 不存在、路徑中間不是目錄、以寫入方式開啟目錄、刪除非空目錄、刪除根目錄、名稱太長;`.` 與 `..` |
| 並行 | 4 條執行緒(在多顆 CPU 上)同時在同一個目錄各自建立、寫入、讀回、刪除 25 個檔案,並往同一個紀錄檔附加;最後紀錄檔的總長度、每一筆的完整性、每條執行緒的筆數都正確 |
| 記憶體 | 全部刪除後,檔案佔用的頁數與 heap 的空閒量回到測試前 |

**使用者端**(`UMODE_FS`,經由系統呼叫):讀取 `/etc/motd`、建立目錄與檔案、寫入 4500 位元組再讀回比對、附加、列出目錄、刪除;以及應該被拒絕的情況:

| 嘗試 | 結果 |
|---|---|
| 寫入唯讀開啟的檔案、使用已關閉的描述符 | 被拒絕 |
| 路徑指標指向 kernel 的記憶體 | 被拒絕 |
| 請 kernel 把讀到的資料寫進 kernel 的記憶體 | 被拒絕 |
| 請 kernel 把讀到的資料寫進自己的程式碼(唯讀頁) | 被拒絕 |
| 開啟不存在的檔案、刪除非空的目錄 | 回報對應的錯誤 |

另外確認:以不存在的路徑、或內容不是程式的檔案(`/etc/motd`)建立行程,都會失敗。

**回歸**:三個架構各 20 個情境共 60 個,結束碼都符合預期。`autoexit selftest stress memtest` 在 1、4、8 顆 CPU 下各 10 次(三個架構共 90 次)全部通過;加上開發過程中另外的 192 次也全部通過。i386 舊 CPU 型號與主機端的執行佇列測試通過。

三個架構的輸出相同:

```
fs: root file system ready: 3 files unpacked from a 20 KB boot archive
fs: tree: /dev/ /dev/console /README.txt /bin/ /bin/init /etc/ /etc/motd
fs: /etc/motd: "Welcome to WElyOS."
fs: 4 threads x 25 files (create, write, read back, delete) plus a shared append-only log: all intact, 9 ticks
fs: selftest ok (all files removed, memory returned)
  [user] /etc/motd says: Welcome to WElyOS.
  [user] /tmp contains: data log
  [user] file system calls work as expected
```

### 效能:行程建立變慢了,但原因不是檔案系統

加入檔案系統後,壓力測試的「300 個循序行程」明顯變慢(單核心、i386:77 → 約 130 tick)。直覺會怪「現在要從檔案讀程式」,實際量測的結果不是:

| 建立一個行程的各階段(i386,相對值) | |
|---|---|
| 建立位址空間 | 32 |
| **載入程式** | **589** |
| 開啟主控台(三個描述符) | 31 |
| 建立執行緒(含對映 4 頁 stack) | 83 |

再把「載入程式」拆開:開檔 40、讀標頭 15、從檔案讀內容 52、**配置並對映 3 頁 476**。檔案系統的部分只佔一成多。而對映的 476 裡,**把新的頁清為 0** 就佔了 454 — 同樣是清一頁,stack 的頁每頁約 7,程式的頁每頁約 150。

原因在模擬器:QEMU 會記住哪些實體頁「放過被執行的程式碼」(它把那些程式碼翻譯成主機的指令並快取起來)。之後只要有人寫入這種頁,它就要走很慢的路徑去檢查、作廢快取。上一個行程結束後,它的程式碼頁被還回去,下一個行程又拿到同一頁並把它清 0,每個位元組的寫入都踩到這條慢路徑。

這一步讓使用者程式從 1 頁程式碼變成 2 頁(加入了檔案測試的程式碼),所以這個成本加倍。**實機上沒有這個現象。**

過程中做了一個相關的修正:檔案測試原本用兩個 9000 位元組的全域陣列,這會讓**每一個**行程載入時都多配置 5 頁,即使它根本不跑這個測試。改成放在 stack 上。

真正的解法是讓執行同一個程式的行程**共用**程式碼的頁,而不是各自複製一份:直接把檔案的頁以唯讀對映進行程。這樣建立行程時程式碼的部分不必配置、清 0、複製,也省記憶體。它需要「正在執行的程式檔不可以被修改」等配套,留到之後做 `exec` 時一起處理。

其他耗時(4 顆 CPU):只開機 0.22~0.33 秒,`selftest` 0.80~1.04 秒(測試項目增加)。

### 已知限制

- **內容只存在記憶體裡**,關機就消失。沒有磁碟驅動,也沒有磁碟上的檔案系統。 **〔後續:已解決:第 27 步加入磁碟驅動與磁碟上的檔案系統(掛載在 `/disk`);根目錄仍在記憶體裡。〕**
- **沒有 `seek`**:只能從頭依序讀寫或附加。也沒有 `rename`、硬連結、符號連結。 **〔後續:第 29 步加入 `lseek`、`rename`、硬連結;符號連結仍然沒有。〕**
- **沒有「目前的工作目錄」**,所有路徑都從根目錄算起。 **〔後續:已解決:第 29 步。〕**
- **沒有擁有者與權限**:任何行程都可以讀寫、刪除任何檔案,包括 `/bin/init`。 **〔後續:已解決:第 34 步。〕**
- **正在執行的程式檔可以被修改或刪除**。目前載入時是整份複製,所以不影響已經在執行的行程;改成共用頁之後就必須禁止。 **〔後續:已解決:第 20 步改為共用頁,並禁止修改正在執行的程式檔(刪除仍允許:名稱消失,內容留到最後一個使用者結束)。〕**
- **主控台只能輸出**,讀取一律回報沒有資料(還沒有輸入驅動)。 **〔後續:已解決:第 21 步加入主控台輸入。〕**
- **每個行程最多 16 個檔案描述符**;不能複製描述符(`dup`),也還沒有管線。 **〔後續:第 21 步加入 `dup2`;管線仍未實作。〕**
- **目錄是線性搜尋的串列**,同一個目錄裡檔案很多時會慢;列出目錄時每取一筆都從頭數起。 **〔後續:第 27 步把列出目錄的介面改成由檔案系統自己記位置;磁碟上的目錄不必從頭數,ramfs 的串列仍然要。〕**
- **讀寫經過兩次複製**(使用者 ↔ kernel 的暫存區 ↔ 檔案的頁),一次 256 位元組。
- **只有一個檔案系統**,沒有掛載(mount)的機制;`/dev` 裡的裝置節點也是 ramfs 的節點。 **〔後續:第 27 步加入掛載(磁碟固定掛在 `/disk`);裝置節點仍然只能在 ramfs 裡。〕**
- **tar 只處理一般檔案與目錄**,其他類型略過;開機檔案包解開後,嵌在 kernel 裡的那一份仍佔著記憶體(等於每個初始檔案存了兩份)。
- **行程建立時每次都重新載入整個程式**,如上一節所述。 **〔後續:已解決:第 20 步讓程式碼頁直接與檔案共用。〕**

---

## 第 20 步:exec、從使用者程式建立行程、參數,以及共用程式碼頁

上一步有了檔案系統,但使用者程式仍只能由 kernel 啟動、只有一個程式、也收不到參數。這一步補上讓「程式可以啟動程式」所需要的東西,這是之後做 shell 的前提:

- **多個程式**:`/bin/init`(測試)、`/bin/echo`、`/bin/cat`、`/bin/ls`
- **參數**:程式的進入點變成一般的 `main(argc, argv)`
- **`spawn`**:建立一個新行程執行某個程式
- **`wait`**:等待行程結束並取得結束碼
- **`exec`**:目前的行程改為執行另一個程式
- **共用程式碼頁**:解決上一步量到的行程建立變慢

### 決策:為什麼是 spawn,而不是先做 fork

Unix 傳統上建立行程是兩步:`fork`(複製自己)再 `exec`(換成別的程式)。這裡先提供一步到位的 `spawn`:

| | fork + exec | spawn |
|---|---|---|
| 成本 | fork 要複製整個位址空間(或實作寫入時複製),而複製出來的內容幾乎立刻被 exec 丟掉 | 直接建立新的位址空間 |
| 需要的機制 | 寫入時複製(copy-on-write)、複製所有頁表 | 沿用建立行程的既有流程 |
| 彈性 | 子行程可以在 exec 之前任意調整自己的狀態 | 只能靠「繼承」與參數 |

`spawn` 少了的彈性,用**繼承檔案描述符**補回最常用的部分(見下面的重新導向)。`fork` 需要的寫入時複製留到之後。

### 參數怎麼傳進新程式

新行程還沒開始執行,它的記憶體是另一個位址空間,kernel 要把字串放進去。做法是放在它 stack 的最頂端:

```
高位址   字串本身("echo\0hello\0…")
         NULL
         argv[argc-1] 的位址
         …
         argv[0] 的位址
argp ->  argc
低位址   (stack 從這裡往下使用)
```

kernel 把 `argp` 當作進入點的唯一參數。使用者程式這一側由 `user/crt0.c`(每個程式都會連結的啟動程式)把它轉成 `main(argc, argv)`,`main` 返回後以回傳值結束行程。

寫入的對象是「還沒載入的那個位址空間」,所以不能直接用使用者位址,而是逐頁查出實體位址再寫(`space_write`)。字串的位址要填「在新行程裡」的位址。

從使用者傳來的參數陣列是兩層不可信任的資料:陣列本身是指標,每個指標又指向字串。每一層都檢查位址,數量(16 個)與總長度(1024 位元組)有上限。

### 「先完整建好,成功了才使用」

`spawn` 與 `exec` 都要做同一件事:建立位址空間、載入程式、準備 stack 與參數。這段抽成 `image_build()`,它只有兩種結果:

- 成功:得到一個隨時可以開始執行的「程式映像」
- 失敗:什麼都不留下

這對 `exec` 特別重要。`exec` 成功就不會返回(原本的程式已經不存在了),所以**任何可能失敗的事都必須在動到舊程式之前做完**:檔案不存在、不是合法的程式、記憶體不足、參數的位址不合法 — 這些情況下 `exec` 只是回傳錯誤,原本的程式照常繼續執行。確定成功之後才換到新的位址空間並釋放舊的。

`exec` 之後行程編號與開啟中的檔案不變,記憶體內容全部換新。

**限制:行程還有其他執行緒時,`exec` 會被拒絕。** 那些執行緒正在舊的位址空間裡執行,換掉之前得先讓它們全部結束,目前不做這件事。

### 繼承檔案描述符:重新導向的基礎

由另一個行程建立的新行程,會**繼承建立者所有開啟中的檔案**(同一次開啟,共用讀寫位置)。

這讓「把輸出導向檔案」不需要任何特別的支援:

```c
close(1);                                  // 關掉標準輸出
open("/out.txt", O_CREAT | O_WRONLY);      // 新開的檔案得到最小的空位,也就是 1
pid = spawn("/bin/echo", argv);            // echo 繼承了這個 1 號描述符
wait(pid);
```

`echo` 完全不知道這件事,它照常寫入 1 號描述符。這正是上一步把主控台做成檔案描述符的目的。測試裡實際這樣做了一次,並把檔案讀回來確認內容。

### 共用程式碼頁

上一步量到:建立行程的時間大部分花在「把新的頁清為 0」,而且模擬器對曾經放過程式碼的頁特別慢。真正的問題是**每個行程都複製一份完全相同、而且永遠不會被修改的程式碼**。

現在程式碼頁不複製了。ramfs 的檔案內容本來就以 4KB 的頁存放,所以直接把「檔案的那一頁」以唯讀、可執行的方式對映進行程。所有執行同一個程式的行程共用同一份實體頁。

需要三個配套:

**1. 位址空間釋放時不能把共用的頁一起釋放。** 那一頁屬於檔案。頁表項目裡有幾個硬體會忽略、保留給作業系統使用的位元(RISC-V 的 RSW、AArch64 的 bit 55~58、x86 的 bit 9~11),用其中一個標記「這一頁不歸這個位址空間所有」(`VM_SHARED`)。釋放位址空間時跳過有標記的頁。

**2. 正在執行的程式檔不可以被修改。** 它的頁直接對映在行程裡,改了檔案就等於改了執行中的程式。每個 inode 多一個計數「有多少行程正在執行它」,大於 0 時寫入與清空會得到 `E_TXTBSY`(Unix 的 "text file busy")。行程結束或 exec 換掉它之後才遞減。

**3. 檔案被刪除時頁不能消失。** 這一點不需要新的機制:行程一直開著它的程式檔,而上一步已經保證「刪除時還有人開著,內容留到最後一個人關閉」。

資料區段仍然每個行程各自一份(它會被修改)。只有「整頁都在程式碼範圍內」的頁才共用;程式檔的格式本來就把程式碼區段對齊到頁。

### 順帶發現的錯誤:行程結束時,還沒開始跑的執行緒不會被終止

加入「還有其他執行緒時 exec 應被拒絕」的測試後,單核心下整個測試卡住(4 顆 CPU 正常)。原因和 exec 無關,是行程結束流程裡一個既有的漏洞:

1. 主執行緒建立了第二條執行緒,但它還沒輪到執行。
2. 主執行緒結束行程。結束流程會把「屬於這個行程」的執行緒都標記為應終止 — 但第二條執行緒要到自己開始執行時才登記歸屬,此刻還不算,沒被標記到。
3. 第二條執行緒之後開始執行,檢查「我被標記了嗎」,沒有,於是進入使用者模式一直跑下去。行程永遠結束不了。

多核心下第二條執行緒通常立刻在別顆 CPU 上開始執行,所以碰不到;單核心下新執行緒不會馬上執行,必定發生。這也是之前沒被發現的原因:既有的測試都會等自己建立的執行緒做完。

修正:新執行緒開始時,除了看自己有沒有被標記,也看行程本身是不是已經開始結束了。

這再次說明 1 顆與多顆 CPU 都要測:它們會暴露不同的時序。

### 驗證

| 測試 | 內容 | 結果 |
|---|---|---|
| kernel 啟動 `/bin/echo` 並帶參數 | 結束碼 = 參數個數 | 3 |
| `spawn` + `wait` | 使用者程式建立 `/bin/echo`、`/bin/cat /etc/motd`、`/bin/ls /bin` 並等待 | 輸出與結束碼正確 |
| 重新導向 | 子行程的輸出寫進檔案,再讀回比對 | 內容正確 |
| `exec` | 行程把自己換成 `/bin/echo` | 輸出來自 echo,行程編號不變,結束碼是 echo 的 |
| `exec` 失敗 | 檔案不存在、不是程式 | 回傳錯誤,原程式繼續執行 |
| 有其他執行緒時 `exec` | | 被拒絕(`E_BUSY`) |
| 不合法的指標 | 路徑、參數陣列、其中一個參數指向 kernel 的記憶體 | 全部被拒絕(`E_FAULT`) |
| 修改正在執行的程式檔 | 寫入、清空 `/bin/init` | 被拒絕(`E_TXTBSY`) |
| 等待自己 | | 被拒絕 |

輸出(三個架構相同):

```
  [user] hello from a spawned /bin/echo: one two
  [user] spawned pid 5 exited with 4 (my pid is 4)
  [user] running: cat /etc/motd
Welcome to WElyOS.
  [user] running: ls /bin
cat  echo  init  ls
  [user] output of a child process was redirected into a file and read back
  [user] process calls work as expected
  [user] this line is printed by /bin/echo after exec, same pid
```

回歸:三個架構各 20 個情境共 60 個,結束碼都符合預期。`autoexit selftest stress memtest` 在 1、2、4、8 顆 CPU 下各 8 次(三個架構共 96 次)全部通過;i386 舊 CPU 型號通過。既有的記憶體核對(行程結束後頁數與 heap 回到原樣)在共用頁之下仍然成立。

### 效能

建立並等待 300 個循序行程(tick):

| | 第 18 步(嵌在 kernel) | 第 19 步(從檔案複製) | 本步(共用程式碼頁) |
|---|---|---|---|
| i386,1 顆 CPU | 77 | 約 130 | **59** |
| riscv64,1 顆 CPU | 107 | 約 146 | **95** |
| aarch64,1 顆 CPU | 81 | 約 114 | **78** |
| i386,4 顆 CPU | 89 | 143 | **75** |
| riscv64,4 顆 CPU | 149 | 191 | **128** |
| aarch64,4 顆 CPU | 208 | 280 | **194** |

不只追回上一步變慢的部分,還比沒有檔案系統時更快:現在程式碼完全不必配置、清 0、複製。使用者程式比第 18 步多了一倍的程式碼(加入了檔案與行程的測試),時間反而減少。

耗時(4 顆 CPU):只開機 0.21~0.35 秒,`selftest` 0.83~1.10 秒(測試項目又增加了)。

### 已知限制

- **沒有 `fork`**。要「複製自己」目前做不到;建立行程只能用 `spawn`。 **〔後續:已解決:第 22 步。〕**
- **`exec` 只允許單一執行緒的行程使用**。
- **沒有父子關係**:任何行程都可以 `wait` 任何行程編號;行程結束後若沒有人 `wait`,它的紀錄會一直留著。也沒有「等待任何一個子行程」。 **〔後續:已解決:第 24 步加入父子關係,只能等自己的子行程;父行程結束後,子行程的紀錄由 kernel 自動回收;也可以等「任何一個子行程」。〕**
- **結束碼與錯誤碼共用同一個回傳值**:`wait` 回傳負值時,無法分辨是「呼叫失敗」還是「那個行程以負值結束」(被終止的行程結束碼是 -1)。 **〔後續:已解決:第 24 步新增 `waitpid`,回傳值是行程編號,結束碼另外寫到呼叫者指定的位置。〕**
- **`spawn` 繼承全部的檔案描述符**,不能指定哪些不要繼承。也還不能複製描述符(`dup`),所以重新導向要靠「關閉再開啟會拿到最小空位」這個性質。 **〔後續:第 21 步加入 `dup2`,重新導向不必再靠「最小空位」。〕**
- **參數最多 16 個、總長 1024 位元組**;沒有環境變數。
- **只共用程式碼頁**。資料區段每個行程各自複製;沒有寫入時複製,也沒有需求分頁(用到才載入)。 **〔後續:第 22 步加入寫入時複製(用於 fork)與 stack 的用到才配置;從檔案載入程式時,資料區段仍是立即複製。〕**
- **只有 ramfs 能提供可共用的頁**。之後的磁碟檔案系統需要頁快取(page cache)才能做到同樣的事;不提供時會自動退回複製。
- **程式檔的格式是自訂的簡單標頭**,不是 ELF。
- **主控台仍然不能輸入**,所以還沒有互動式的 shell。 **〔後續:已解決:第 21 步。〕**

---

## 第 21 步:主控台輸入與 shell

到上一步為止,系統只能輸出。這一步讓它能接受輸入,並提供一個 shell:開機後出現提示符號,可以打命令執行程式。

```
starting /bin/sh (type "help" for a list of commands)
$ ls /
dev/  README.txt  bin/  etc/
$ echo hello > /note
$ cat /note
hello
$ exit
```

### 字元怎麼從硬體進來:輪詢

每個架構提供一個函式 `arch_console_getc()`:有收到字元就回傳,沒有就立刻回傳 -1,不等待。

| 架構 | 來源 |
|---|---|
| i386 | COM1 序列埠(16550)的線路狀態暫存器 bit 0 |
| RISC-V | 同樣是 16550,只是暫存器對映在記憶體 |
| AArch64 | PL011 的旗標暫存器 bit 4(接收緩衝區是否為空) |

**決策:用輪詢,不用中斷。** 計時器每個 tick(1ms)由第 0 顆 CPU 去問一次硬體。

| | 輪詢(本步) | 中斷驅動 |
|---|---|---|
| 實作 | 每個架構一個小函式 | 三個架構各要設定一套中斷控制器(RISC-V 的 PLIC、AArch64 的 GIC 共用中斷、i386 的 PIC IRQ 4) |
| 延遲 | 最多 1ms | 幾乎立即 |
| 沒有輸入時的成本 | 每個 tick 多讀一次暫存器 | 無 |
| 大量輸入 | 可能來不及取走(見下) | 較不容易 |

**〔後續:第 25 步改成中斷驅動,三個架構的中斷控制器設定見該步。〕** 對人打字來說 1ms 感覺不出來。介面刻意分成兩層:`arch_console_getc()` 只管取字元,「取到之後怎麼處理」在 `kernel/console.c`。之後要改成中斷驅動,只要改「由誰呼叫」。

i386 目前只接序列埠;QEMU 圖形視窗裡的 PS/2 鍵盤還不能用。

### 行規程(line discipline)

使用者打字會打錯、會按退格鍵。程式通常不想處理這些,只想拿到「一整行」。所以 kernel 在硬體和程式之間多一層:收集字元、處理編輯、把打的字顯示出來(回顯),等按下 Enter 才把整行交給正在讀取的程式。Unix 稱這種模式為 canonical mode。

```
硬體 ──字元──▶ console_input() ──編輯中的一行──▶ 按 Enter ──▶ 已完成的資料 ──▶ read()
```

| 按鍵 | 動作 |
|---|---|
| Enter | 這一行完成,交給讀取者 |
| Backspace / DEL | 刪掉前一個字元(畫面上以「退一格、印空白、再退一格」蓋掉) |
| Ctrl-C | 放棄目前這一行 |
| Ctrl-D | 在行首:表示輸入結束,讀取者得到 0(和讀到檔案結尾一樣) |

`read()` 在沒有完成的行時會睡著,不佔 CPU,有人按 Enter 才被叫醒。一次最多交出一行。

**睡在讀取裡的行程要能被終止。** 第 15 步留下的限制是「被終止的執行緒若正睡在 kernel 裡,要等它醒來才會結束」— 等鍵盤輸入的行程可能永遠不會醒。所以終止行程時會叫醒所有等輸入的執行緒,讀取的迴圈也會檢查自己是否已被終止。測試裡啟動一個沒有人打字的 shell,10ms 後終止它,確認它真的結束。

### shell(`user/sh.c`)

就是一個迴圈:讀一行 → 切成一個個字 → 第一個字是程式名稱,其餘是參數 → `spawn` → `wait` → 再讀下一行。大約 200 行,完全在使用者模式執行,用到的都是前兩步建立的系統呼叫。

| 功能 | 說明 |
|---|---|
| `命令 參數...` | 名稱不含 `/` 時到 `/bin` 底下找 |
| `命令 > 檔案`、`命令 >> 檔案` | 輸出寫入檔案(覆蓋 / 附加) |
| `"含 空白 的 參數"` | 雙引號內的空白不拆開 |
| `# 註解` | |
| `exit [數字]`、`status`、`help` | 內建命令,由 shell 自己處理 |
| `sh 檔案` | 依序執行檔案裡的命令(script),不印提示符號 |

找不到程式時結束碼是 127(Unix 的慣例)。另外加了兩個小工具:`mkdir`、`rm`。

**重新導向的做法**:新行程會繼承建立者開啟的檔案,所以 shell 在建立它之前,把**自己的**標準輸出暫時換成那個檔案,建立完再換回來。被執行的程式完全不知情。上一步用的是「關閉再開啟會拿到最小空位」的技巧,這樣換不回原本的輸出;這一步新增 `dup2(舊, 新)` 系統呼叫(讓「新」也指向「舊」的那次開啟),先把原本的輸出存到另一個編號,用完再接回來。

**開機流程的改變**:沒有 `autoexit` 參數時,kernel 做完初始化就啟動 `/bin/sh`(之前是進入閒置)。有 `autoexit` 時只有另外給了 `shell` 參數才啟動,shell 結束後以它的結束碼結束模擬器。

### 踩坑一:貼上的文字會掉字

第一次測試用管線一次送進整段命令,shell 只收到每一行的第一個字元。

原因有兩層:

1. Windows 版 QEMU 的標準輸入在客體還沒準備好接收時,**直接把字元丟掉**(Linux 版會等)。
2. RISC-V 與 AArch64 的序列埠沒有啟用 FIFO,硬體一次只能暫存 1 個字元,而 kernel 每 1ms 才來取一次。

處理:兩個架構都啟用接收 FIFO(可暫存 16 個字元;i386 原本就有)。自動化測試改用一個小腳本(`scripts/type-input.py`)模擬打字,一個字元一個字元送。

這只是減輕,不是解決:一次貼上超過 16 個字元仍可能掉字。要根治得改成中斷驅動,讓 kernel 在字元到達時立刻取走。列在已知限制。 **〔後續:第 25 步改成中斷驅動並做了量測。結論是大幅改善,但「一次整段送進去」在 Windows 版 QEMU 上仍會掉字,那是模擬器的行為,客體無法根治。〕**

### 踩坑二:排程公平性測試在 i386 上突然失敗

加入這一步的程式後,第 10 步的搶佔示範在 i386、4 顆 CPU 下每次都失敗:兩條一樣的忙碌執行緒,一條跑了 94 萬圈,另一條只有 19 萬圈,超過「差距 3 倍以內」的檢查。

這個測試和主控台、shell 完全無關。把這一步的每個改動逐一關掉都沒有用;回到上一個提交重新建置則一切正常。

查的過程:

1. 兩條執行緒的程式碼完全相同,只有兩樣東西不同:計數器的位址與 stack 的位址。
2. 同時建立 8 條探測執行緒,印出各自的 stack 位址與圈數:**只有 stack 落在某一個特定實體頁的那條慢了 4 倍**,其他都正常。
3. 那一頁本身沒有任何特別之處。

原因在模擬器轉換位址的快取:QEMU 用一張「直接對映」的表記住最近用過的頁,表的位置由頁號的低幾個位元決定。這一步讓 kernel 變大,記憶體配置跟著位移,結果某條執行緒的 stack 頁(`0x171000`)和它反覆讀寫的資料頁(`0x131000`)剛好落在表的**同一格**(兩個頁號只差在第 6 個位元)。迴圈每一圈都輪流存取這兩頁,每次都把對方擠掉,每次都要重新查。

兩條執行緒得到的 CPU 時間其實是一樣的:

```
i386:  spin1: 19 ms of cpu (740739 loops), spin2: 18 ms (140949 loops)
```

所以測試改為比較**實際得到的 CPU 時間**(執行期間系統時間前進了幾次),而不是迴圈圈數,門檻也從 3 倍收緊為 2 倍。圈數仍然印出來,但不拿來判斷。

這個發現也回頭影響第 18 步的結論:當時用「完成的迴圈次數」比較兩種執行佇列,得到環狀版多 30%~36%。既然圈數會因記憶體配置的巧合差到 5 倍,那個數字就不能採信了(已在第 18 步加註)。當時另一個指標「換過 CPU 的執行緒數」是直接計數,不受影響。

教訓:**在模擬器上,「同樣的時間做了多少事」不是可靠的量測**;可靠的是直接數得出來的東西(tick、切換次數、移動次數)。

### 驗證

| 測試 | 內容 | 結果 |
|---|---|---|
| script | `sh /etc/test.sh`:建立目錄、兩次重新導向(覆蓋與附加)、引號內的連續空白、找不到的命令、打不開的檔案、刪除非空目錄、`exit 7` | 結束碼 7;留下的檔案內容逐位元組正確 |
| 終止等輸入的行程 | 啟動互動式 shell(沒有輸入),10ms 後終止 | 以指定的結束碼結束 |
| 互動輸入 | 模擬打字:執行命令、重新導向、找不到的命令與 `status`、退格修正、`exit 5` | 三個架構輸出相同,結束碼 5 |
| 互動輸入之二 | `help`、打到一半按 Ctrl-C、重新導向、最後按 Ctrl-D 結束 | 三個架構 × 1 與 4 顆 CPU,共 12 次全部正確 |

回歸:三個架構各 20 個情境共 60 個,結束碼都符合預期。`autoexit selftest stress memtest` 在 1、4、8 顆 CPU 下各 8 次(三個架構共 72 次)全部通過。

耗時(4 顆 CPU):只開機 0.21~0.35 秒,`selftest` 0.82~1.10 秒。

### 關於 fork

到這一步,建立行程都用 `spawn`,shell 也不需要 `fork`。`fork` 的語意是「複製目前的行程」,有效率的做法必須靠寫入時複製(copy-on-write):先共用所有的頁並標為唯讀,有人寫入時才真的複製那一頁。這需要「處理 page fault 之後繼續執行」的能力,目前 page fault 一律視為錯誤。所以 `fork` 會和寫入時複製、需求分頁放在同一步一起做,那一步完成後「資料區段每個行程各自複製」的成本也會一併消失。

### 已知限制

- **輸入是輪詢的**,最多 1ms 的延遲;一次送進超過 16 個字元(例如貼上一長串)可能掉字。 **〔後續:第 25 步改成中斷驅動,每個字元間隔 2ms 以上不再掉字;一次整段貼上在 Windows 版 QEMU 上仍會掉字,原因在模擬器。〕**
- **i386 只接受序列埠的輸入**,QEMU 圖形視窗裡的鍵盤不能用。
- **只有一個主控台、一份輸入狀態**。多個行程同時讀取時,每一行只會交給其中一個,沒有「前景行程」的概念。
- **Ctrl-C 只是放棄正在打的那一行**,不會終止正在執行的程式(沒有訊號機制);放棄之後 shell 也不會重印提示符號。 **〔後續:已解決:第 24 步起 Ctrl-C 會終止前景的命令(含整條管線)。〕**
- **退格以位元組為單位**:中文等多位元組的字元刪除時畫面會亂掉。也沒有游標左右移動、歷史紀錄、Tab 補完。
- **沒有「原始模式」**:程式不能一次讀一個按鍵(編輯器需要)。
- **shell 很陽春**:沒有管線(`|`)、輸入重新導向(`<`)、背景執行(`&`)、變數、條件與迴圈、萬用字元;`/bin` 是唯一的搜尋路徑。 **〔後續:第 23 步加入管線與輸入重新導向;第 25 步加入背景執行。〕**
- **script 的執行結果只看最後的結束碼**,中間的命令失敗不會中止。
- **沒有 `fork`**,如上所述。 **〔後續:已解決:第 22 步。〕**

---

## 調整:三個平台相同的 WElyOS 開機標誌

目標:三個架構開機時都出現同一個 WElyOS 文字標誌。

```
 __          __  ______   _            ____     _____
 \ \        / / |  ____| | |          / __ \   / ____|
  \ \  /\  / /  | |__    | |  _   _  | |  | | | (___
   \ \/  \/ /   |  __|   | | | | | | | |  | |  \___ \
    \  /\  /    | |____  | | | |_| | | |__| |  ____) |
     \/  \/     |______| |_|  \__, |  \____/  |_____/
                               __/ |
                              |___/

Hello WElyOS
arch: riscv64 (64-bit)
```

### 做法:由 kernel 印,不交給韌體

標誌是 `kernel/main.c` 裡的一個字串(`boot_logo`),`kmain` 一開始就印出來。三個架構走的是同一份程式,所以一定完全相同;序列埠與 i386 的螢幕上都看得到(寬 55 個字元,80 欄的文字畫面放得下)。

不交給韌體的原因:三個平台的開機方式各不相同,能印標誌的只有 RISC-V。

| 架構 | kernel 之前執行的東西 | 會印標誌嗎 |
|---|---|---|
| RISC-V | OpenSBI(M-mode 韌體) | 會,一個 "OpenSBI" 大字 |
| AArch64 | 無(QEMU 直接從 kernel 開始) | 沒有韌體可以印 |
| i386 | BIOS | 只顯示在圖形視窗,不在序列埠 |

### RISC-V:拿掉韌體自己的標誌

RISC-V 的韌體 OpenSBI 會先印出自己的標誌,這樣 RISC-V 就比另外兩個架構多一個。那個標誌寫死在韌體的程式裡,kernel 改不到,所以用 OpenSBI 的原始碼重新建置一份,只改一處:拿掉印出標誌的那一行。韌體的版本與平台資訊照常顯示。

`scripts/build-opensbi.sh` 負責下載原始碼、修改、建置,產生 `firmware/opensbi-riscv64-welyos.bin`。這個檔案已放進版本庫,一般不必重新建置。`scripts/qemu.sh` 預設使用它;設定 `QEMU_BIOS=default` 可以改回 QEMU 內附的版本。OpenSBI 以 BSD 2-Clause 授權,允許修改與散布,條件是附上授權條款(`firmware/LICENSE-OpenSBI.txt`)。

### 過程:先試了「把韌體的標誌改成 WElyOS」

最初的要求是把 OpenSBI 的標誌字樣換成 WElyOS,第一版就是這樣做的,並且成功了。後來需求變成「三個平台要有一樣的標誌」,而另外兩個平台沒有對應的韌體可以改,於是改為現在的做法:標誌移到 kernel,韌體的標誌直接拿掉。

那一版留下一個值得記錄的坑。換上 WElyOS 字樣後,標誌的第五行少了一個 `|`:

```
    \  /\  /    | |____  | | | |_| | |  __| |  ____) |
                                       ^ 這裡應該是 |
```

檢查輸出的原始位元組,那個位置是一個 0(字串結尾符號);韌體檔裡的字串本身是正確的。原因在 OpenSBI v1.3 的 `sbi_printf`:它先把結果組進一個固定大小的緩衝區再輸出,較長的字串跨過緩衝區邊界時,邊界上的那個字元被結尾符號蓋掉。原本的 OpenSBI 標誌比較窄,碰巧沒有踩到。當時的解法是改用不經過緩衝區的 `sbi_puts`;現在韌體不印標誌了,這個修改也就不需要了。

### 決策:用 OpenSBI v1.3,不是 QEMU 內附的 v1.4

v1.4 起韌體一律建置成「可重新定位」的形式,連結時需要工具鏈支援,而建置 kernel 用的裸機工具鏈(`riscv64-unknown-elf-`)沒有這項功能。

| 選項 | 結果 |
|---|---|
| 建置 v1.4 | 需要另外安裝 Linux 版的交叉編譯器(`gcc-riscv64-linux-gnu`),要管理員權限 |
| 直接改 QEMU 內附韌體的二進位檔 | 換了 QEMU 版本就失效,也不容易確認改得對 |
| **建置 v1.3** | 現有的工具鏈就能建置 |

代價是韌體比 QEMU 內附的舊一版(提供的 SBI 規格是 1.0 而不是 2.0)。WElyOS 用到的功能(計時器、處理器間中斷、啟動其他 hart)兩個版本都有,測試結果也相同。

### 評估過但沒有採用:改用 UEFI(EDK II)

EDK II 是 UEFI 韌體的開放原始碼實作,三個架構都有,也有開機標誌的功能。沒有採用的理由:

- 它的標誌是一張圖片,只顯示在圖形畫面;我們主要在序列埠的文字畫面操作,看不到。
- 換標誌要自己重新建置 EDK II,三個架構各一次,需要額外的工具。
- kernel 的開機方式要整個重做:UEFI 不會載入現在的 kernel 格式,需要另外寫 UEFI 的開機載入器。
- RISC-V 上 EDK II 是跑在 OpenSBI 之上的,OpenSBI 的標誌仍會先出現。

UEFI 開機本身有它的價值(標準的開機流程、能在實機開機、有圖形畫面),但那是另一個大題目,不該只為了標誌去做。

### 驗證

- 三個架構印出的標誌逐位元組相同(比對雜湊值)。
- 三個架構各 20 個回歸情境共 60 個,結束碼都符合預期;`autoexit selftest stress memtest` 在 1、4、8 顆 CPU 下三個架構共 27 次全部通過。
- 改標誌之前、以 v1.3 韌體(當時帶 WElyOS 標誌)另外跑過 RISC-V 的 1、2、4、8 顆 CPU 各 10 次與互動式 shell,全部通過。
- `QEMU_BIOS=default` 仍可切回 QEMU 內附的韌體。

### 已知限制

- RISC-V 的韌體比 QEMU 內附的舊一版(v1.3 對 v1.4)。
- RISC-V 開機時,標誌之前仍有韌體印出的版本與平台資訊(約 40 行),另外兩個架構沒有。
- 韌體的二進位檔(約 135KB)直接放在版本庫裡:不必另外建置就能用,但它和建置腳本不會自動同步。
- 標誌是純 ASCII 文字;i386 的圖形視窗裡顯示的也是文字模式的畫面,不是圖片。

---

## 第 22 步:可恢復的 page fault、寫入時複製、stack 自動成長與 fork

到上一步為止,page fault 只有一種結果:出錯的行程被終止。這一步讓 kernel 能「處理完之後讓程式繼續執行」,並用這個能力做三件事:

- **寫入時複製**(copy-on-write,COW):多個行程先共用同一頁,有人寫入時才複製
- **`fork`**:複製目前的行程,靠寫入時複製才做得有效率
- **stack 自動成長**:stack 用到哪裡才配置到哪裡

### 可恢復的 page fault

使用者程式存取一個「現在不能這樣存取」的位址時,CPU 會進入 kernel 並記下出錯的位址與是不是寫入。之前 kernel 一律終止行程;現在先問一句「這是不是我可以解決的情況」:

```
使用者程式寫入位址 X ──▶ page fault ──▶ proc_page_fault(X, 寫入)
                                           │
                        可以解決(見下)   │   不能解決
                 ┌─────────────────────────┴─────────────────────┐
                 ▼                                               ▼
     修正頁表,從例外返回                                  終止行程(和以前一樣)
     CPU 重新執行同一條指令,這次成功
```

關鍵在「重新執行同一條指令」:三個架構在 page fault 時記下的返回位址都指向出錯的那條指令本身,所以 kernel 修好頁表後直接返回,程式完全不知道發生過什麼。

三個架構取得資訊的方式:

| 架構 | 出錯位址 | 是不是寫入 |
|---|---|---|
| i386 | `CR2` | 錯誤碼的 bit 1 |
| RISC-V | `stval` | `scause` 為 15(store page fault) |
| AArch64 | `FAR_EL1` | `ESR_EL1` 的 bit 6(WnR),且例外類別是資料存取 |

### 寫入時複製

**問題**:`fork` 要讓子行程擁有和父行程一樣的記憶體內容。真的把每一頁都複製一遍很慢,而且經常白做 — 最常見的用法是 fork 之後立刻 exec,複製的內容馬上被丟掉。

**做法**:fork 時不複製任何一頁的內容。

1. 子行程的頁表指向和父行程**同一批實體頁**。
2. 原本可寫的頁,在**父子兩邊**都改成唯讀,並標記 `VM_COW`。
3. 之後誰先寫入,誰就觸發 page fault。kernel 看到那一頁有 `VM_COW` 標記,才配置一頁新的、把內容複製過去、改成可寫。
4. 從來沒被寫入的頁永遠共用。

`VM_COW` 和上一步的 `VM_SHARED` 一樣,記在頁表項目裡保留給作業系統使用的位元,硬體會忽略它。它的用途是區分兩種「唯讀」:本來就唯讀的頁(寫入是錯誤),和暫時唯讀的頁(寫入時要複製)。

**一頁有幾個人在用?** 需要參考計數。`pmm` 為每一頁加了一個 16 位元的計數(128MB 記憶體多用 64KB):配置時為 1,每多一個共用者加一,`pmm_free` 改為減一、降到 0 才真的釋放。這個設計讓其他程式不必改:位址空間釋放時照常對每一頁呼叫 `pmm_free`,共用的頁自然只是少一個使用者。

參考計數也帶來一個最佳化:寫入時若發現計數是 1(其他人都已經複製走或結束了),**不必複製,直接改回可寫**。測試中看得到這兩種情況各發生了幾次:

```
user: copy-on-write during that test: 35 pages copied, 6 times a page was reused without copying
```

**三個必須處理的地方**:

1. **父行程的 TLB**。父行程有些頁剛從可寫變成唯讀,但它在各顆 CPU 上的 TLB 可能還留著「可寫」的舊紀錄。不清掉的話,父行程的寫入不會觸發 page fault,父子就會改到同一份資料。fork 的最後會讓每顆 CPU 清掉這個位址空間的紀錄(用第 18 步的跨 CPU 呼叫)。

2. **kernel 自己的寫入**。kernel 把資料交給使用者程式時(例如 `read` 的結果),是經由實體位址寫入的,不會觸發 page fault。如果那一頁正和別的行程共用,kernel 會直接寫進共用的那一頁,別的行程就看到了不該看到的修改。所以 `copy_to_user` 遇到不可寫的頁時,會先走一次和 page fault 相同的處理,再寫入。

3. **多條執行緒同時為同一頁觸發**。同一個行程的兩條執行緒在不同的 CPU 上同時寫入同一個共用頁,會各自進入 page fault。處理過程在頁表的鎖之下進行,第二條執行緒進來時會看到「這一頁已經可寫了」,清掉自己這顆 CPU 的舊紀錄後直接重試。

### fork

```c
pid = fork();
if (pid == 0) {
    // 子行程:從同一個地方繼續執行,只是 fork 回傳 0
} else {
    // 父行程:pid 是子行程的編號
}
```

除了複製位址空間,還要讓子行程「從父行程呼叫 fork 的同一個地方」繼續執行。使用者程式進入 kernel 時,它所有的暫存器都被存在 kernel stack 上(trap frame);從 kernel 返回就是把它們還原。所以:

1. 複製一份父行程的 trap frame。
2. 把其中「系統呼叫的回傳值」那個暫存器改成 0。
3. 子行程的執行緒第一次執行時,把這份 trap frame 放到自己的 kernel stack 上,走「從例外返回」的路徑。

trap frame 的格式各架構不同,所以 kernel 的其他部分只把它當成一塊不透明的資料,透過三個函式操作:`arch_frame_size()`、`arch_frame_set_retval()`、`arch_user_resume()`。各架構的例外入口原本就有一段「還原暫存器並返回」的程式,現在加上一個標籤讓它可以從外面進入:

| 架構 | 進入返回路徑的方式 |
|---|---|
| RISC-V | 把 `sp` 指向 trap frame,跳到返回路徑。下次進入 kernel 時用的 stack 來自 `struct cpu`,所以 trap frame 放在 stack 的哪裡都可以 |
| i386 | 同上;下次進入時 CPU 從 TSS 取得 stack 頂端 |
| AArch64 | **trap frame 必須擺在 kernel stack 的最頂端**。AArch64 從使用者模式進入例外時,從「上次離開時的 stack pointer」開始用;若 trap frame 在中間,返回後頂端那一段就永遠用不到了。所以先把它搬到頂端再返回 |

AArch64 的搬移有個細節:來源也在同一條 stack 上、位置比較低,兩塊可能重疊,所以從尾端往前搬(目的地在較高的位址時,這個方向不會蓋掉還沒搬的部分)。

子行程繼承開啟中的檔案與正在執行的程式檔,行程編號是新的。

**限制:只有一條執行緒的行程才能 fork**(和 `exec` 相同)。原因有兩個:其他執行緒可能正在寫入即將被改成唯讀的頁;而且子行程裡只會有呼叫 fork 的那一條執行緒,其他執行緒持有的鎖在子行程裡永遠解不開。

### stack 自動成長

每條執行緒的 stack 佔一格 1MB 的位址範圍,但只預先配置最上面的 16KB。之前用超過 16KB 就會被終止;現在往下用到還沒配置的部分時,page fault 的處理會當場配置一頁(內容為 0)並繼續執行。

- 大多數程式用不到 16KB,不必為它們多配置。
- 每格最下面的一頁永遠不配置(保護頁)。stack 真的用完整格時會在那裡出錯,而不是悄悄長進下一條執行緒的 stack。無窮遞迴的測試仍然會被終止,只是現在是用完 1MB 之後。
- 兩條執行緒同時為同一頁觸發時,只有一條會配置成功,另一條發現已經有了就放棄自己配置的那頁。

### 順帶修正:排程公平性測試的量測方式

第 21 步把搶佔示範的判斷依據從「迴圈圈數」改成「執行期間看到系統時間前進了幾次」。這一步的重複測試中它又失敗了一次(i386、4 顆 CPU,25ms 對 12ms)。

那個量法有漏洞:計時器中斷延遲時,第 12 步的補記機制會一次補上好幾個 tick,但執行緒只看到「時間變了一次」,少算了;兩條執行緒少算的量不一樣,比例就偏了。

改為由排程器自己記帳:每個 tick 記在當時正在執行的執行緒頭上(新增 `thread_cpu_ticks()`),和時間片扣的是同一份帳。改完之後三個架構各跑 12 次,兩條執行緒都在 27~33ms 之間(理想值是各 30ms)。

這個記帳之後也能用來顯示每個行程用了多少 CPU 時間。

### 驗證

使用者程式的測試(`UMODE_FORK`):

| 項目 | 內容 | 結果 |
|---|---|---|
| 基本 | 子行程看到相同的資料;它改了全域變數與區域變數,父行程的不受影響 | 父行程仍是 100 與 7 |
| fork + exec | 子行程把自己換成 `/bin/echo` | 輸出與結束碼來自 echo |
| 連續 20 次 fork | 每個子行程回報不同的結束碼 | 全部正確 |
| 三代 | 子行程再 fork,把孫行程的結束碼加工後回報 | 正確 |
| 大量資料 | stack 上 60000 位元組(需要 stack 成長),子行程改掉一半 | 父行程的內容逐位元組不變 |
| 多執行緒同時觸發 | 子行程的三條執行緒同時寫入同一個仍與父行程共用的頁 | 三條的結果都正確,父行程的那一頁沒被動到 |
| 有其他執行緒時 fork | | 被拒絕 |

另外:`UMODE_STACK_GROW` 遞迴用掉約 180KB 的 stack 後正常返回;原本的 stack 溢位測試仍然被終止。

**記憶體沒有流失**:既有的檢查(所有行程結束後,空閒頁數與 heap 回到原樣)在加入參考計數與共用頁之後仍然通過,代表每一頁的計數都正確歸零。

回歸:三個架構各 20 個情境共 60 個,結束碼都符合預期。`autoexit selftest stress memtest` 在 1、4、8 顆 CPU 下各 10 次,三個架構共 90 次全部通過;i386 舊 CPU 型號與互動式 shell 也通過。

第一次建置後,fork 的所有測試在三個架構、1 與 4 顆 CPU 下就全部通過。

### 效能

- 耗時(4 顆 CPU):只開機 0.21~0.34 秒;`selftest` 0.86~1.14 秒(測試又增加了)。
- 建立並等待 300 個循序行程(tick):

| | 1 顆 CPU | 4 顆 CPU | 8 顆 CPU |
|---|---|---|---|
| i386 | 57~64 | 77~87 | 106~145 |
| riscv64 | 96~114 | 140~153 | 201~234 |
| aarch64 | 71~84 | 200~219 | **646~750** |

1 與 4 顆 CPU 和上一步相近。**AArch64 在 8 顆 CPU 下特別慢**(約是 4 顆時的 3 倍多)。這是這一步第一次量 8 顆 CPU 的這個數字,所以不確定是不是這一步造成的;我的猜測是 AArch64 用的「廣播式」TLB 失效指令在模擬器上隨 CPU 數量變得很貴,但沒有驗證。列在已知限制。

### 已知限制

- **只有單一執行緒的行程可以 fork**。
- **fork 仍要走訪並複製整份頁表**,成本和行程對映了多少頁成正比(不是和寫入了多少頁)。沒有共用頁表本身。
- **參考計數是 16 位元**,同一頁最多 65535 個共用者,超過會 panic。
- **整個寫入時複製的處理在一把全域的鎖之下**(頁表的鎖),多個行程同時大量觸發時會互相等待。
- **從檔案載入程式時,資料區段仍然立即複製**;沒有「程式的某一頁用到才從檔案讀進來」。
- **stack 成長不檢查 stack pointer**:只要存取落在某條 stack 的那一格 1MB 內就會配置,即使那是一個亂指的指標。
- **stack 只會長大,不會縮回去**;每條執行緒仍然只有 1MB 的上限。
- **kernel 模式的 page fault 仍然一律是致命錯誤**。kernel 不直接存取使用者的位址(都經由實體位址),所以目前不需要。
- **記憶體不足時,觸發寫入時複製的行程會被終止**,沒有其他的處理方式。
- **AArch64 在 8 顆 CPU 下建立行程特別慢**,原因未確認。 **〔後續:已查明並修正,見下一節「效能調整:多核心下的 TLB 失效與跨 CPU 通知」。〕**
- **shell 仍然用 `spawn` 而不是 fork + exec**(比較快,也不需要改)。
- **沒有父子關係**:fork 出來的行程和 `spawn` 的一樣,任何人都可以 `wait` 它,沒有人 `wait` 的話紀錄會留著。 **〔後續:已解決:第 24 步。〕**

---

## 效能調整:多核心下的 TLB 失效與跨 CPU 通知

第 22 步量到 AArch64 在 8 顆 CPU 下建立行程特別慢:300 個循序行程要 646~750 tick,4 顆時只要 200~219。這一節記錄查的過程與修正。

### 先確認是不是新問題

把第 18 步與第 20 步的版本各自重新建置,在同樣的條件下量:

| 版本 | AArch64、8 顆 CPU、300 個行程 |
|---|---|
| 第 18 步(其他 CPU 開始參與排程) | 583、619、600 tick |
| 第 20 步 | 556、583、545 tick |
| 第 22 步 | 646~750 tick |

從第 18 步就這樣了,不是 fork 或寫入時複製造成的。只是之前只量過 1 與 4 顆 CPU,沒有注意到。

### 用實驗找原因

逐一關掉可疑的機制(只為了量測,那樣的 kernel 並不正確),看時間怎麼變:

| 實驗 | 8 顆 CPU、300 個行程 |
|---|---|
| 基準 | 560、573、557 tick |
| A:AArch64 的 TLB 失效改成只作用在自己這顆 CPU | 255、220、223 |
| B:行程結束時不通知其他 CPU | 262、260、278 |

兩個實驗各自都砍掉一半以上,加起來超過總量,代表它們會互相放大。

### 原因

**1. 廣播式的 TLB 失效被用在不需要廣播的地方。**

第 18 步為了 TLB shootdown,把 AArch64 的失效函式整個換成「廣播」的指令(`tlbi ...is`,由硬體通知所有 CPU)。但同一個函式也被用在**新增對映**之後 — 那種情況只需要清自己這顆 CPU 的紀錄(別的 CPU 不可能有一筆「還不存在的對映」的紀錄)。結果每對映一頁就廣播一次,而建立一個行程要對映十幾頁。

廣播在模擬器上特別貴:QEMU 要讓所有模擬的 CPU 都停下來配合。CPU 越多、而且正在執行(而不是休眠)的 CPU 越多,就越貴。

**2. 行程結束時通知了所有 CPU。**

第 18 步在釋放位址空間前,會以跨 CPU 呼叫問每一顆 CPU「你還載入著它嗎」。但大多數行程只在一兩顆 CPU 上執行過,其他 CPU 根本沒碰過它。每次通知都要打斷對方;對方若在休眠,還得先把它叫醒 — 在模擬器上,叫醒一顆休眠的 CPU 很慢。

**互相放大**:第 2 點把所有 CPU 都叫醒了,它們醒著的時候,第 1 點的廣播就更貴。

### 修正

**廣播只用在真正需要的地方。** AArch64 的失效函式分成兩個:只清自己的(`arch_tlb_flush`),與廣播的(`arch_tlb_flush_broadcast`)。新增對映用前者;移除或更改既有的對映才用後者。

**記錄每個位址空間用過哪些 CPU。** `struct vm_space` 多一個位元遮罩 `used_cpus`:某顆 CPU 載入這個位址空間時,把自己那一位設起來。只增不減。

它的意義是一個保證:**某一位是 0,代表那顆 CPU 沒有、也不曾有過這個位址空間的任何 TLB 紀錄**。所以:

| 動作 | 之前 | 現在 |
|---|---|---|
| 釋放位址空間 | 通知所有其他 CPU | 只通知 `used_cpus` 裡的 CPU |
| fork 之後清父行程的 TLB | 通知所有其他 CPU | 同上 |
| 更改某個行程的一頁(寫入時複製) | 通知所有其他 CPU(或廣播) | 除了自己以外沒人用過:只清自己的;否則只通知用過的 |
| 更改 kernel 的對映 | 通知所有其他 CPU(或廣播) | 不變 — kernel 的對映每顆 CPU 都可能用過 |

為此新增 `smp_call_mask()`:和 `smp_call_others()` 相同,但只呼叫指定的 CPU;沒有對象時立刻返回,連鎖都不必拿。

遮罩要在「真正載入之前」設定。順序若相反,會有一個瞬間「已經載入、遮罩還沒設」,這時另一顆 CPU 來查就會以為這顆 CPU 沒有它的紀錄而跳過通知。

這個修正不是 AArch64 專用的。RISC-V 與 i386 沒有廣播式的指令,每一次失效都要發處理器間中斷,所以「少通知幾顆」對它們同樣有效。

### 結果

300 個循序行程(tick):

| | 1 顆 CPU | 4 顆 CPU | 8 顆 CPU |
|---|---|---|---|
| i386 | 57~64 → 58~65 | 77~87 → **61~64** | 106~145 → **64~68** |
| riscv64 | 96~114 → 92~103 | 140~153 → **105~115** | 201~234 → **112~136** |
| aarch64 | 71~84 → 66~74 | 200~219 → **78~82** | 646~750 → **86~98** |

AArch64 在 8 顆 CPU 下快了約 7 倍。更重要的是,現在**加 CPU 幾乎不再讓循序的工作變慢**:i386 從 1 顆到 8 顆只差幾個 tick。第 18 步記錄的「多核心的代價」有一大半是這兩個問題造成的,不是多核心本身。

### 驗證

這類修改如果有錯,後果是偶發的:某顆 CPU 繼續用一筆過期的 TLB 紀錄,存取到已經釋放或換了內容的頁。所以重複測試的次數比平常多。

- 三個架構各 20 個回歸情境共 60 個,結束碼都符合預期。
- `autoexit selftest stress memtest` 在 1、2、4、8 顆 CPU 下各 15 次,三個架構共 **180 次全部通過**。測試內容包含 300 個行程的建立與結束、fork 與寫入時複製、多執行緒同時觸發複製、行程結束後的記憶體核對。
- i386 舊 CPU 型號通過。

### 教訓

- **量測要涵蓋預期會使用的範圍。** 第 18 步支援到 8 顆 CPU,效能卻只量了 1 與 4 顆。
- **「正確但太保守」的做法會藏很久。** 多廣播、多通知都不影響正確性,所有測試都通過,只是慢。如果不是量了 8 顆 CPU 的數字,不會有人發現。
- **換掉一個函式的實作之前,先看它所有的使用者。** 第 18 步改的是 `arch_tlb_flush`「怎麼做」,卻沒有檢查每個呼叫它的地方需要的是不是同一件事。

### 已知限制

- `used_cpus` 只增不減。一個長時間執行、在每顆 CPU 上都跑過的行程,之後的每一次通知仍會送給所有 CPU。
- 遮罩是 32 位元,目前最多 8 顆 CPU,足夠。
- AArch64 的廣播在模擬器上的成本仍然隨 CPU 數量增加;只是現在只有 kernel 對映的變更(例如回收執行緒 stack 的保護頁)與多 CPU 共用的行程才會用到。
- 跨 CPU 呼叫仍然一次只能有一個在進行。
- 以上的時間都是模擬器上的數字。「少發中斷、少廣播」在實機上同樣是好事,但倍數不會相同。

---

## 第 23 步:管線

```
$ ls /bin | wc
1 8 41
$ cat /etc/motd | cat | cat | wc
2 18 104
$ wc < /etc/motd
2 18 104
```

管線(pipe)讓兩個程式接在一起,而且彼此不必知道對方的存在:前一個照常寫入標準輸出,後一個照常讀取標準輸入。這一步加入 kernel 裡的管線、`pipe` 系統呼叫,並讓 shell 支援 `|` 與 `<`。

### 管線是什麼

一塊固定大小(4KB)的環狀緩衝區,加上兩個等待佇列(`kernel/pipe.c`):

```
寫入者 ──▶ [ 緩衝區 4KB ] ──▶ 讀取者
           滿了:寫入者睡     空了:讀取者睡
```

緩衝區的大小同時是**流量控制**:讀取者處理得慢,緩衝區滿了,寫入者自然被擋下來等,不會無限制地堆積資料。測試中傳 50000 位元組,寫入者會被擋下來十幾次。

**怎麼知道「結束了」**:管線記錄還有幾個寫端、幾個讀端開著。

| 情況 | 結果 |
|---|---|
| 所有寫端都關閉 | 讀取者把剩下的資料讀完後得到 0,和讀到檔案結尾一樣 |
| 所有讀端都關閉 | 寫入者得到錯誤 `E_PIPE`:繼續寫已經沒有意義 |

這裡的「端」是指**一次開啟**,不是檔案描述符。fork 之後父子共用同一次開啟,要兩邊都關閉才算關閉。這也是為什麼使用管線時,每個行程都要關掉自己用不到的那一端:父行程若留著寫端不關,就永遠讀不到「結束」。

### 管線在 VFS 裡的位置

管線做成一種特殊的 inode(類型 `I_PIPE`):沒有名稱、不在任何目錄裡,兩端都關閉後自動消失(第 19 步的參考計數已經支援「沒有名稱但還有人開著」)。這樣 `read`、`write`、`close`、`dup2`、檔案描述符的繼承全部沿用既有的程式,不必為管線各寫一份。

需要兩處特別處理:

- **讀寫不能拿著 inode 的鎖**。一般檔案的讀寫是在 inode 的鎖之下進行的;管線的讀取可能要等對方寫入,而對方寫入也要同一把鎖,會互相卡死。所以 VFS 遇到管線時直接呼叫它的讀寫函式,由管線自己上鎖。
- **VFS 在一次開啟完全關閉時通知**(`inode_ops` 新增的 `closed`),管線才知道讀端 / 寫端各剩幾個。

### spawn 只繼承 0、1、2 號描述符

第 20 步的 `spawn` 會繼承建立者**所有**開啟中的檔案。做管線時這會出問題:

```
shell 要執行  A | B
  建立管線,shell 手上有讀端與寫端
  建立 A(輸出接到寫端)—— 但 A 也繼承了 shell 手上的「讀端」
  建立 B(輸入接到讀端)
```

A 自己就握著讀端,所以即使 B 提早結束,A 的寫入也不會得到 `E_PIPE`;緩衝區滿了之後,A 會永遠等一個不存在的讀取者。反過來,B 若繼承到寫端,就永遠讀不到結束。

改為:**`spawn` 只繼承 0、1、2 號(標準輸入、輸出、錯誤)**。新程式要用的東西,建立者先接到這三個號碼上;其他的不會外流。`fork` 仍然繼承全部(子行程是父行程的複本)。

這比「逐一標記哪些描述符不要繼承」簡單,也不容易漏掉。

### 一般化:被終止的行程從任何睡眠中醒來

管線帶來一種新的「可能永遠等下去」:讀一條沒有人會再寫的管線。第 21 步為主控台輸入處理過同樣的問題,當時的做法是終止行程時特別去叫醒等輸入的人。再為管線加一個特例不是好方向。

改為一般化的做法:每條執行緒記住自己睡在哪個等待佇列上;終止行程時,直接把它所有正在睡的執行緒從佇列(或休眠串列)裡拿出來、放回執行佇列。

對被叫醒的那段程式來說,這是一次「條件還沒成立就醒來」。第 11 步定下的規則是所有等待都要在醒來後重新檢查條件,所以 mutex 之類的等待會重新睡下去(之後在返回使用者模式前結束);會讓行程等很久的地方(主控台、管線、等待另一個行程結束)則另外檢查「我是不是被終止了」,是的話就放棄等待。

主控台與 futex 原本的特例因此可以拿掉。

### shell 的管線

`A | B | C` 的做法(`user/sh.c`):

```
對每個命令 i:
    若不是最後一個:建立管線 i
    把自己的標準輸入 / 輸出暫時換成它該用的東西
        輸入:上一條管線的讀端,或 "< 檔案"
        輸出:這一條管線的寫端,或 "> 檔案"
    spawn(新行程繼承 0、1、2 號)
    換回來;關掉自己手上的寫端;讀端留給下一個命令
全部建立完,再逐一 wait
```

兩個重點:

- **建立完一個命令,shell 要立刻關掉那條管線的寫端**。只要還有人開著寫端,讀的那一方就等不到結束。
- **全部建立完才開始等**。管線中的程式是同時在執行的;如果建立一個就等一個,前面的程式寫滿緩衝區後會卡住,因為後面的程式還沒被建立。

另外加了 `wc`(行數、字數、位元組數),並讓 `cat` 在沒有參數時讀標準輸入,這樣才有東西可以接在管線的中間與尾端。

### 驗證

**kernel 端**(`pipe_demo`):

| 項目 | 結果 |
|---|---|
| 一條執行緒寫 50000 位元組、另一條讀(緩衝區只有 4KB) | 順序與內容逐位元組正確;寫端關閉後讀到 0 |
| 方向固定:從寫端讀、往讀端寫 | 被拒絕 |
| 沒有讀端時寫入 | `E_PIPE` |
| 寫端關閉後緩衝區裡還有資料 | 仍然讀得到,讀完才是結束 |
| 記憶體 | 全部關閉後 heap 回到原樣 |

**使用者端**(`UMODE_PIPE`):fork 後子行程寫、父行程讀到結束;40000 位元組的傳輸並逐位元組核對;把 `/bin/echo` 的輸出接進管線再讀出來;沒有讀端時寫入得到 `E_PIPE`;把 kernel 的位址交給 `pipe()` 被拒絕。

**shell**:script 中的 `ls /bin | wc`、`cat /etc/motd | cat | wc >> 檔案`、`wc < /etc/motd`、語法錯誤的 `echo too | many | | pipes`,kernel 另外核對檔案內容。互動模式以模擬打字測了三個架構,包含四段的管線與「第一個命令不存在」的情況:

```
$ nosuch | wc
sh: nosuch: command not found
0 0 0
```

回歸:三個架構各 20 個情境共 60 個,結束碼都符合預期。`autoexit selftest stress memtest` 在 1、2、4、8 顆 CPU 下各 8 次(三個架構共 96 次),加上最後一輪 36 次,全部通過。

### 踩坑:記憶體核對又漏了 heap 的成長

加入管線後,使用者模式測試最後的記憶體核對失敗(「頁數沒有回到原樣」)。不是管線漏了記憶體:管線的結構約 4.2KB,第一次配置時 heap 向頁面配置器多要了幾頁,而 heap 拿到的頁不會歸還(第 9 步的已知限制)。其他測試早就把「這段期間 heap 長大了多少」算進去,只有這一個檢查沒有。補上。

### 順帶:i386 的開機時間

量開機時間時發現 i386 比另外兩個架構慢(kernel 本身約 30ms,RISC-V 約 14ms,AArch64 約 9ms)。暫時關掉螢幕輸出後降到 16ms,所以差距來自 VGA。

做了一個小修正:原本每輸出一個字元就更新一次硬體游標(4 次 I/O 埠寫入),改為每次 `kprintf` 結束時更新一次。效果有限(約省 5ms)。剩下的成本在螢幕捲動:每捲一列要搬動整個畫面,而模擬器對顯示記憶體的每一次讀寫都要特別處理。要再改善得在記憶體裡保留一份畫面的副本、累積起來再一次更新,這會讓畫面的更新稍微延遲,先不做。 **〔後續:第 25 步採用另一個做法 — 硬體捲動,不需要副本,也沒有延遲。〕**

目前只開機的耗時(4 顆 CPU):i386 約 0.40 秒、riscv64 約 0.32 秒、aarch64 約 0.25 秒,其中 kernel 本身只佔 0.01~0.03 秒,其餘是模擬器的啟動。`selftest` 為 0.90~1.27 秒。

### 已知限制

- **管線的容量固定為 4KB**,整個結構(含緩衝區)一次配置。
- **管線的資料複製在排程器的鎖之下進行**(一次最多 4KB),期間其他 CPU 的排程操作要等。
- **寫入沒有人讀的管線只會得到錯誤碼**,不會終止寫入的程式(沒有訊號)。不檢查寫入結果的程式會繼續白做工,直到自己結束。
- **管線中的命令數上限是 6**;shell 等待時依序等,所以整條管線的結束碼是最後一個命令的。
- **`spawn` 只繼承 0、1、2 號**。需要把其他描述符交給新程式時,目前只能用 `fork`。
- **沒有具名的管線**(FIFO),只能在有共同來源的行程之間使用。
- **被提早叫醒的執行緒若正在等 mutex**,它借給鎖的擁有者的優先權可能要等到下一次有人等那把鎖時才重新計算。
- **i386 的螢幕輸出慢**,如上所述。 **〔後續:第 25 步改用硬體捲動,每捲動一列存取顯示記憶體的次數從約 3900 次降到 80 次。〕**

---

## 第 24 步:行程的父子關係與訊號

```
$ sleep 30
^C
$ status
130
$ sleep 30 | sleep 30 | cat
^C
$ status
130
```

到上一步為止,執行中的程式沒有辦法從外面叫停:Ctrl-C 只會放棄正在打的那一行。這一步加入讓它成為可能的三樣東西:行程之間的父子關係、訊號、行程群組。

### 父子關係與 zombie

行程結束後不會立刻消失:它的結束碼要留到「有人來問」為止。這種已經結束、等著被回收的行程稱為 zombie。

之前的規則是「任何人都可以 `wait` 任何行程」,這有兩個問題:行程結束後若沒有人 `wait`,紀錄永遠留著;而且一個行程可以搶走別人子行程的結束碼。

現在每個行程記得是誰建立它的(`ppid`),規則改為:

| 情況 | 處理 |
|---|---|
| 父行程呼叫 `wait` | 取得結束碼,紀錄被回收。**只能等自己的子行程** |
| 父行程先結束了 | 子行程變成沒有父行程。它結束時 kernel 直接回收,不留紀錄 |
| 父行程結束時,有子行程已經是 zombie | 當場回收 |

這樣不論父行程有沒有好好 `wait`,紀錄都不會永遠留著。

Unix 的做法是把失去父行程的行程過繼給 1 號行程(`init`),由它負責 `wait`。這裡沒有那樣的行程,所以由 kernel 直接回收,效果相同。

新增 `waitpid(pid, &status)`:

- `pid` 為 -1 表示「任何一個子行程」,回傳值是結束的那個子行程的編號。
- 結束碼寫到呼叫者指定的位置,不再和錯誤碼共用回傳值(第 20 步列出的限制)。
- 沒有子行程可等時回傳 `E_CHILD`,而不是永遠等下去。

原本的 `wait(pid)` 保留(shell 還在用),只是現在只能等自己的子行程。

### 訊號

訊號(signal)是從外面通知一個行程的方法。這裡只實作最基本的用途:**請行程結束**。

| 訊號 | 編號 | 慣用的場合 |
|---|---|---|
| `SIGINT` | 2 | 使用者按了 Ctrl-C |
| `SIGKILL` | 9 | 強制終止 |
| `SIGTERM` | 15 | 一般的「請結束」(`kill` 指令的預設) |

三者的效果相同。被訊號終止的行程,結束碼是 **128 + 訊號編號**(Unix shell 的慣例),所以從結束碼看得出它是自己結束的還是被終止的:130 是 Ctrl-C,137 是 `kill -9`。

實作上沿用第 15 步就有的機制:把行程標記為「結束中」,它的每條執行緒在下一次要返回使用者模式時自己結束。加上第 23 步的「正在睡的執行緒會被叫醒」,不論行程當時在做什麼(執行中、睡覺、等輸入、等管線)都會很快結束。

系統呼叫 `kill(pid, sig)`:`pid` 大於 0 是單一行程,小於 0 是整個行程群組。

**刻意沒做的**:行程不能攔截或忽略訊號(沒有訊號處理函式)。那需要讓 kernel 在返回使用者模式時改去執行使用者指定的函式,再回到原本的地方,是另一個層次的複雜度。

### 行程群組與前景

`sleep 30 | sleep 30 | cat` 是三個行程。Ctrl-C 應該三個一起終止,而不是只終止其中一個,也不該把 shell 自己終止掉。

- **行程群組**(`pgid`):一組一起接收訊號的行程。新行程沿用建立者的群組;`setpgid` 可以把自己或子行程放進別的群組。
- **前景群組**:主控台記著一個群組編號。使用者按 Ctrl-C 時,這一組的所有行程收到 `SIGINT`。

shell 執行一條管線時:

```
建立所有行程
把它們放進同一個群組(以第一個行程的編號當群組編號)
把這個群組設為前景        tcsetfg(pgid)
等它們全部結束
把前景設回「沒有」        tcsetfg(0)
```

「沒有前景」時 Ctrl-C 只放棄正在打的那一行,所以在提示符號下按 Ctrl-C 不會把 shell 結束掉。這比 Unix 的做法簡單:Unix 的 shell 是靠「忽略 `SIGINT`」保護自己,而我們沒有忽略訊號的機制。

Ctrl-C 是在計時器中斷裡處理的(輸入是輪詢的; **〔後續:第 25 步起輸入由序列埠的中斷驅動,Ctrl-C 在那個中斷裡處理。〕** ),終止行程的動作只是設定幾個旗標並把睡著的執行緒叫醒,可以在中斷中完成。

另外加了兩個小工具:`sleep`(`sleep 3`、`sleep 200ms`)與 `kill`(`kill 12`、`kill -9 12`)。

### 踩坑:剛好要睡的行程被終止,仍然睡滿整段時間

加入測試後,自我測試偶爾會多花約 1 秒(i386 與 AArch64 各出現一次,RISC-V 沒有)。測試裡的子行程在迴圈中每次睡 1 秒,等著被終止。

時序:

1. 子行程在一顆 CPU 上正要呼叫「睡 1 秒」。
2. 父行程在另一顆 CPU 上終止它:標記為結束中,並把「正在睡」的執行緒叫醒 — 但它此刻還沒睡,沒有東西可叫。
3. 子行程接著睡下去,睡滿 1 秒才醒來、才發現自己該結束了。

行程確實結束了,只是晚了 1 秒。修正:進入睡眠前(持有排程器的鎖時)檢查自己是否已被標記。標記與檢查在同一把鎖之下,所以不會有「剛檢查完就被標記、接著照樣睡下去」的空檔。這和第 11 步談過的 lost wakeup 是同一類問題:**「檢查條件」與「睡下去」必須是不可分割的一步**。

修正後三個架構在 1、4、8 顆 CPU 下各跑 6 次,自我測試的時間都穩定(同一組設定下相差不到 5%)。

### 回歸測試腳本進版本庫

之前的回歸測試靠的是放在暫存目錄的腳本,這一步進行到一半時暫存目錄被清空,腳本就沒了。現在放進專案:

```
bash scripts/regress.sh            # 三個架構各 20 個情境,核對結束碼
bash scripts/regress.sh -r 6       # 另外以 1、4、8 顆 CPU 各重複 6 次全部測試
bash scripts/regress.sh -q         # 只跑最基本的三個情境
bash scripts/regress.sh -a riscv64 # 只測一個架構
```

每個情境預期的結束碼寫在腳本裡,不符合的會列出來並指出輸出檔的位置。`scripts/type-input.py`(模擬打字)也加了「停頓」的寫法,用來等命令開始執行後再按 Ctrl-C。

### 驗證

使用者程式的測試(`UMODE_SIGNAL`):

| 項目 | 結果 |
|---|---|
| 終止一個不會自己結束的子行程 | `waitpid` 取得結束碼 137(128 + 9) |
| 兩個子行程各自結束,兩次 `waitpid(-1)` | 各收到一個,不重複;第三次得到 `E_CHILD` |
| 兩個子行程放進同一個群組,`kill(-群組)` | 兩個都以 143(128 + 15)結束 |
| 孤兒:子行程建立孫行程後自己先結束 | 孫行程結束後被自動回收(最後的記憶體核對確認) |
| 終止不存在的行程、不支援的訊號編號、`wait` 不是自己的子行程、把結束碼寫到 kernel 的位址 | 全部被拒絕 |

互動測試(模擬打字,三個架構輸出相同):`sleep 30` 與三段的管線都在按下 Ctrl-C 後立刻結束,`status` 顯示 130;在提示符號下按 Ctrl-C 不影響 shell。整段測試約 4 秒完成(其中包含兩個 `sleep 30`)。 **〔後續:這個測試是用管線送進字元的,沒有經過終端機。第 25 步發現在終端機直接執行時,Ctrl-C 會被終端機攔下來結束 QEMU,到不了 WElyOS;已在該步修正(`scripts/qemu.sh`),之後的互動測試改經由虛擬終端機執行。〕**

回歸(`scripts/regress.sh -r 6`):三個架構各 20 個情境共 60 個,全部符合預期;全部測試在 1、4、8 顆 CPU 下各 6 次,三個架構共 54 次全部通過。i386 舊 CPU 型號與主機端的執行佇列測試也通過。

耗時:只開機 0.25~0.41 秒(不變);`selftest` 在 4 顆 CPU 下約 0.9~1.3 秒。

### 已知限制

- **訊號只有「終止」一種效果**,不能攔截、忽略或自訂處理;也沒有暫停與繼續(Ctrl-Z)。 **〔後續:第 26 步加入暫停與繼續(`SIGSTOP`、`SIGTSTP`、`SIGCONT`)與 Ctrl-Z;攔截與自訂處理仍然沒有。〕**
- **任何行程都可以終止任何行程**(沒有擁有者與權限的概念)。 **〔後續:已解決:第 34 步起,對別人的行程送訊號需要特權。〕**
- **寫入沒有人讀的管線仍然只得到錯誤碼**,沒有對應的訊號(Unix 的 `SIGPIPE`)。
- **只有一個主控台、一個前景群組**,沒有「工作階段」(session)的概念,也沒有背景執行(`&`)。 **〔後續:第 25 步加入背景執行與 `jobs`、`fg`、`wait`。〕**
- **Ctrl-C 與設定前景之間有極短的空檔**:shell 剛建立行程、還沒把它設為前景時按下 Ctrl-C,只會放棄那一行,命令照常執行。
- **被終止的行程不會被強行打斷**,要等它下一次進入 kernel(系統呼叫或計時器中斷)。在使用者模式中空轉的程式最多延遲 1ms。
- **沒有 `ps` 這類列出行程的工具**(需要另一個系統呼叫),所以 `kill` 要自己知道行程編號。 **〔後續:已解決:第 25 步加入 `ps`。〕**
- **由 kernel 建立的行程**(開機時的 shell、自我測試的程式)沒有父行程,由 kernel 自己等待。

---

## 第 25 步:中斷驅動的輸入、背景執行與 ps

```
$ sleep 30 &
[1] started, pid 3  sleep 30
$ ps
  PID  PPID  PGID  THR  STATE     TIME  NAME
    1     0     1    1  sleep       6  sh
    3     1     3    1  sleep       0  sleep
    4     1     4    1  run         2  ps
$ jobs
[1] running  sleep 30
$ fg
[1] foreground  sleep 30
^C
$ status
130
```

這一步有兩個主題:主控台的輸入從輪詢改成由中斷驅動;shell 可以把命令放到背景執行,並有 `ps` 可以看目前有哪些行程。過程中另外找到並修正了兩個問題:i386 的螢幕捲動太慢(會讓輸入遺失),以及在終端機直接執行時 Ctrl-C 其實到不了 WElyOS。

### 中斷驅動的輸入

到上一步為止,輸入是輪詢的:計時器每 1ms 由第 0 顆 CPU 去問一次序列埠。現在改成序列埠收到字元時自己發出中斷,中斷處理程式呼叫 `console_rx()` 把硬體裡的字元全部取出來。第 21 步把「取字元」(`arch_console_getc`)與「取到之後怎麼處理」(`console.c`)分成兩層,所以行規程(line discipline)完全不必改,只換了「由誰呼叫」。

每個架構多一個函式 `arch_console_irq_init()`,要做的事分兩邊:讓序列埠在收到資料時拉起中斷線,以及讓中斷控制器把這條線送到 CPU。

| | i386 | RISC-V | AArch64 |
|---|---|---|---|
| 序列埠那一側 | 16550 的中斷致能暫存器 bit 0;另外要拉起 OUT2(PC 上 UART 的中斷線經過它才接到 PIC) | 16550 的中斷致能暫存器 bit 0 | PL011 的中斷遮罩:接收、接收逾時兩個都開 |
| 中斷控制器 | PIC 的 IRQ 4(向量 36) | PLIC 的 10 號來源 | GIC 的 33 號(SPI 1) |
| CPU 看到的 | 向量 36 | S-mode 外部中斷(scause 中斷碼 9) | IRQ,向 GIC 認領後得到 33 |
| 送給哪顆 CPU | PIC 的輸出只接到開機的那顆 | 只啟用開機那顆 hart 的 S-mode context | 目標設為開機的那顆 |

**RISC-V 第一次用到 PLIC**(Platform-Level Interrupt Controller,平台層級中斷控制器)。到目前為止 RISC-V 只用了 CPU 內建的兩條中斷線(計時器、軟體中斷),週邊裝置的中斷則要經過 PLIC 彙整後,從第三條線「外部中斷」進來。PLIC 把「某顆 hart 的某個權限模式」稱為一個 context(QEMU virt 上 hart N 的 S-mode 是 context 2N+1),每個 context 各有一組啟用位元、一個優先權門檻,以及「認領 / 完成」暫存器:讀取它得到是哪個來源(同時標記為處理中),處理完把同一個編號寫回去,PLIC 才會再送下一次。開機的 hart 不一定是 0 號(第 16 步),所以 context 編號用它實際的 hart 編號計算。PLIC 的暫存器需要對映進 kernel 的頁表,只對映用得到的三段(共 18 頁)。

**AArch64 第一次用到共用的週邊中斷**(SPI, Shared Peripheral Interrupt)。先前用的計時器與處理器間中斷都是每顆 CPU 私有的號碼(0~31),32 號以上則不屬於特定的 CPU,要在 `GICD_ITARGETSR` 指定送給誰。這個暫存器在 0~31 號是唯讀的,讀出來正好是「自己這顆 CPU」的遮罩,直接拿來當 33 號的目標。

**決策:只送給開機的那顆 CPU。** 輸入的量很小,分散到多顆 CPU 沒有好處,反而要處理「兩顆 CPU 同時認領」的情況。

**決策:接收門檻設為 1 個字元。** i386 的 16550 原本設成「累積 14 個字元才通知」,那是給輪詢用的設定;中斷驅動時要改成一有字元就通知,否則打一個字要等硬體的逾時機制才會送出中斷。

計時器中斷裡不再有任何和主控台有關的工作。Ctrl-C 現在是在序列埠的中斷裡處理的(第 24 步是在計時器中斷裡),做的事情相同。

### 量測:貼上文字還會不會掉字

測試方法:送進 40 行命令(每行 51 個字元,共約 2000 個字元),每個字元間隔固定的時間,看 shell 完整執行了幾行。對照組是上一步的 kernel(輪詢)。

| 每個字元的間隔 | RISC-V 輪詢 → 中斷 | AArch64 輪詢 → 中斷 | i386 輪詢 → 中斷 → 中斷 + 硬體捲動 |
|---|---|---|---|
| 2 ms | 18 → 40 | 40 → 40 | 10 → 6 → 40 |
| 1 ms | 0 → 40 | 33 → 40 | 0 → 0 → 39~40 |
| 0.3 ms | 0 → 約 16 | — | — |
| 完全不間隔(一次整段送進去) | 0 → 0 | 0 → 0 | 0 → 0 |

(舊版的數字是用較嚴格的比對方式量的,可能少算一兩行,不影響結論。)

結論有三點:

1. **中斷驅動確實有效**:每個字元間隔 2ms 以上時三個架構都不再掉字;1ms 時 RISC-V 與 AArch64 不掉字,i386 偶爾掉一兩個字元(40 行中 0~1 行受影響)。
2. **「一次整段送進去」仍然會掉字,而且這不是 WElyOS 能解決的。** Windows 版 QEMU 的標準輸入一次讀一個位元組,客體的接收緩衝區(16 個字元)滿了就把那個位元組丟掉,不會等。客體再快,也要等模擬器把 CPU 的執行權交給它才能取走字元。我試過在 kernel 裡加一層較大的緩衝區、每處理一個字元前先把硬體清空,量測結果沒有差別(0.3ms 時 14/40,和沒加時的 16/40 在誤差範圍內),所以沒有保留那段程式。在真正的硬體或 Linux 版的 QEMU 上沒有這個問題。
3. **i386 只改成中斷驅動是不夠的**,見下一節。

### 順帶修正:i386 的螢幕改用硬體捲動

i386 改成中斷驅動之後,掉字的情況沒有改善(2ms 時 6/40)。原因在輸出這一側:i386 的輸出同時送到序列埠與 VGA 螢幕,而螢幕每捲動一列,要把整個畫面往上搬(讀 1920 格、寫 2000 格)。在模擬器裡,每一次存取顯示記憶體都要離開客體、由模擬器處理,非常慢;而輸出期間中斷是關著的。捲動的那段時間裡序列埠的中斷進不來,進來的字元超過 16 個就遺失了。第 23 步已經注意到 i386 的螢幕輸出慢,當時只影響開機時間,現在它會造成輸入遺失,所以要處理。

做法是硬體捲動:VGA 的文字模式有 32KB 的顯示記憶體(16384 格),一個畫面只用其中 2000 格,而顯示卡有一個「顯示起點」暫存器(CRTC 的 0x0C / 0x0D),決定從第幾格開始畫。

```
顯示記憶體  [ ......|<------ 目前的畫面(2000 格)------>|...... ]
                     ^ 顯示起點                捲動一列 = 起點往後移 80 格
```

捲動一列只要把起點往後移 80 格,再清空新露出來的最後一列(寫 80 格)。起點移到記憶體尾端、放不下一整個畫面時,才把畫面搬回最前面一次,大約每捲動 180 列發生一次。起點和游標一樣,在每次 `kprintf` 結束時才寫進顯示卡。

| | 每捲動一列存取顯示記憶體的次數 |
|---|---|
| 原本(搬動整個畫面) | 約 3900 次(讀 1920、寫 2000) |
| 硬體捲動 | 80 次(另外每 180 列左右搬一次畫面) |

第 23 步評估過的另一個做法是「在記憶體裡保留一份畫面的副本」。那只能省掉讀取的一半,而且畫面更新會延遲;硬體捲動兩者都沒有,程式也更短。

驗證:自我測試會捲動超過 180 列(經過「搬回最前面」的路徑),結束後以 QEMU 的螢幕截圖確認畫面內容、游標位置都正確。

### 修正:在終端機直接執行時,Ctrl-C 到不了 WElyOS

寫這一步的互動測試時,第一次用「虛擬終端機」(pty)執行 `scripts/qemu.sh`,模擬使用者真的坐在終端機前面的情況,結果按 Ctrl-C 之後 QEMU 直接結束了。

原因:終端機自己會攔截 Ctrl-C,把它變成「終止前景程式」的訊號送給 QEMU,那個按鍵根本沒有進到 WElyOS。**第 24 步的 Ctrl-C 測試都是用管線送進字元的,不經過終端機,所以沒有發現。** 也就是說第 24 步完成時,使用者實際在終端機裡按 Ctrl-C 得到的是「QEMU 結束」,不是文件上寫的「終止執行中的命令」。這是那一步驗證的疏漏:測試的路徑和使用者實際操作的路徑不同。

修正(`scripts/qemu.sh`,只在標準輸入是終端機時生效):

| 按鍵 | 原本 | 現在 |
|---|---|---|
| Ctrl-C | 終端機攔下來,結束 QEMU | 原樣送進 WElyOS:終止前景的命令 |
| Ctrl-] | 沒有作用 | 結束 QEMU(WElyOS 沒有回應時的最後手段) |
| Ctrl-\、Ctrl-Z | 終端機攔下來(結束 / 暫停 QEMU) | 原樣送進 WElyOS(目前不處理) |

做法是把終端機的「中斷鍵」從 Ctrl-C 改設為 Ctrl-](`stty intr '^]'`),結束時還原。

配合這個改變,**互動使用時 shell 結束就關機**:在 shell 輸入 `exit`(或在空的提示符號下按 Ctrl-D)之後,kernel 印出 `powering off` 並結束模擬器。原本 shell 結束後 kernel 會永遠閒置,只能靠 Ctrl-C 結束 QEMU;現在 Ctrl-C 另有用途,正常的離開方式就是 `exit`。

之後互動相關的測試都用虛擬終端機執行(`script -qc "bash scripts/qemu.sh ..." /dev/null`),走和使用者相同的路徑。

### 背景執行

命令最後加上 `&`,shell 建立行程之後不等它結束,直接回到提示符號。一條在背景執行的管線稱為一個「工作」(job),shell 記住它的每個行程並給它一個小編號。

和前景的命令相比,差別只有三點,都建立在第 24 步的機制上:

| | 前景 | 背景 |
|---|---|---|
| 主控台的前景群組 | 設為這條管線 | 不設:Ctrl-C 不會終止它 |
| 標準輸入 | 繼承 shell 的(鍵盤) | `/dev/null`:不然它會和 shell 搶著讀鍵盤 |
| 誰來等它結束 | shell 立刻等 | shell 每次要讀下一個命令之前,看看有沒有背景的行程結束了 |

kernel 為此加了兩樣東西:

- **`waitpid` 的 `WNOHANG` 選項**:不等待。有子行程已經結束就回收並回傳它的編號;都還在執行就立刻回傳 0。shell 在每次印提示符號之前用它回收已經結束的背景行程,整個工作都結束時印出通知(`[1] done  sleep 400ms`,結束碼不是 0 時印 `[1] exit 137  ...`)。
- **`/dev/null`**:讀取永遠得到「結束」,寫入的內容直接丟掉。它和 `/dev/console` 一樣是裝置節點,只是兩個函式都不做事。

shell 的內建命令:

| 命令 | 作用 |
|---|---|
| `jobs` | 列出還在執行的背景工作 |
| `fg [編號]` | 把工作帶回前景並等它結束。此時它是前景群組,Ctrl-C 會終止它。沒有給編號時是最後建立的那個 |
| `wait` | 依序等所有背景工作結束(等待期間 Ctrl-C 會終止正在等的那個) |

`fg` 與 `wait` 是同一個動作:把那個工作設為前景群組、等它的每個行程結束、再把前景設回「沒有」。

shell 結束時還在執行的背景工作不會被終止:依第 24 步的規則,它們成為沒有父行程的行程,結束時由 kernel 自動回收。

**決策:工作的管理全部在 shell(使用者模式)裡。** kernel 只提供行程群組、前景群組與不等待的 `waitpid`,不知道「工作」這個概念。這和 Unix 相同,也讓 kernel 需要保護的狀態最少。

### ps:列出行程

新的系統呼叫 `procinfo(after, info)`:找出「編號大於 `after` 的行程中編號最小的那一個」,把它的資訊寫到 `info`;沒有更多時回傳 1。`ps` 從 0 開始,每次傳入上一次得到的編號,直到沒有為止。

**決策:一次只給一個,用「上一個的編號」接續。** 另外兩種做法與不採用的理由:

| 做法 | 問題 |
|---|---|
| 一次把所有行程複製到使用者給的陣列 | 要在持有排程器的鎖時複製一大塊資料,或先在 kernel 裡配置一塊暫存的記憶體;陣列不夠大時還要重來 |
| kernel 記住「這個呼叫者上次列到哪裡」 | kernel 多一份要維護、要在行程結束時清理的狀態 |
| 一次一個、以編號接續(採用) | 每次呼叫要走一遍行程串列,列出 N 個行程共走 N 次;N 很小,不是問題 |

kernel 不保存任何「列表進行中」的狀態,每次呼叫只在鎖內填一個固定大小的結構,解鎖之後才複製給使用者。列表期間有行程建立或結束,它們可能出現也可能不出現,但不會重複,kernel 也不會出錯。結構在填寫之前整個清為 0,欄位之間的空隙不會把 kernel 的資料帶到使用者模式。

回報的內容:行程編號、父行程、行程群組、執行緒數、狀態、累計的 CPU 時間、名稱。

- **狀態**:只要有一條執行緒正在執行或等著執行就是 `run`;全部在等待(輸入、時間、子行程)是 `sleep`;已經結束、父行程還沒來取結束碼是 `zombie`。
- **CPU 時間**:還活著的執行緒各自累計的 tick,加上已經結束的執行緒留下的(執行緒結束時把自己的數字加到行程上,否則多執行緒的程式會少算)。

### 驗證

使用者程式的測試(`UMODE_JOBS`):

| 項目 | 結果 |
|---|---|
| 沒有子行程時 `waitpid(-1, WNOHANG)` | `E_CHILD`,和一般的 `waitpid` 相同 |
| 子行程還在執行時 `WNOHANG` | 立刻回傳 0,`*status` 不變 |
| 不認得的選項 | `E_INVAL` |
| 列出行程 | 找得到自己(`run`、1 條執行緒、名稱 `init`)與子行程(父行程是自己);編號遞增;最後一個之後回傳 1 |
| 把 kernel 的位址當作 `info` | `E_FAULT` |
| 終止子行程後、`wait` 之前 | 列表中它是 `zombie` |
| `wait` 之後 | `WNOHANG` 收到它(結束碼 137);列表中不再有它 |
| `/dev/null` | 讀取得到 0(結束);寫入 4 個位元組回傳 4;之後讀取仍是 0 |

shell script(`/etc/test.sh`)新增:兩個背景工作(其中一個是管線)、`jobs`、`ps`、`wait`、背景執行不存在的命令、`fg` 不存在的工作。

互動測試(經由虛擬終端機,三個架構輸出相同):

```
$ sleep 30
^C                      ← Ctrl-C 送進 WElyOS,終止前景的命令
$ status
130
$ sleep 30 &
[1] started, pid 3  sleep 30
$ ^C                    ← 在提示符號下按:背景工作不受影響
jobs
[1] running  sleep 30
$ exit 6
shell exited with code 6
powering off
```

另外以模擬打字測試:背景工作結束時在下一個提示符號之前印出通知;`fg` 之後 Ctrl-C 終止該工作(`status` 130);`ps` 列出 shell、兩個背景行程與 `ps` 自己。

回歸(`scripts/regress.sh -r 4`):三個架構各 20 個情境共 60 個,全部符合預期;全部測試在 1、4、8 顆 CPU 下各 4 次,三個架構共 36 次全部通過。i386 舊 CPU 型號(`QEMU_CPU=qemu32`)與主機端的執行佇列測試也通過。

耗時(4 顆 CPU,各量 3 次):

| | i386 | RISC-V | AArch64 |
|---|---|---|---|
| 只開機(含模擬器啟動) | 0.37 秒 | 0.34 秒 | 0.25 秒 |
| 其中 kernel 本身 | 16~19 ms(上一步約 30 ms) | 13~17 ms | 8~11 ms |
| `selftest` | 1.07 秒 | 1.06 秒 | 0.99 秒 |

i386 的 kernel 開機時間少了將近一半,來自硬體捲動。

### 已知限制

- **一次貼上一大段文字,在 Windows 版的 QEMU 上仍然會掉字**(每個字元間隔 2ms 以上則不會),原因在模擬器,如上所述。
- **輸入的中斷只送給開機的那顆 CPU**。那顆 CPU 關著中斷的時間(持有 spinlock、輸出文字)會延後輸入的處理。
- **回顯在中斷處理程式裡輸出**,輸出期間其他中斷被擋住。序列埠很快,但 i386 的螢幕輸出仍比序列埠慢。
- **背景的命令照常輸出到螢幕**,可能穿插在正在打的字中間;沒有 Unix 那種「背景行程要讀寫終端機時先暫停」的機制(需要 Ctrl-Z 用的暫停訊號)。 **〔後續:第 26 步起,背景行程「讀」鍵盤時會被暫停(`SIGTTIN`);輸出仍不受限制。〕**
- **沒有 Ctrl-Z 與 `bg`**:執行中的前景命令不能中途丟到背景。 **〔後續:已解決:第 26 步。〕**
- **背景工作最多 8 個**;工作結束的通知只在下一次提示符號之前印出,不會即時出現。
- **shell 結束時不會警告還有背景工作**,它們會繼續執行到自己結束。 **〔後續:第 26 步起,暫停中的工作在 shell 結束時由 kernel 終止;執行中的仍繼續。〕**
- **`ps` 沒有選項**,也不顯示記憶體用量與個別的執行緒。 **〔後續:第 28 步的 `top` 顯示每個行程的記憶體用量與 CPU 使用率;個別的執行緒仍然看不到。〕**
- **在提示符號下按 Ctrl-C 之後不會重印提示符號**(shell 還在同一次讀取裡等輸入)。 **〔後續:已解決:第 26 步。〕**
- **i386 仍然只接受序列埠的輸入**,QEMU 圖形視窗裡的鍵盤不能用。
- **GDB 除錯時 shell 結束會關機**:想在 shell 結束後檢查 kernel 的狀態,要先設中斷點。

---

## 第 26 步:工作控制補完整(暫停、繼續、Ctrl-Z、bg)

### 目標

第 25 步的 shell 可以把命令丟到背景(`&`)、列出來(`jobs`)、帶回前景(`fg`),但少了另一半:**已經在前景執行的命令,沒辦法中途放到背景**。這一步補上:

- **Ctrl-Z**:暫停前景的命令,回到提示符號
- **`bg`**:讓暫停中的工作在背景繼續;**`fg`** 也能讓暫停中的工作回到前景繼續
- **`kill -STOP` / `kill -CONT`**:從命令列暫停與繼續任何行程
- 背景的工作如果去讀鍵盤,會被自動暫停,不再和 shell 搶輸入
- 在提示符號下按 Ctrl-C,會重新印出提示符號

### 暫停是什麼

暫停(stop)= 行程還在,記憶體與開啟的檔案都保留,只是不再執行;之後可以從停下的地方繼續(continue)。它和「睡眠」的差別是:睡眠是行程自己要求的(等時間、等輸入),暫停是外面加給它的,程式自己不知道。

新增四個訊號(signal),編號沿用 Unix:

| 訊號 | 編號 | 效果 | 誰送的 |
|---|---|---|---|
| `SIGCONT`(continue) | 18 | 繼續 | shell 的 `fg`、`bg`;`kill -CONT` |
| `SIGSTOP`(stop) | 19 | 暫停 | `kill -STOP` |
| `SIGTSTP`(terminal stop) | 20 | 暫停 | 主控台收到 Ctrl-Z |
| `SIGTTIN`(terminal input) | 21 | 暫停 | 主控台:背景的行程想讀鍵盤 |

三個暫停訊號的效果相同,編號不同只是讓 shell 能說出原因(例如 `stopped (wants input; use fg)`)。

### kernel 這一側:沿用「終止」的檢查點

終止行程的做法(第 15 步)是:把它的執行緒標記為應終止,每條執行緒在**要從 kernel 返回使用者模式之前**(`user_return_check`)自己結束。暫停用同一個檢查點:

```
user_return_check():
    行程被暫停了?  ->  睡在 p->stop_wq 上,直到 SIGCONT(或被終止)
    被終止了?      ->  結束這條執行緒
```

- **正在使用者模式執行的執行緒**:最晚在下一個計時器中斷(1ms)進入 kernel,返回前停下。
- **正睡在 kernel 裡的執行緒**(等時間到、等管線…):**不打斷它**。它等的事情照常進行,醒來要回使用者模式時才停下。

第二點是刻意的選擇。另一種做法是把睡著的執行緒叫醒、讓系統呼叫提早返回(Unix 的 `EINTR`),但那樣每一種等待、每一個使用者程式都得處理「被打斷」;這裡的做法讓程式完全看不出自己被暫停過,也不必改動任何既有的等待。代價是:`sleep 30` 被暫停 10 秒再繼續,仍然在原本的 30 秒到期時結束(暫停期間時間照算)。

行程一被標記為暫停,父行程**立刻**得到通知,不必等所有執行緒真的停下 — 它們已經不可能再執行任何使用者模式的程式碼了。

檢查點在每次返回使用者模式都會經過,所以先不上鎖看一眼 `p->stopped`,絕大多數時候是 0,不增加任何鎖的負擔;真的被暫停時才上鎖睡下去。

**終止類的訊號對暫停中的行程立刻生效**(不必先讓它繼續):`sched_kill_group` 本來就會把睡著的執行緒叫醒,睡在 `stop_wq` 上的也一樣。Unix 是把 `SIGTERM` 留到繼續之後才處理,這裡沒有「待處理的訊號」這個概念,也比較直覺。

### 父行程怎麼知道:waitpid 的 WUNTRACED

`waitpid` 原本只回報「子行程結束了」。新增選項 `WUNTRACED`(名稱沿用 Unix):子行程**被暫停**也算一個事件 — 回傳它的 pid,但不回收它。同一次暫停只回報一次。

`*status` 要能分辨「結束碼」與「被暫停」。結束碼是完整的 `int`(例如出錯被終止是 -1),沒有空的位元可以借用,所以劃出一小段數值:

```
WSTOPPED_BASE | 訊號編號      (0x80000000 ~ 0x800000FF)
WIFSTOPPED(status)            是不是落在這一段
WSTOPSIG(status)              取出訊號編號
```

程式自己若以這一段的數值呼叫 `exit`,kernel 只留下最低 8 個位元,所以不可能有行程「假裝」被暫停。

### 主控台:Ctrl-Z,以及誰可以讀鍵盤

`console.c` 收到 Ctrl-Z(0x1A)時,對前景群組送 `SIGTSTP`,和 Ctrl-C 的處理對稱。沒有前景命令(shell 在提示符號下)時不做任何事。

暫停帶來一個新問題:**鍵盤只有一個**。`cat` 等輸入時被 Ctrl-Z 暫停,回到 shell — 此時 `cat` 和 shell 都睡在「等鍵盤」上,打的一行會被誰拿走?規則和 Unix 的終端機相同:

- 有前景群組(shell 正在等一個命令):只有那一組可以讀
- 沒有前景群組(提示符號下):只有 shell 自己那一組可以讀(`tcsetfg` 的呼叫者被記為主控台的管理者)
- 其他行程一讀就被暫停(`SIGTTIN`),等使用者用 `fg` 把它帶到前景才繼續
- 被暫停的行程不拿輸入;繼續時被叫醒重新檢查
- 沒有任何 shell 在管理時(管理者為 0 或已經結束)不限制 — 自我測試的程式直接讀鍵盤不受影響

所以 `cat`、Ctrl-Z、`bg` 的結果是:`cat` 在背景一繼續就想讀鍵盤,馬上又被暫停,shell 在下一個提示符號之前印出 `[1] stopped (wants input; use fg)`。

### 踩坑:暫停在讀取裡的行程,握著共用的鎖

第一次測試 `cat`、Ctrl-Z 之後,shell 印出提示符號就再也讀不到任何輸入。

原因在 VFS:`vfs_read` 會鎖住「這一次開啟」(`struct file`)來保護讀寫位置。shell 和它啟動的命令**共用同一次開啟**(標準輸入是繼承來的),`cat` 睡在主控台的讀取裡時握著這把鎖;以前它一定會在前景讀完或被終止,現在它可以被暫停在那裡,shell 就永遠拿不到鎖。

修正:裝置與管線沒有讀寫位置,`vfs_read` 對它們不再上這把鎖(寫入本來就沒上)。這也讓「兩個行程同時讀同一條管線」不再互相卡住。

教訓:**一個等待可以持續任意久的時候,等待期間握著的每一把鎖都要重新檢查。** 加入暫停,等於讓所有「會等的地方」都可能等上無限久。

### 孤兒:沒有人能讓它繼續的暫停行程

暫停中的行程只有 shell(它的父行程)會讓它繼續。shell 結束了,它就永遠停在那裡,佔著記憶體。所以父行程結束時,它暫停中的子行程由 kernel 直接終止(Unix 對失去管理者的暫停工作也是送 `SIGHUP` 終止)。

### shell

工作(job)多了一個狀態「暫停中」。前景與背景的命令現在走同一條路:都先整理成一個工作,差別只在要不要等它。

| 動作 | 做法 |
|---|---|
| 等前景的工作 | `waitpid(pid, &st, WUNTRACED)`;回報暫停就停止等待 |
| 前景的命令被 Ctrl-Z 暫停 | 登記進工作表,印出 `[n] stopped  命令`,結束碼 148(128 + 20) |
| `fg [n]` | 先設為前景群組,再對整組送 `SIGCONT`,然後等它 |
| `bg [n]` | 對整組送 `SIGCONT`,不等它;沒有給編號時選編號最大的暫停中工作 |
| `jobs` | 每個工作顯示 `running` 或 `stopped` |
| `wait` | 只等執行中的工作(暫停中的不會自己結束) |
| 每次提示符號之前 | `waitpid(-1, WNOHANG \| WUNTRACED)`:回收結束的、通知被暫停的 |

`fg` 的順序(先設前景、再繼續)有意義:工作也許正等著讀鍵盤,先繼續的話它會立刻又被 `SIGTTIN` 暫停。

工作表滿了(8 個)而前景命令又被 Ctrl-Z 暫停時,shell 沒有地方記住它;這時讓它繼續並印出訊息,而不是留下一個沒有人能繼續的暫停行程。

`kill` 可以用名稱指定訊號(`-STOP`、`-CONT`、`-TSTP`、`-INT`、`-KILL`、`-TERM`),pid 寫成負數表示整個行程群組。`ps` 的狀態多了 `stop`。

### 在提示符號下按 Ctrl-C

以前按了之後畫面上只有 `^C`,游標停在空白行(shell 還在同一次讀取裡)。現在主控台在沒有前景命令時,會交出一個空行,shell 讀到空行就重新印出提示符號。

### 驗證

使用者程式的測試(`UMODE_STOP`,自我測試的一部分):

| 項目 | 結果 |
|---|---|
| 執行中的子行程,`waitpid(WNOHANG \| WUNTRACED)` | 0(沒有事件) |
| `SIGSTOP` 之後 `waitpid(WUNTRACED)` | 回傳它的 pid,`WIFSTOPPED`,訊號 19;再問一次得到 0 |
| 暫停中的空轉行程 | 狀態 `stop`;12ms 內用掉的 CPU 時間完全沒有增加 |
| 對已暫停的行程再送 `SIGSTOP` | 沒有作用,不重複回報 |
| `SIGCONT` 之後 | 12ms 內 CPU 時間增加 |
| `SIGTSTP` 暫停後直接 `SIGKILL` | 立刻結束,結束碼 137 |
| 沒有 `WUNTRACED` 的 `waitpid` | 不因暫停而返回;繼續之後才收到結束碼 5 |
| 以 `0x80000014` 結束的行程 | 結束碼 20,`WIFSTOPPED` 為否 |
| 暫停中的孫行程,其父行程結束 | 200ms 內從行程列表消失 |
| 不存在的訊號編號 | `E_INVAL` |

互動測試(經由虛擬終端機模擬按鍵,三個架構除了 `ps` 的時間欄以外輸出相同):

```
$ sleep 30
^Z
[1] stopped  sleep 30
$ ps
  PID  PPID  PGID  THR  STATE     TIME  NAME
    1     0     1    1  sleep      10  sh
    2     1     2    1  stop        0  sleep
    3     1     3    1  run         4  ps
$ bg
[1] continued  sleep 30
$ jobs
[1] running  sleep 30
$ fg
[1] foreground  sleep 30
^Z
[1] stopped  sleep 30
$ fg
[1] foreground  sleep 30
^C
$ status
130
$ cat
^Z
[1] stopped  cat
$ hello                                  ← 這一行由 shell 讀到,不是 cat
sh: hello: command not found
$ bg
[1] continued  cat
$                                        ← 按 Enter
[1] stopped (wants input; use fg)  cat   ← cat 在背景想讀鍵盤,被暫停
$ fg
[1] foreground  cat
abc
abc                                      ← 回到前景,cat 讀到輸入
$ echo x | cat | sleep 20
^Z
[1] stopped  echo x | cat | sleep 20     ← 整條管線一起暫停
$ fg
[1] foreground  echo x | cat | sleep 20
^C
$ half^C                                 ← 提示符號下按 Ctrl-C
$ exit                                   ← 重新印出了提示符號
```

回歸(`scripts/regress.sh -r 2`):三個架構各 20 個情境共 60 個,全部符合預期;全部測試在 1、4、8 顆 CPU 下各 2 次,三個架構共 18 次全部通過。

### 已知限制

- **行程不能攔截或忽略訊號**(沒有訊號處理函式)。所以 shell 不能在收到 Ctrl-C 時做自己的事,`SIGTSTP` 與 `SIGSTOP` 也沒有差別。 **〔後續:已解決:第 31 步加入訊號處理函式與忽略;`SIGTSTP` 可以攔截,`SIGSTOP` 不行。〕**
- **暫停期間時間照算**:`sleep 30` 暫停再繼續,仍在原本的時間點結束。正睡在 kernel 裡的執行緒也要等它醒來才算真的停下(但在那之前它本來就沒有在執行)。
- **終止類的訊號不會等行程繼續才生效**,和 Unix 不同。
- **背景工作的輸出不受限制**(Unix 可以設定成寫終端機時也暫停,即 `SIGTTOU`)。
- **沒有「目前的工作」記號**(Unix 的 `%+`、`%-`):`fg`、`bg` 不給編號時選編號最大的。
- **工作被暫停的通知只在下一次提示符號之前印出**;背景工作最多 8 個。
- **Ctrl-Z 與設定前景之間有極短的空檔**(和 Ctrl-C 相同,見第 24 步)。
- **在 script 裡無法取得背景行程的 pid**(沒有 `$!` 這類變數),所以 `/etc/test.sh` 沒有測到 `kill -STOP`;這部分由 `UMODE_STOP` 與互動測試涵蓋。

---

## 第 27 步:磁碟驅動與磁碟上的檔案系統

### 目標

到上一步為止,所有檔案都在記憶體裡(ramfs),關機就消失。這一步讓系統有**關機後還在的儲存空間**:

- 三個架構各有一個磁碟驅動程式
- 一個存在磁碟上的檔案系統 WFS,掛載在 `/disk`
- VFS 支援「掛載」:目錄樹的一部分來自另一個檔案系統
- 沒有正常關機時,下次開機自動檢查並修復
- 新的命令:`cp`、`df`、`sync`

使用起來和記憶體裡的檔案沒有差別 — `echo hi > /disk/a`、`cat /disk/a`、`/disk/程式` 都照常,只是內容跨開機保留。

### 整體結構

```
系統呼叫 (proc.c)
     |
VFS (vfs.c)          路徑解析走到掛載點時,換到另一個檔案系統
     |
  +--+-----------+
ramfs           WFS (wfs.c)        檔案系統:把「檔案」變成「第幾個區塊」
(記憶體)         |
               區塊快取            記憶體裡留著最近用過的 64 個區塊
                 |
               struct blkdev      區塊裝置(block device)介面:讀寫第 N 個磁區
                 |
        +--------+---------+
      ATA PIO           virtio-blk
      (i386)            (RISC-V、AArch64)
```

每一層只認得下一層的介面。換一種磁碟只要換最底下那一塊;換一種檔案系統只要提供同一組 `inode_ops`。

### 區塊裝置介面

`include/blk.h`:一個磁碟就是「容量」加上兩個函式 — 讀寫若干個磁區(sector,512 位元組)、把裝置暫存的資料寫到媒體上(flush)。驅動程式都是**輪詢**(polling):送出要求後反覆檢查是否完成,不使用中斷。

為什麼不用中斷:模擬器裡的磁碟要求幾乎立刻完成,等中斷再切換執行緒反而比較慢;而輪詢不需要再接一條中斷線、不需要處理「中斷與等待的競爭」,程式少、不容易錯。每個等待都有上限(ATA 以次數、virtio 以 5 秒),裝置沒有回應時回報 `E_IO` 而不是永遠卡住。代價是等待期間那顆 CPU 在空轉;真實硬體上磁碟慢得多,那時才值得改成中斷驅動(介面不必變)。

### i386:ATA PIO

ATA(AT Attachment)是 PC 最傳統的硬碟介面;PIO(Programmed I/O)是其中最簡單的傳輸方式:CPU 自己用 `in` / `out` 指令把資料一個字一個字搬進搬出。真實硬體上它很慢(現代系統用 DMA(Direct Memory Access)或 AHCI(Advanced Host Controller Interface)),但不需要 PCI(Peripheral Component Interconnect)列舉也不需要中斷,是最少的程式碼。

- 暫存器在 I/O port `0x1F0`~`0x1F7`;磁區以 LBA(Logical Block Addressing)編號,28 位元
- 開機時送 `IDENTIFY`(`0xEC`)確認有磁碟並取得容量
- 讀:命令 `0x20`,等 `DRQ`,`rep insw` 讀 256 個字;寫:命令 `0x30`,`rep outsw`
- `rep insw` / `rep outsw` 讓 CPU 自己重複 256 次,比在 C 裡跑迴圈快(模擬器只需處理一次)

### RISC-V 與 AArch64:virtio-blk

virtio 是專為虛擬機器設計的裝置規格:不模擬真實的硬體,客體與模擬器用一塊共用的記憶體直接交換要求。QEMU 的 `virt` 機器把 virtio 裝置放在固定的記憶體位址(virtio-mmio;MMIO 是 Memory-Mapped I/O):RISC-V 在 `0x10001000` 起 8 個位置,AArch64 在 `0x0a000000` 起 32 個位置。驅動程式逐一檢查,找到裝置編號 2(區塊裝置)的那一個。兩個架構共用同一份程式(`kernel/virtio_blk.c`),只有位址不同。

交換要求的地方稱為 virtqueue,由三部分組成:描述表(每格描述一塊緩衝區)、可用環(我們放進去的要求)、已用環(裝置做完的)。一個磁碟要求是三塊緩衝區串在一起:

```
[標頭:讀或寫、第幾個磁區]  ->  [資料]  ->  [狀態:1 個位元組]
```

這裡一次只送一個要求,佇列只有 8 格,整個佇列加上標頭放在同一頁。

QEMU 的 virtio-mmio 預設使用舊版(legacy)介面,`scripts/qemu.sh` 以 `-global virtio-mmio.force-legacy=false` 選用現行版本;驅動程式遇到舊版會印出該怎麼做的訊息並略過。

### WFS:磁碟上的配置

磁碟切成 4KB 的區塊(block,8 個磁區):

| 區塊 | 內容 |
|---|---|
| 0 | superblock:整個檔案系統的描述(大小、各區的位置、是否正常卸載、掛載次數) |
| 1 起 | 區塊位元圖(bitmap):每個區塊一個位元,1 = 使用中 |
| 之後 | inode 表:每個 inode 64 位元組,一個區塊放 64 個;編號 1 是根目錄 |
| 其餘 | 資料區:檔案與目錄的內容、間接區塊 |

inode 記錄「內容放在哪些區塊」:前 10 個區塊的編號直接記在 inode 裡;第 11 個起記在一個**間接區塊**(indirect block)裡,它裝著 1024 個編號。所以單一檔案最大 (10 + 1024) × 4KB ≈ 4MB。小檔案(40KB 以內)不需要間接區塊,多一次磁碟讀取的代價只由大檔案負擔。

目錄的內容是一連串 64 位元組的紀錄 `{ inode 編號, 名稱 }`,和檔案一樣存在區塊裡。

16MB 的磁碟:1 個 superblock、1 個位元圖、8 個 inode 表(512 個 inode),其餘 4086 個區塊放資料。

**為什麼自己設計而不用現成的格式**(例如 FAT):FAT 沒有 inode,名稱和檔案本身綁在一起,「刪除時還有人開著」這類 VFS 已經定好的規則很難對上;ext2 則大得多。自己的格式可以直接對應 VFS 的概念,程式最少。代價是主機上的工具看不懂這個映像檔。

### 區塊快取

記憶體裡留著最近用過的 64 個區塊。修改先改快取裡的那一份(標記為髒,dirty),之後才寫回:

- 檔案關閉(寫入過的)、建立、刪除、清空之後
- `sync` 系統呼叫、關機
- 快取滿了要把髒的區塊讓出來時

寫入本身(`write`)不立刻碰磁碟:使用者程式一次寫 256 位元組,寫一個 4KB 的區塊要呼叫 16 次,每次都寫磁碟會慢 16 倍。規則是**關閉檔案之後,資料就在磁碟上了**。

自我測試中寫入 100000 位元組、再讀回驗證,實際的磁碟讀取是 0 次(全部來自快取)。

### 當機之後:寫回的順序與掛載時的檢查

一個操作要改好幾個區塊(位元圖、inode、目錄)。寫到一半斷電,磁碟上的各部分會對不上。兩層對策:

1. **寫回的順序**:區塊編號由大到小。資料區在後面、位元圖與 superblock 在最前面,所以資料先寫、描述資料的東西後寫。中斷時最可能的結果是「多佔了沒有人用的區塊」(無害),而不是「inode 指向還沒寫入的資料」。

2. **正常卸載的記號**:superblock 裡的 `clean`,掛載時清掉、關機時設回來。掛載時發現它是清掉的,就做一次完整的檢查與修復(Unix 上這類工具稱為 fsck,file system check):

| 發現的問題 | 處理 |
|---|---|
| 目錄裡的紀錄指向不存在的 inode | 清掉那筆紀錄 |
| 從根目錄走不到的 inode(沒有名稱、刪到一半) | 釋放 |
| inode 記錄的名稱數和實際不符 | 改成實際的數目 |
| 區塊編號超出範圍,或兩個檔案指向同一個區塊 | 拿掉(那一段變成沒有配置) |
| 位元圖和實際使用的區塊不符 | 依實際的重新產生 |

檢查以「從根目錄走得到的東西」為準。16MB 的磁碟上只要讀 9 個區塊加上各個目錄,開機幾乎沒有多花時間。

**沒有做日誌(journaling)**:日誌能讓每個操作「要嘛全部完成、要嘛完全沒發生」,但程式多很多。這裡的做法保證的是**檔案系統的結構永遠修得回來**,不保證當機當下正在寫的那個檔案內容完整。

### 不信任磁碟上的內容

磁碟的內容可能損壞,也可能被人刻意改過。原則和系統呼叫的參數一樣:**用之前都檢查**。

- superblock 各區的位置,必須和「依大小重新算出來的」完全一致,否則不掛載
- 每個區塊編號使用前檢查範圍;不合理的當成「沒有配置」(讀到 0)
- 目錄紀錄裡的 inode 編號檢查範圍;名稱強制補上結尾
- inode 記錄的大小超過上限時截到上限
- **磁碟開頭不是全 0、也不是 WFS 時,不格式化也不掛載**,只印出訊息 — 那可能是別人的資料。只有全新(全 0)的磁碟才會自動建立檔案系統

### 掛載

VFS 原本只有一棵樹(ramfs)。現在 `struct inode` 多了兩個欄位:

- `mounted`:這個目錄是掛載點,路徑走到這裡時改走另一個檔案系統的根目錄
- `covered`:這是一個被掛載的根目錄,它蓋住的是哪個目錄(`..` 要從那裡往上)

`walk` 多的只有兩處判斷,所以 `/disk/../etc/motd` 這種跨過掛載點來回的路徑也走得通。掛載點不可以刪除(`E_BUSY`)。

`readdir` 的介面順便改了:原本是「第 idx 筆」,磁碟上的目錄有空格,每次都要從頭數;改成由檔案系統自己維護位置(`*pos`),VFS 只負責保存。

### 記憶體:全部在掛載時配置

快取的 64 頁與 inode 表在掛載時一次配置,之後的任何操作都不再配置記憶體。好處:

- 不會因為記憶體不足而做到一半失敗(留下不一致的磁碟)
- 自我測試的記憶體核對不必為磁碟另外記帳

### 上鎖

WFS 用一把大鎖保護所有東西,每個操作從頭鎖到尾。磁碟本來就一次只做一件事,細分鎖得不到什麼;一把鎖則不可能出現「兩個操作同時配置到同一個區塊」這類錯誤。順序固定為 VFS 的 inode 鎖 → WFS 的鎖 → 驅動程式的鎖。

### 量測與調整:flush 很貴

第一版在每次寫回之後都要求磁碟 flush(把裝置自己的暫存寫到媒體上)。在模擬器裡這是對主機的映像檔做一次同步。改成只在 `sync` 與關機時做:平常的寫回只把資料交給磁碟。差別只在「整台主機突然斷電」時最後幾個操作可能還沒落到媒體上,檔案系統的結構仍然由寫回順序與掛載檢查保證。

(實際量測時,兩種做法在這台機器上沒有可見的差別 — 當時觀察到的變慢後來確認是主機一時忙碌。保留這個改動是因為理由本身成立:真實的磁碟上 flush 一次是毫秒等級。)

### 新的系統呼叫與命令

| | |
|---|---|
| `sync()` | 把所有掛載的檔案系統寫回磁碟(含 flush) |
| `statfs(path, info)` | path 所在的檔案系統的用量:區塊大小、總數、空閒數、還能建立幾個檔案、種類 |
| `cp 來源 目的` | 複製檔案 |
| `df [路徑]` | 顯示用量;不給路徑時顯示 `/` 與 `/disk` |
| `sync` | 呼叫 `sync()` |

`scripts/qemu.sh` 預設接上 `build/<架構>/disk.img`(不存在時建立 16MB 的空白檔);`QEMU_DISK=路徑` 指定別的映像檔,`QEMU_DISK=none` 不接磁碟。

### 驗證

kernel 內的測試(`disk_demo`,自我測試的一部分,全部在 `/disk/selftest` 底下進行):

| 項目 | 結果 |
|---|---|
| 建立目錄;重複建立 | 成功;`E_EXIST` |
| `/disk/selftest/..`、`/disk/../etc/motd` | 走得通(跨過掛載點來回) |
| 刪除 `/disk`;在磁碟上建立裝置節點 | `E_BUSY`;`E_INVAL` |
| 寫入 100000 位元組再讀回 | 內容相同;用掉 27 個區塊(25 資料 + 1 間接 + 1 目錄) |
| 刪除非空的目錄 | `E_NOTEMPTY` |
| 換成 5000 位元組的內容 | 多出來的區塊歸還(剩 3 個) |
| 附加 4 個位元組 | 大小 5004,內容在最後 |
| 開著的時候刪除 | 名稱立刻消失;仍可讀取;關閉後區塊與 inode 才歸還 |
| 4 條執行緒 × 6 個檔案同時建立、寫入、讀回、刪除 | 全部正確 |
| 全部刪除後 | 空閒的區塊與 inode 數回到測試之前 |
| 再做一次完整檢查 | 0 個問題 |

使用者程式的測試(`UMODE_DISK`):`statfs` 回報 `wfs` / `ramfs`;把 `/bin/echo` 複製到磁碟上並從磁碟執行;空閒區塊數的增減符合檔案大小;`sync`;刪除後空間歸還;把 kernel 位址當作 `info` 得到 `E_FAULT`。

跨開機的測試(回歸測試以同一個映像檔連續開機三次):

```
disktest=1   disk: ..., new file system created and mounted on /disk (mount #1, 16344 KB free)
             disktest 1: wrote /disk/persist (9000 bytes)
disktest=2   disk: ..., mounted on /disk (mount #2, 16328 KB free)
             disktest 2: /disk/persist survived the reboot; damaged the file system on purpose,
                         exiting without a clean shutdown
disktest=3   disk: the file system was not shut down cleanly; checked it and repaired 4 problem(s)
             disktest 3: /disk/persist is intact; a second check found nothing
```

第 2 階段故意製造四種損壞(佔用沒有人用的區塊、沒有名稱的 inode、指向不存在 inode 的目錄紀錄、錯誤的名稱數),不正常關機;第 3 階段確認四個都被修復、原本的檔案完好。

互動測試(經由虛擬終端機,兩次開機,三個架構除了 `df` 的記憶體數字以外輸出相同):

```
$ df
TYPE    SIZE(KB) USED(KB) FREE(KB)  FILES  PATH
ramfs    131072     3772   127300      -  /
wfs       16344        0    16344    510  /disk
$ mkdir /disk/notes
$ echo hello from boot one > /disk/notes/a.txt
$ cp /bin/echo /disk/echo
$ /disk/echo ran from disk
ran from disk
$ exit
powering off
                                         ← 重新開機
disk: virtio-blk, 16 MB, mounted on /disk (mount #2, 16324 KB free)
$ ls /disk
notes/  echo
$ cat /disk/notes/a.txt
hello from boot one
$ /disk/echo still here
still here
```

回歸(`scripts/regress.sh -r 2`):三個架構各 20 個情境共 60 個,全部符合預期;磁碟跨開機測試各 3 個階段共 9 個,全部通過;全部測試在 1、4、8 顆 CPU 下各 2 次,三個架構共 18 次全部通過。另外確認:磁碟開頭放了別的內容時,kernel 印出訊息、不掛載,映像檔的雜湊值前後相同(完全沒有被寫入);128KB 的磁碟回報太小。

耗時(4 顆 CPU):掛載讓只開機的時間多了約 10~20 毫秒;自我測試中磁碟的部分在客體內約佔 25(AArch64)~ 95(i386,PIO 較慢)個 tick。含模擬器啟動的總時間:只開機 i386 約 0.36 秒、RISC-V 約 0.29 秒、AArch64 約 0.24 秒;`selftest` 約 1.1~1.5 秒。(量測期間主機有時會讓模擬器的啟動多花將近 1 秒,不接磁碟時也一樣,客體內的 tick 數不變,與 kernel 無關。)

### 已知限制

- **沒有日誌**:當機時正在寫的檔案內容可能不完整(結構會被修復)。
- **單一檔案最大約 4MB**(沒有二級間接區塊);**最多 4096 個 inode**;只使用磁碟的前 4GB。
- **只能掛載一個磁碟,固定在 `/disk`**;沒有 `mount` / `umount` 命令,也不能卸載。
- **根目錄仍然是記憶體檔案系統**:`/bin` 的程式每次開機從 kernel 內嵌的檔案包解開。要常駐的程式得自己 `cp` 到 `/disk`。
- **磁碟驅動是輪詢的**,等待期間佔著一顆 CPU;一次只處理一個要求。
- **i386 只看第一組 IDE 的第一顆磁碟**;virtio 只找第一個區塊裝置;不支援舊版(legacy)virtio-mmio。
- **平常的寫回不要求磁碟 flush**(只有 `sync` 與關機時做),如上所述。
- **寫入中但還沒關閉的檔案**,資料只在快取裡,要等關閉、`sync`、或快取滿了才到磁碟;沒有定時寫回。
- **磁碟上的程式執行時是整份讀進記憶體**,不像 `/bin` 的程式可以和檔案共用頁。
- **目錄不會縮小**:刪除檔案後,目錄佔用的區塊留著重複使用。
- **沒有硬連結、符號連結、權限、時間戳記**;inode 表以線性掃描找空位。 **〔後續:第 29 步加入硬連結,第 32 步加入修改時間;符號連結與權限仍然沒有。〕**
- **不檢查映像檔是否同時被另一個 QEMU 使用**:兩個模擬器同時用同一個映像檔會互相破壞。
- **讀寫錯誤**只回報 `E_IO`,不會重試,也不會把檔案系統轉成唯讀。 **〔後續:第 33 步起,寫入失敗會把檔案系統轉為唯讀,並加入錯誤注入的測試。〕**

---

## 第 28 步:top(類似 htop 的即時系統狀態)

### 目標

`ps` 只能看到某一瞬間有哪些行程。這一步加入 `top`:全螢幕、每秒更新,顯示每顆 CPU 的使用率、記憶體與磁碟的用量、每個行程用了多少 CPU 與記憶體,並且可以直接在畫面上選擇行程來暫停、繼續或終止。

```
WElyOS top  up 0:00:04  4 cpus  4 processes  6 threads  1 switches/s
CPU0 [                        ]   0%    CPU1 [||||||||||||||||||||||||] 100%
CPU2 [                        ]   0%    CPU3 [                        ]   0%
Mem  [                        ]   3% 3960K/128M
Disk [                        ]   0% 0K/15M  /disk

    PID  PPID THR STATE   CPU%  MEM(KB)  TIME(ms)  NAME
>     2     1   1 run      100       36      2369  init
      4     1   2 run        0       60        14  top
      1     0   1 sleep      0       28         4  sh
      3     1   1 sleep      0       24         0  sleep

q quit  up/down select  x kill  s stop/continue  sort: c cpu  m memory  p pid
```

| 按鍵 | 作用 |
|---|---|
| `q` | 離開 |
| 上 / 下(或 `k` / `j`) | 選擇行程 |
| `x` 再按 `y` | 終止選到的行程(`SIGTERM`) |
| `s` | 暫停 / 繼續選到的行程 |
| `c` `m` `p` | 依 CPU% / 記憶體 / PID 排序 |

`top -b` 只印一次純文字(不用控制序列),可以接在管線或 script 裡。

它需要 kernel 提供四樣原本沒有的東西,以下分別說明。

### 1. 系統的統計:sysinfo 系統呼叫

`SYS_sysinfo` 一次回報整個系統的狀態:運作時間、CPU 數、行程與執行緒數、記憶體總量與空閒量、累計的執行緒切換次數,以及**每顆 CPU 忙碌與閒置的 tick 數**。

後者是新加的記帳:`struct cpu` 多了 `ticks_busy` 與 `ticks_idle`,計時器中斷(`sched_tick`)來的時候,看這顆 CPU 當下是不是在跑 idle 執行緒,把其中一個加一。每顆 CPU 只寫自己的計數器,不需要上鎖;讀的一方不上鎖直接讀,差一兩個 tick 無妨。成本是每個 tick 多一次加法。

kernel 只提供**累計值**,「使用率」由使用者程式自己算:

```
這一秒的使用率 = (這次的 busy - 上次的 busy) / (busy 與 idle 的增加量總和)
```

這樣 kernel 不必決定「多久算一次」「誰在看」,也不必替每個觀察者保留狀態。`ps` 的 `TIME`、這裡的 CPU% 都是同一個做法。

### 2. 行程的記憶體用量

`struct vm_space` 多了一個計數 `pages`:這個位址空間目前對映了幾個使用者的頁。在對映新的頁(`vm_space_map`、`vm_space_map_new`)與 `fork` 複製對映時加一;位址空間沒有「取消對映單一頁」的操作,所以只增不減,直到整個被釋放。`procinfo` 新增欄位 `mem_kb` 回報它。

這個數字包含**和別人共用的頁**(程式碼、`fork` 之後還沒被寫入的頁),所以所有行程的 `MEM` 加起來會超過實際用掉的記憶體。這和 Unix 的 RSS(Resident Set Size)是同一種算法。

**踩坑(審視程式時發現,尚未造成問題):讀取別的行程的位址空間。** `procinfo` 是「行程 A 讀取行程 B 的資料」。B 的位址空間可能正在被釋放:

- B 結束時:先在排程器的鎖內把執行緒數減到 0,然後才在鎖外釋放位址空間 → 在鎖內看到執行緒數大於 0,位址空間就一定還在
- B 執行 `exec` 時:原本是不上鎖直接換掉 `p->space`、然後釋放舊的 → A 可能剛讀到舊指標,B 就把它釋放了

第二種情況改成在排程器的鎖內更換指標。A 是在同一把鎖內讀取並使用它,所以 B 的更換(以及之後的釋放)一定排在 A 用完之後。

### 3. 主控台的 raw 模式

一般的程式要的是「一整行」(第 21 步的行規程):kernel 負責編輯與回顯,按下 Enter 才交出去。全螢幕的程式要的相反:**每個按鍵立刻送到,不要回顯**。

新的系統呼叫 `tcsetmode(mode)`:

| 模式 | 效果 |
|---|---|
| `CON_RAW` | 每個字元原樣、立刻交給讀取者,不做行編輯(退格、Ctrl-D 都只是普通的字元) |
| `CON_NOECHO` | 不把打的字顯示出來 |

幾個為了安全而做的決定:

- **Ctrl-C 與 Ctrl-Z 在 raw 模式下照常作用。** 全螢幕的程式卡住時,使用者仍然有辦法終止它。
- **只有目前可以讀鍵盤的行程(前景)可以改模式**,背景的行程不能把別人的終端機改掉。
- **`tcsetfg` 會把模式設回一般。** shell 每次執行命令前後都會呼叫它,所以不論上一個程式是正常結束、被 Ctrl-C 終止、還是被 Ctrl-Z 暫停,回到提示符號時輸入一定是正常的 — 不必依賴程式自己記得還原(被終止的程式根本沒有機會還原)。
- **離開 raw 模式時,還沒被讀走的按鍵丟掉。** 那是打給上一個程式的,不該變成下一個命令的開頭。

因為 shell 會把模式改回去,`top` 被 Ctrl-Z 暫停再用 `fg` 帶回來之後,它的更新執行緒每一輪都重新要求一次 raw 模式。

### 4. 螢幕的控制序列

全螢幕的程式靠夾在文字裡的命令移動游標、清除畫面、換顏色:以 ESC(0x1B)加 `[` 開頭,接著是數字,最後一個字母決定動作。這是終端機通用的寫法(ANSI escape code)。

- **序列埠**:原樣送出去,另一端的終端機(執行 QEMU 的那個視窗)本來就看得懂。
- **i386 的 VGA 螢幕**:它只是一塊顯示記憶體,不認得任何命令。`vga.c` 加了一個小的狀態機,認得最常用的幾個:

| 序列 | 作用 |
|---|---|
| `ESC [ 列 ; 欄 H` | 游標移到指定位置 |
| `ESC [ 2 J`、`ESC [ J` | 清除整個畫面 / 游標之後的部分 |
| `ESC [ K` | 清除游標到列尾 |
| `ESC [ … m` | 顏色:0 還原、1 加亮、7 反白、30~37 前景、40~47 背景、90~97 亮的前景 |

不認得的序列整個略過,不會被當成文字印出來。這樣同一個程式在三個架構、兩種輸出上畫面一致。

### top 本身

**兩條執行緒。** 它要同時做兩件事:等按鍵、每秒更新。沒有「等按鍵,但最多等一秒」的系統呼叫,所以用行程內的兩條執行緒(第 15 步就有):主執行緒卡在讀鍵盤,另一條每秒取樣並重畫;兩者以一把使用者模式的 mutex(第 15.5 步的 futex)輪流使用資料與畫面。按鍵之後主執行緒立刻用上一次取樣的資料重畫,所以選擇與排序的反應是即時的。

**不閃爍。** 整個畫面先組在一塊緩衝區裡,一次寫出;游標回到左上角之後整個重畫,每一列畫完清到列尾。不先清除整個畫面(那會讓畫面閃一下)。

**CPU% 的分母。** 一開始用「時鐘走了幾個 tick」當分母,在 i386 上一個全速空轉的行程只顯示 64%。原因:計時器中斷被延後時,時鐘會補上遺失的 tick(見「效能調整:開機耗時與計時準確度」),但執行緒的記帳是每次中斷記一次,兩者不一樣多。改成用「同一段時間內一顆 CPU 實際記了幾個 tick」(各 CPU 忙碌與閒置增加量的平均)當分母,單位就一致了。

**終止要確認。** `x` 之後要再按 `y`。沒有任何行程在用 CPU 時,列表依 PID 排,游標預設停在第一列 — 通常是 shell;誤按一下 `x` 就等於關機。測試時我自己就踩到了(原本要打給 shell 的 `exit` 被 `top` 當成按鍵收走,其中的 `x` 把 shell 終止了),所以加上確認。

**64 位元的除法。** i386 上 64 位元的除法需要編譯器的輔助函式庫(`__udivdi3`),使用者程式沒有連結它。`top` 裡的除法都先把數值轉成 `unsigned long` 再做。

### 驗證

使用者程式的測試(`UMODE_SYSINFO`,自我測試的一部分):

| 項目 | 結果 |
|---|---|
| `sysinfo` 的基本欄位 | 1000Hz、CPU 數 1~8、行程與執行緒數至少 1、空閒記憶體小於總量 |
| 把 kernel 位址當作 `info` | `E_FAULT` |
| 空轉 20ms 前後 | 各 CPU 的累計值只增不減;忙碌的 tick 總和增加;沒有在使用的 CPU 欄位為 0 |
| 自己的 `mem_kb` | 至少 16KB;多用約 60KB 的 stack 之後增加超過 32KB |
| `tcsetmode` 不認得的位元 | `E_INVAL` |
| `tcsetmode` 設成 raw 再還原 | 都成功 |

`/etc/test.sh` 加入 `top -b`。

互動測試(經由虛擬終端機模擬按鍵;三個架構的結果相同,各產生 22 個畫面):

| 操作 | 結果 |
|---|---|
| 背景執行一個空轉的程式後啟動 `top` | 它排在第一列,CPU% 100;某一顆 CPU 的長條是滿的 |
| `p`、上下鍵 | 改依 PID 排序;`>` 記號跟著移動 |
| 對空轉的程式按 `s` | 狀態變成 `stop`,下一秒 CPU% 降到 0,累計時間不再增加;訊息列顯示 `stopped process 2` |
| 再按 `s` | 繼續執行,CPU% 回升 |
| `x` 然後 `n` | 顯示 `cancelled`,行程還在 |
| `x` 然後 `y` | 行程被終止;離開後 shell 回報 `[1] exit 143` |
| Ctrl-Z | `[1] stopped  top`;shell 的輸入恢復正常(可以打字、退格、Enter) |
| `fg` 之後 Ctrl-C | `top` 回到前景繼續更新;Ctrl-C 終止它,回到提示符號 |
| `q` | 游標移到畫面最下面,回到提示符號 |

i386 另外擷取了 VGA 螢幕的畫面:長條的顏色、反白的表頭、加亮的選取列都正確,游標停在最後一列。

回歸(`scripts/regress.sh -r 3`):三個架構各 20 個情境共 60 個,其中 59 個符合預期;磁碟跨開機測試 9 個階段全部通過;全部測試在 1、4、8 顆 CPU 下各 3 次,三個架構共 27 次全部通過。i386 舊 CPU 型號(`QEMU_CPU=qemu32`)、不接磁碟(`QEMU_DISK=none`)與主機端的執行佇列測試也通過。

**一次沒有解釋的偶發結果:** 那一輪 AArch64 的 `crash=panic` 情境,模擬器的結束碼是 5 而不是預期的 1。該次的輸出完全正常(panic 訊息與 backtrace 都完整),而 kernel 在這條路徑上傳給模擬器的結束碼固定是 1(`panic_halt` → `arch_exit(1)`),此時也還沒有初始化計時器、排程器或磁碟。之後單獨重跑這個情境 36 次、整套 AArch64 回歸再跑 2 輪,都沒有再出現。判斷是 kernel 以外(模擬器或主機)的偶發狀況,但**沒有找到確切原因**,記在這裡以便日後再遇到時對照。

### 已知限制

- **畫面固定以 80×24 計**(kernel 不知道終端機的大小);行程列表一次顯示 14 列,更多的要用上下鍵捲動。
- **更新間隔固定 1 秒**,不能調整。
- **輸出不是終端機時不會自動改成純文字**(沒有 `isatty` 這類查詢):`top | cat` 會把控制序列寫進管線,而且鍵盤仍然被 `top` 佔用。接管線時請用 `top -b`。
- **不顯示個別的執行緒**,也沒有樹狀顯示、搜尋、調整優先權(沒有對應的系統呼叫)。
- **記憶體用量含共用的頁**,各行程相加會超過實際用量;kernel 自己用掉的記憶體只反映在 `Mem` 那一列。
- **每秒切換次數與 CPU% 都包含 `top` 自己造成的部分**(它每秒做數十次系統呼叫)。
- **raw 模式是整個主控台共用的一個設定**,不是每個行程各自一份;靠「前景換人時設回一般」來避免互相影響。
- **VGA 只認得上表的幾種控制序列**;不支援捲動區域、插入刪除列、256 色。
- **`x` 送的是 `SIGTERM`**,而這個系統的訊號不能被攔截,所以效果等同強制終止。 **〔後續:第 31 步起 `SIGTERM` 可以被攔截或忽略;`top` 自己也用它來還原畫面。〕**

---

## 第 29 步:檔案介面補齊 — lseek、rename、硬連結、目前的工作目錄

### 目標

第 28 步之後做了一次整體檢查,列出還缺的重要項目。這一步先補檔案介面:到目前為止檔案只能從頭依序讀寫、不能改名、所有路徑都要寫完整。

| 新增 | 用途 |
|---|---|
| `lseek(fd, off, whence)` | 改變讀寫位置,可以讀寫檔案的任何一段 |
| `rename(old, new)` | 改名或搬移(檔案與目錄) |
| `link(old, new)` | 硬連結(hard link):同一個檔案的另一個名稱 |
| `chdir(path)` / `getcwd(buf, len)` | 目前的工作目錄(current working directory):相對路徑的起點 |
| shell 的 `cd`、`pwd`;`mv` 命令 | 提示符號顯示目前的目錄(`/disk/proj$ `) |

### lseek

讀寫位置原本就存在每一次「開啟」裡(`struct file` 的 `off`),只是沒有辦法改。`vfs_seek` 以三種起點計算新位置:從頭(`SEEK_SET`)、從目前位置(`SEEK_CUR`)、從結尾(`SEEK_END`)。

- **可以移到結尾之後。** 之後寫入時,中間沒寫過的部分讀起來是 0。兩種檔案系統本來就支援「沒有配置的區塊讀到 0」,不必為它配置空間。
- **管線與裝置沒有位置**,回傳 `E_SPIPE`。目錄只允許回到開頭(重新列一次)。
- 位置的上限取 2GB:回傳值是 `long`,在 i386 上只有 32 位元,超過會被誤認成負的錯誤碼。

### rename:為什麼分成兩步

`inode_ops` 多了一個操作 `link(dir, name, inode)`:在目錄裡加一個名稱,指向**已經存在**的 inode。改名與硬連結都建立在它上面:

```
硬連結 = 加上新名稱
改名   = 先加上新名稱,再移除舊名稱
```

改名刻意採用這個順序。中途不論發生什麼(另一條執行緒插進來、磁碟寫到一半當機),**檔案最少都還有一個名稱**;最壞的結果只是新舊兩個名稱都在,而且磁碟上的名稱數是對的(2),不需要修復。反過來做(先移除再加上)的話,中途失敗檔案就消失了。

另一個好處是不必同時鎖兩個目錄:兩步各自只鎖一個。同時鎖兩個目錄必須規定順序,而「上層先於下層」的既有規則在兩個目錄互為上下層時會和「依位址排序」衝突,容易死結。

**目的已經存在時回傳 `E_EXIST`,不會蓋掉它。** Unix 的 `rename` 會原子地取代目的檔;那需要「加上新名稱」與「移除舊的目的」不可分割。這裡選擇不做取代:誤蓋檔案是無法復原的,而需要取代時先 `rm` 再 `mv` 即可。

### 搬移目錄:不可以搬進自己底下

`mv /a /a/b/c` 如果成功,`/a` 以下的整段目錄樹會形成一個從根目錄走不到的環,再也刪不掉。檢查方法:從目的目錄沿著 `..` 一路走到根目錄,途中不可以遇到被搬的那個目錄。

這個檢查和實際的搬移之間不能有別的改名插進來(否則兩個改名可以各自通過檢查、合起來卻形成環),所以所有的改名由一把鎖(`rename_mx`)排成一次一個。它只在改名時使用,而且總是第一個取得,不會和其他鎖形成循環。

目錄搬家時,`link` 順便把它的「上層目錄」改成新的(ramfs 的 `parent` 指標、WFS inode 的 `parent` 欄位),所以搬完之後 `..` 是對的。

不能跨檔案系統(`E_XDEV`):`/` 與 `/disk` 之間要用 `cp` 再 `rm`。掛載點不能搬(`E_BUSY`)。

### 目前的工作目錄:放在系統呼叫這一層

每個行程記著一個目錄。使用者傳來的路徑不是以 `/` 開頭時,接在它後面。

這件事**完全在系統呼叫層做**(`proc.c` 的 `user_path`):把相對路徑接成絕對路徑,再交給 VFS。VFS 仍然只認得從根目錄開始的路徑,不必知道「行程」是什麼 — 沒有增加兩層之間的耦合。

工作目錄以**字串**保存,而不是抓著那個目錄的 inode:

| | 存字串(這裡的做法) | 存 inode(Unix 的做法) |
|---|---|---|
| 實作 | 簡單;`fork` / `spawn` 直接複製 | 要管理參考計數 |
| 目錄被刪除 | 照常刪除,之後相對路徑得到「不存在」 | 目錄刪不掉,或留下沒有名稱的目錄 |
| 目錄被改名 | 行程記著的還是舊名稱(缺點) | 不受影響 |

`chdir` 把路徑整理成標準的樣子(拿掉 `.`、`..` 與重複的 `/`)再存,所以 `cd ..` 做一百次,字串也不會變長。整理之前先用原本的路徑向 VFS 確認它存在而且是目錄。

`cd` 必須是 shell 的內建命令:工作目錄是每個行程自己的,另外啟動一個程式去改,改到的是那個程式的,它一結束就沒有了。

### 驗證

使用者程式的測試(`UMODE_FILEOPS`),**同一組測試在記憶體檔案系統與磁碟上各做一次**:

| 項目 | 結果 |
|---|---|
| `lseek` 三種起點後讀取 | 讀到對應位置的位元組 |
| 負的位置;不認得的 whence | `E_INVAL` |
| 移到 5000 再寫 1 個位元組 | 大小 5001;第 4999 個位元組讀到 0 |
| 移到中間寫 2 個位元組 | 只蓋掉那兩個,前後不變 |
| 對已關閉的描述符;對主控台 | `E_BADF`;`E_SPIPE` |
| 改名 | 舊名稱消失,新名稱的大小與名稱數正確 |
| 來源不存在;目的已存在 | `E_NOENT`;`E_EXIST`(來源還在) |
| 把目錄搬進自己底下 | `E_INVAL` |
| 把目錄搬到別處後走 `..` | 到達新的上層目錄 |
| 硬連結 | 名稱數 2;從一個名稱寫入,另一個名稱讀得到;刪掉一個,檔案還在 |
| 對目錄做硬連結;目的已存在 | `E_INVAL`;`E_EXIST` |
| `chdir` 到檔案 / 不存在的路徑 | `E_NOTDIR` / `E_NOENT` |
| 相對路徑、`./a`、空字串 | 找得到;找得到;`E_NOENT` |
| `fork` 的子行程以相對路徑建立檔案 | 父行程用完整路徑看得到(工作目錄有繼承) |
| `chdir("/../../etc/../")` | 工作目錄是 `/` |
| 跨檔案系統改名、硬連結 | `E_XDEV` |

磁碟上做完之後,kernel 再跑一次完整的一致性檢查:0 個問題。

互動測試:

```
$ cd /disk
/disk$ mkdir proj
/disk$ cd proj
/disk/proj$ echo one > a.txt
/disk/proj$ mv a.txt b.txt
/disk/proj$ cd ..
/disk$ mv proj work
/disk$ cat work/b.txt
one
/disk$ mv work /etc
mv: cannot move work (different file system; use cp and rm)
/disk$ cd nowhere
sh: cd: no such directory: nowhere
```

### 已知限制

- **改名不會取代已經存在的目的檔**,也不是原子的(中途可以看到兩個名稱)。
- **不能跨檔案系統搬移**;`mv` 不會自動改成複製再刪除。
- **工作目錄是字串**:目錄被改名後,正在裡面的行程要重新 `cd`。
- **沒有符號連結**(symbolic link);硬連結只限一般檔案。
- **shell 找程式仍然只看 `/bin`**(沒有 `PATH`),也沒有 `cd -`、`~`。
- **讀寫位置上限 2GB**。

---

## 第 30 步:使用者 heap 與小型 C 函式庫

### 目標

到目前為止,使用者程式只有兩種記憶體:編譯時就固定大小的全域變數,以及 stack。事先不知道要多大的資料(例如「把整個檔案讀進來排序」)沒有地方放。另外每個程式都自己寫字串處理與輸出,沒有 `printf`。

這一步補上:

- kernel:`sbrk` 系統呼叫 — 行程可以在執行期間向系統要記憶體、也可以還回去
- 使用者端:`user/libc.c` 與 `libc.h` — `malloc` / `free` / `realloc` / `calloc`、`printf` 系列、常用的字串函式
- 一個真的需要它們的程式:`sort`

### kernel:heap 是一段「用到才配置」的位址範圍

每個行程的位址空間裡保留一段固定的範圍給 heap(程式之後、stack 之前,最多 256MB)。行程記著其中的「結尾」(break),`sbrk(incr)` 把它往後或往前移,回傳移動之前的位置。

```
USER_BASE          +256MB                              +768MB
  | 程式與資料 ....... | heap ------> brk ...(上限 256MB)... | <------ 各執行緒的 stack |
```

**變大時 kernel 什麼都不配置**,只是把結尾的數字改掉。某一頁第一次被碰到時才觸發 page fault,當場配置一頁清為 0 的記憶體 — 和第 22 步的 stack 自動成長是同一個機制(`proc_fault` 多一段判斷)。好處:

- `sbrk` 本身極快,不會失敗在一半
- 程式要了很大一塊卻只用一點點,只花用到的部分
- 新的記憶體保證是 0,不會讀到別的行程留下的資料

**變小時立刻歸還**:結尾之後整頁落在範圍外的,當場取消對映並釋放。為此 `vm.c` 新增 `vm_space_unmap`(之前位址空間只能整個釋放,不能拿掉單一頁)。取消對映要通知其他 CPU 清掉它們的舊紀錄(同一行程的另一條執行緒可能正在別顆 CPU 上),用的是第 22 步之後就有的「只通知用過這個位址空間的 CPU」。

`fork` 時 heap 隨位址空間以寫入時複製的方式複製,結尾的位置一併複製;`exec` 時歸零。

**記憶體不足時**:配置是延後到碰觸時才做的,所以 `sbrk` 成功不代表之後一定拿得到記憶體。真的用完時,碰到那一頁的行程會像存取非法位址一樣被終止。這稱為過度承諾(overcommit),Linux 預設也是這樣;代價是程式沒有機會處理「記憶體不足」。

### malloc

heap 裡一塊接一塊,每一塊前面有 16 位元組的標頭(大小、使用中或空閒的記號)。

- `malloc`:從頭找第一個夠大的空塊(first fit);太大就切開;都不夠就用 `sbrk` 再要一段(每次至少 64KB,減少系統呼叫)
- `free`:標記為空的,和後面相鄰的空塊合併;最尾端的空塊超過 256KB 時還給 kernel
- 多條執行緒可以同時使用:整個 heap 由一把使用者模式的 mutex 保護

簡單而不快(每次都從頭掃),對這裡的小程式足夠。設計上優先的是**偵測錯誤**:標頭裡有固定的記號,`free` 時檢查。釋放不是 `malloc` 給的指標、同一塊釋放兩次、標頭被寫壞,都會立刻印出訊息並終止程式。這類錯誤如果不當場攔下,會在很久以後以完全不相干的樣子出現,非常難查。

**踩坑:32 位元架構上的對齊。** 第一版的標頭在 i386 上是 12 位元組(`size_t` 只有 4 位元組),回傳的指標因此沒有對齊 16 位元組,測試當場失敗。改成明確要求標頭的對齊為 16,並加上編譯期的檢查(`_Static_assert`)。這是「三個架構都測」抓到的問題:只在 64 位元上測試不會發現。

### printf

`format()` 是唯一的格式化核心,輸出到一個可換的目的地:`printf` / `fprintf` 寫到檔案描述符(先組在 256 位元組的緩衝區,通常一次寫出,不會和別的行程的輸出交錯),`snprintf` 寫到呼叫者的緩衝區。支援 `%d %i %u %x %X %c %s %p %%`、`l` / `ll` / `z`、`-` 與 `0`、寬度(含 `*`)。不支援浮點數(kernel 與使用者程式都不使用浮點暫存器)。

i386 上 64 位元的除法需要編譯器的輔助函式庫,所以 `%lld` 不直接除以 10,而是每次處理 16 個位元,讓被除數不超過 32 位元。

### 連結:只帶走用到的部分

`libc.c` 連結進每個程式,但以 `-ffunction-sections` 編譯、`--gc-sections` 連結:每個函式各自一個區段,沒有被用到的由連結器丟掉。實測加入函式庫之後,原本不使用它的程式(`cat`、`ls`…)大小完全沒變(仍是 4100 位元組)。

既有的程式沒有改寫,仍然使用 `ulib.h`;新的程式可以直接用 `libc.h`。

### sort

`sort [-r] [檔案]`:把輸入的每一行排序後輸出。它事先不知道輸入有多大,用 `realloc` 讓緩衝區長大,以合併排序(穩定,最壞情況也是 n log n)排好,`printf` 輸出。

### 驗證

使用者程式的測試(`UMODE_HEAP`):

| 項目 | 結果 |
|---|---|
| `sbrk(0)` 兩次 | 相同的位置 |
| 加大 64KB 之後、還沒碰 | 行程的記憶體用量不變(沒有配置) |
| 讀過每一頁 | 內容都是 0;用量增加 64KB |
| 縮小 32KB | 用量立刻減少 32KB |
| 再加大 32KB | 歸還過的部分又是 0;前半的資料不受影響 |
| 縮到起點之前;超過上限 | `E_NOMEM` |
| 碰結尾之後的位址(子行程) | 被終止 |
| 120 塊大小不一的配置,釋放一半再配置 | 沒被釋放的那一半內容原封不動;指標都對齊 16 |
| 全部釋放後配置一塊和整個 heap 差不多大的 | 成功,而且沒有向 kernel 多要(空塊有合併) |
| `realloc` 加大;`calloc` | 內容保留;全部是 0 |
| 配置 2GB;`calloc` 相乘溢位 | 回傳 NULL |
| 配置 1MB、寫入、釋放 | heap 的結尾退回去,用量下降 |
| `fork` 後子行程改寫 heap | 父行程的內容不變 |
| 同一塊釋放兩次(子行程) | 結束碼 134 |
| 三條執行緒各 150 次配置 / 檢查 / 釋放 | 全部正確 |
| `snprintf` 各種格式(含 64 位元、寬度、靠左、補零) | 和預期的字串完全相同 |
| 緩衝區太小 | 截斷,回傳完整的長度 |
| `memmove` 重疊的範圍、`strrchr`、`atoi` 等 | 正確 |

`/etc/test.sh` 加入 `sort` 讀檔案與接在管線後面兩種用法。

### 已知限制

- **記憶體用完時,碰到新頁的行程直接被終止**,沒有機會處理(過度承諾)。 **〔後續:已解決:第 35 步改為承諾記帳與配額,超過的要求當場回報 `E_NOMEM`。〕**
- **`malloc` 每次從頭掃描**,區塊很多時會慢;沒有依大小分類的空閒串列。
- **只有最尾端的空塊會還給 kernel**;中間的空洞留在行程裡重複使用。
- **沒有 `mmap`**:不能把檔案對映進記憶體,也不能配置不相連的區域。 **〔後續:第 35 步加入匿名對映(`mmap` / `munmap`);仍然不能對映檔案。〕**
- **`printf` 不支援浮點數**,也沒有 `scanf`、`FILE *` 與緩衝的輸入輸出。
- **既有的程式沒有改用函式庫**,兩套小工具(`ulib.h` 與 `libc.h`)並存。

---

## 第 31 步:訊號處理函式

### 目標

第 24、26 步的訊號只有固定的效果(終止、暫停、繼續),行程不能決定怎麼回應。這一步讓行程可以:

- **忽略**某個訊號(`SIG_IGN`)
- 登記一個**處理函式**(signal handler):訊號來的時候,暫時放下手邊的事去執行它,執行完再從被打斷的地方繼續

用途例如:`top` 被 Ctrl-C 終止前先把畫面還原;長時間執行的程式收到 `SIGTERM` 時先存檔再結束。

可以設定的訊號:`SIGINT`、`SIGTERM`、`SIGTSTP`,以及新加入、留給程式自己使用的 `SIGUSR1`(10)、`SIGUSR2`(12)。**`SIGKILL` 與 `SIGSTOP` 不能改**:不論程式怎麼寫,使用者永遠有辦法停下它。

### 困難在哪裡

訊號隨時可能來。程式當時也許正算到一半,暫存器裡都是有用的資料。處理函式跑完之後,程式必須**從被打斷的那一條指令、以完全相同的暫存器內容**繼續,而且它不知道自己被打斷過。

這和「中斷」是同一個問題,只是這次打斷使用者程式的不是硬體而是 kernel,被執行的也不是 kernel 的程式而是使用者自己的函式。

### 做法:把暫存器存到使用者自己的 stack

kernel 沒有另外的地方保存這些暫存器,而是借用使用者的 stack。三個步驟:

```
1. 交付    執行緒要返回使用者模式時(user_return_check),把它的暫存器(trap frame)
           整份複製到使用者 stack 上,再把 trap frame 改成「呼叫 handler(sig)」
2. 返回    處理函式的返回位址被設成一小段使用者端的程式(restorer),
           它只做一件事:呼叫 sigreturn,並告訴 kernel 那份暫存器放在哪裡
3. 還原    sigreturn 把那份暫存器讀回來,取代目前的 trap frame。
           返回使用者模式後,程式從當初被打斷的地方繼續

     使用者 stack(往下長)
     | 被打斷時正在用的部分 |
     +----------------------+ <- 被打斷時的 sp
     | (空隙 128 位元組)     |
     | 存下來的暫存器        |   kernel 寫入;sigreturn 時讀回
     +----------------------+ <- frame(對齊 16)
     | 處理函式自己用的部分  |
```

交付的時機沿用既有的檢查點:每次從 kernel 返回使用者模式之前。終止、暫停、訊號處理函式三件事現在都在 `user_return_check` 裡依序檢查。

處理函式巢狀沒有問題(處理到一半又來一個訊號):每一次交付各自在 stack 上往下放一份,返回時依相反的順序還原。

### 架構相關的部分只有四個小函式

存放、複製、檢查都在 `proc.c`(與架構無關)。各架構只提供(`arch.h`):

| 函式 | 做的事 |
|---|---|
| `arch_frame_user_sp(tf)` | 取出使用者的 stack pointer |
| `arch_frame_call(tf, frame, pc, arg, ret, words)` | 把 tf 改成「呼叫 pc(arg),返回到 ret」 |
| `arch_frame_restore(tf, saved)` | 以 saved 的內容取代 tf,但保留特權相關的欄位 |
| `arch_frame_get_retval(tf)` | 讀出回傳值的暫存器 |

`arch_frame_call` 的差異在呼叫慣例:RISC-V 與 AArch64 的參數與返回位址都放在暫存器(`a0` / `ra`、`x0` / `x30`);i386 兩者都在 stack 上,所以它另外回報「有兩個字要寫到新的 stack pointer 處」。

`sigreturn` 的回傳值有一個細節:系統呼叫返回時,回傳值會被寫進「回傳值的暫存器」,而那個暫存器剛被還原成被打斷時的內容。所以 `sigreturn` 回傳的就是還原之後那個暫存器的值,等於沒有動到它。

restorer 必須用組合語言寫(`user/crt0.c`,每個架構三行):C 函式一進來就會動到 stack pointer,而它需要的正是 stack pointer 原本的值,用來算出那份暫存器的位置。

### 安全:那份暫存器是使用者可以改的

存下來的暫存器放在使用者的記憶體裡,程式可以任意修改,也可以從頭捏造一份直接呼叫 `sigreturn`。如果 kernel 照單全收,程式就能把「特權等級」那個欄位改成 kernel 模式,返回之後以 kernel 的權限執行任意程式碼 — 這是歷史上真實出現過的一類漏洞。

所以還原時,**只有 kernel 能決定的部分一律不採用**,保留 trap frame 原本的值:

| 架構 | 不採用的欄位 | 裡面有什麼 |
|---|---|---|
| RISC-V | `sstatus` | 返回後的特權等級(SPP)、中斷開關 |
| AArch64 | `spsr`、kernel 的 `sp` | 例外等級、中斷遮蔽 |
| i386 | `cs` `ss` `ds` `es`,`eflags` 只採用運算結果的 7 個位元 | 特權等級、中斷開關、I/O 權限 |

程式能改的只有它本來就能改的一般暫存器。最壞的結果是它自己跳到不存在的位址,像任何出錯的程式一樣被終止。

其他檢查:處理函式與 restorer 的位址在登記時就確認是行程自己可以存取的;交付時如果使用者的 stack 寫不進去(壞了或用完了),行程以該訊號的預設方式終止。

### 打斷等待中的系統呼叫

行程登記了處理函式,而訊號來的時候它正睡在 kernel 裡(`sleep`、等輸入、等管線、等子行程)。如果等它自然醒來才處理,Ctrl-C 可能要等很久才有反應。

所以送出這類訊號時,把那個行程正在等待的執行緒提早叫醒,系統呼叫回傳 `E_INTR`,處理函式隨即執行。排程器為此多了一個和「終止」平行的標記:

| | 終止 | 被訊號打斷 |
|---|---|---|
| 標記 | `killed` | `interrupted` |
| 設定者 | `sched_kill_group` | `sched_interrupt_group` |
| 等待迴圈檢查 | `thread_killed()` | `thread_interrupted()`(終止時也是真) |
| 之後 | 執行緒結束 | 系統呼叫回傳 `E_INTR`,繼續執行 |

管線與主控台因此只需要認得排程器的這個函式,不必知道「訊號」或「行程」是什麼。

**只有登記了處理函式的行程會遇到 `E_INTR`。** 沒有登記的行程,訊號只會終止或暫停它,既有的程式不必修改。讀取時 `E_INTR` 和「結束」(0)是分開的:被打斷的 `read` 可以再呼叫一次,不會被誤認成檔案結尾。

### exec 與 fork

- `exec`:處理函式是舊程式裡的位址,全部還原成預設;「忽略」則保留(和 Unix 相同)
- `fork`:子行程沿用父行程的設定;還沒交付的訊號不帶過去

### 驗證

使用者程式的測試(`UMODE_SIGHANDLER`):

| 項目 | 結果 |
|---|---|
| 登記後送給自己 | 系統呼叫返回前處理函式已執行;再登記一次回傳原本的函式 |
| 登記 `SIGKILL`、`SIGSTOP`、不存在的訊號 | `E_INVAL` |
| 處理函式是 kernel 的位址 | `E_FAULT` |
| 一段使用 8 個變數的運算,途中被打斷約 20 次 | 結果和沒被打斷時完全相同 |
| 睡 5 秒,20ms 後收到訊號 | 立刻醒來,處理函式執行一次 |
| 等管線的資料時收到訊號 | `read` 回傳 `E_INTR`;再讀一次得到資料 |
| 忽略 `SIGTERM` 的行程 | 收到後還活著;`SIGKILL` 仍然終止它 |
| 處理函式直接結束行程 | 結束碼是處理函式指定的 |
| `exec` 之後收到原本有處理函式的訊號 | 以預設方式終止(138) |
| 捏造一份全是 0xFF 的暫存器呼叫 `sigreturn` | kernel 不受影響;行程因跳到不存在的位址被終止 |
| `sigreturn` 的位址是 kernel 的 | 行程被終止 |
| stack pointer 被設成 16 時收到訊號 | 行程以該訊號終止(140),kernel 不受影響 |

三個架構、1 顆與 4 顆 CPU 都通過。捏造暫存器那一項是在驗證安全性:如果特權欄位被採用,i386 會在 kernel 裡發生保護錯誤、RISC-V 會以 kernel 權限跳到垃圾位址,測試就會以 panic 收場。

`top` 改用處理函式:被 Ctrl-C 或 `kill` 終止時先還原畫面與主控台模式,結束碼 130。

### 已知限制

- **沒有訊號遮罩(signal mask)**:處理函式執行中,同一個訊號可以再進來(巢狀)。不能暫時擋住訊號。
- **同一個訊號在交付前來了好幾次,只算一次**(每個訊號只有一個待處理的位元)。
- **處理函式只收到訊號編號**,沒有 `sigaction` 那樣的額外資訊(誰送的、出錯的位址)。
- **沒有 `SIGCHLD`、`SIGSEGV`、`SIGPIPE`、`SIGALRM`**:子行程結束不會通知;存取錯誤仍然直接終止,不能攔截;沒有鬧鐘。
- **被打斷的系統呼叫不會自動重來**,程式要自己處理 `E_INTR`。
- **shell 沒有使用處理函式**:它仍然靠「前景群組」避開 Ctrl-C。
- **`SIGCONT` 不能有處理函式**。

---

## 第 32 步:日期時間

### 目標

系統只知道「開機之後過了幾個 tick」,不知道現在是幾月幾號。這一步補上:

- 開機時讀取硬體的即時時鐘(Real-Time Clock,RTC)
- `time` 系統呼叫:現在的日期時間
- 檔案與目錄的**最後修改時間**(存在 inode 裡;磁碟上的檔案跨開機保留)
- `date` 命令、`ls -l`、函式庫的 `gmtime_`(秒數換成年月日)

### 時間的表示

kernel 裡只有一個數字:**1970-01-01 00:00:00 UTC 至今的秒數**(Unix 時間)。年月日、星期、時區都是「顯示」的問題,交給使用者端的函式庫。這樣 kernel 不需要曆法的程式碼,比較時間先後只是比大小。

RTC 讀起來慢(i386 要十幾次 I/O),所以只在開機時讀一次,記下「開機那一刻是第幾秒」;之後的時間 = 那一刻 + 開機後經過的 tick 換算成的秒數。

### 三個架構的時鐘

| 架構 | 裝置 | 讀法 |
|---|---|---|
| i386 | CMOS RTC(I/O port 0x70 / 0x71) | 分別讀出秒、分、時、日、月、年、世紀,自己換算成秒數 |
| RISC-V | Goldfish RTC(0x101000) | 兩個 32 位元暫存器合起來是「1970 年至今的奈秒數」 |
| AArch64 | PL031(0x09010000) | 一個暫存器,就是秒數 |

i386 的最麻煩,因為它給的是「人看的」格式,而且有好幾種變化要處理:

- 數值可能是 BCD(Binary-Coded Decimal,每 4 個位元一位十進位數字)或二進位,由狀態暫存器 B 決定
- 可能是 12 小時制(最高位元表示下午)
- 時鐘每秒更新一次,更新途中讀到的值可能一半新一半舊:先等「更新中」的旗標消失,並且**連續讀到兩次相同才採用**
- 讀回來的數值不合理(月份 13、年份太離譜)就當作沒有時鐘,而不是算出一個錯的時間

### 64 位元的除法

tick 換算成秒要除以 1000,而 tick 是 64 位元。i386 上 64 位元的除法需要編譯器的輔助函式庫,kernel 沒有連結它(第 10 步起就一直避開)。這次寫了一個小函式 `div_u64`:每次處理 16 個位元,讓每一步的被除數都放得進 32 位元。使用者端的 `printf("%lld")` 用的是同一個做法。

### 檔案的修改時間

`struct inode` 多了 `mtime`,由 **VFS** 在修改之前更新:寫入、清空、在目錄裡建立或移除東西。放在 VFS 而不是各個檔案系統裡,所以 ramfs 與 WFS 不必各寫一次,之後加入的檔案系統也自動具備。

WFS 把它存進 inode 原本沒有使用的 4 個位元組(磁碟上的格式不必更動,既有的磁碟映像照常可用,舊檔案的時間顯示為「不知道」)。32 位元的秒數可以用到西元 2106 年。VFS 是在呼叫檔案系統「之前」設定時間,WFS 寫回 inode 時就一併存下,不需要額外的磁碟寫入。

### 秒數換成年月日

`gmtime_`(在 `libc.c`)採用 Howard Hinnant 的 `civil_from_days` 算法:把一年當成從 3 月開始,閏日就落在一年的最後一天,整個換算只需要幾次整數除法,沒有迴圈、沒有查表。i386 的 RTC 驅動用的是它的反方向(`days_from_civil`)。

系統沒有時區的設定:`date` 顯示 UTC,`date +8` 加上時差。

### 驗證

使用者程式的測試(`UMODE_TIME`):

| 項目 | 結果 |
|---|---|
| `time` | 落在 2023~2100 年之間;微秒小於 100 萬 |
| 把 kernel 位址當作參數 | `E_FAULT` |
| 睡 30ms 前後 | 時間往前走了至少 20ms |
| 換算:1970-01-01(星期四) | 正確 |
| 換算:2000-02-29 的第一秒與最後一秒、2000-03-01 | 正確(2000 是閏年) |
| 換算:2100-02-28 的最後一秒、下一秒 | 2100-03-01(2100 不是閏年) |
| 換算:加 8 小時跨到隔天 | 日期與星期都進位 |
| 新建立的檔案 | 修改時間是現在 |
| 寫入之後 | 不會倒退,不會超過現在 |
| 刪除檔案之後,所在目錄的修改時間 | 不早於檔案的時間 |

三個架構開機後 `date` 顯示的時間和主機的 UTC 時間相同(實測:`2026-11-18 03:35:08`,主機 `03:36:11`,差距是測試本身花的時間)。

跨開機測試(`disktest=2`)新增:上一次開機寫入的檔案,修改時間仍在、落在合理範圍、不晚於現在。

```
$ ls -l /etc
-      104  2026-11-18 03:35  motd
-      924  2026-11-18 03:35  test.sh
$ date +8
2026-11-18 11:35:08 Wed UTC+8
```

### 順帶:測試的耗時

這幾步加了不少測試,自我測試在客體內的時間從約 0.8 秒增加到約 1.1 秒。每個使用者程式的測試現在會印出它花了幾毫秒,方便找出拖慢的項目;據此把訊號測試(227 → 約 100 ms)與 `top -b` 的取樣間隔(200 → 100 ms)縮短了。

### 已知限制

- **時間以秒為單位記在檔案上**;同一秒內的多次修改分不出先後。
- **沒有時區與日光節約時間的設定**,也不能設定時間(沒有 `settimeofday`,不會寫回硬體時鐘)。
- **開機之後不再和硬體時鐘對時**:模擬器暫停很久之後,系統的時間會落後。
- **只記錄修改時間**,沒有建立時間與存取時間。
- **開機檔案包裡的檔案,時間是解開的那一刻**(也就是開機的時間),不是原始檔的時間。
- **`gmtime_` 用 32 位元運算**,可以表示到 2106 年。

---

## 第 33 步:磁碟錯誤的處理與測試

### 目標

架構檢查時發現:磁碟驅動程式與 WFS 裡「讀寫失敗」的分支**從來沒有被執行過** — 模擬器裡的磁碟不會壞。沒有執行過的程式碼等於沒有測試過,而它們偏偏是出事時才會用到的部分。

這一步:

- 讓測試可以**故意製造**磁碟錯誤
- 決定並實作「寫入失敗之後檔案系統該怎麼辦」(原本沒有定義)
- 把這些情況放進回歸測試

### 可以故意出錯的磁碟

`kernel/blkfault.c` 在真正的磁碟外面包一層。它提供同一個 `struct blkdev` 介面,平常原封不動地轉給底下的驅動程式;測試時可以要求「接下來的讀取(或寫入)一律回報失敗」。

檔案系統看到的就是一顆會壞的磁碟,而它完全不知道中間多了一層 — 這正是第 27 步定下區塊裝置介面的用途:任何提供這個介面的東西都可以當磁碟。測試不需要在驅動程式或檔案系統裡埋任何「測試專用」的判斷。

平常的代價是每次磁碟讀寫多一次函式呼叫與兩次比較。

### 寫入失敗之後:轉為唯讀

原本的行為:寫回失敗的區塊被直接放棄(標記為「不髒了」),操作回報錯誤,然後一切照舊。問題是此時**記憶體裡的狀態和磁碟上的已經不一致**(記憶體說「建立了這個檔案」,磁碟上沒有),而且不知道是哪些區塊沒寫成功。繼續在上面修改,只會把不一致擴大,最後連原本好的資料也賠進去。

新的規則:

| 情況 | 處理 |
|---|---|
| **讀取**失敗 | 那一次操作回報 `E_IO`。狀態不變;磁碟恢復之後可以再試 |
| **寫入**失敗 | 整個檔案系統轉為**唯讀**:之後所有會修改的操作(建立、刪除、寫入、清空、改名、連結)都回報 `E_IO`。讀取照常 |
| 唯讀之後關機 | 不記「正常卸載」→ 下次掛載時做完整的檢查與修復 |

即使磁碟馬上就恢復正常,也不會自動解除唯讀:修復需要「以磁碟上的內容為準重新來過」,那正是下次掛載時的檢查在做的事。這是保守的選擇 — 寧可拒絕服務,也不在不確定的狀態上繼續寫。Linux 的 ext4 預設也是這樣(`errors=remount-ro`)。

### 驗證

跨開機測試從 3 個階段增加為 5 個(同一個映像檔):

```
disktest=4
  清空快取、讓讀取失敗      -> 讀檔案得到 E_IO
  恢復                      -> 同一個檔案內容完全正確;檔案系統不是唯讀
  讓寫入失敗、建立目錄      -> E_IO;印出 "write error on block 10; the file system is now read-only"
  恢復之後再建立目錄        -> 仍然 E_IO
  刪除、附加寫入、建立檔案、改名  -> 全部 E_IO
  讀取原本的檔案            -> 內容完全正確
  要求讀取超出磁碟範圍的磁區  -> 驅動程式拒絕

disktest=5
  掛載                      -> "not shut down cleanly; checked it and repaired 0 problem(s)"
  原本的檔案                -> 內容完全正確
  上一次失敗的那兩個目錄    -> 不存在
  再做一次完整檢查          -> 0 個問題
  建立並刪除一個目錄        -> 成功(又可以寫入了)
```

三個架構的結果相同。第 5 階段修復了 0 個問題是合理的:失敗的那次寫入連第一個區塊都沒寫進去,磁碟上保持在操作之前的一致狀態。

另外在區塊快取加了一個斷言:同一個緩衝區被歸還的次數多於取用的次數時 panic(這類錯誤會讓使用中的區塊被換掉,造成很難追查的資料損壞)。

### 已知限制

- **只能注入「全部失敗」**,不能指定第幾次或哪個區塊;寫到一半才失敗(部分區塊已經寫入)的情況沒有測到。
- **驅動程式裡的逾時分支仍然沒有被執行過**(需要一個不回應的裝置)。
- **唯讀之後沒有辦法在不重開機的情況下修復**(沒有重新掛載的操作)。
- **讀取失敗不會重試**;區塊快取裡不會留下失敗的紀錄,下次讀取會再問磁碟一次。
- **寫回失敗時,那些還沒寫入的修改就遺失了**(記憶體裡的快取在下次開機時不存在)。

---

## 第 34 步:使用者與權限

### 目標

到上一步為止,任何行程都可以讀寫刪除任何檔案、終止任何行程。這一步加入:

- 每個行程的**身分**(使用者編號 uid、群組編號 gid)與一組**特權**
- 每個檔案與目錄的**擁有者**與**權限位元**,由 VFS 在每個操作檢查
- 誰可以對誰送訊號
- `id`、`su`、`chmod`、`chown`;`ls -l` 顯示權限與擁有者;`/etc/passwd`

從這一步起,設計時另外對照高可靠系統(航太飛行軟體)的做法。這裡採用的是**最小權限**與**特權只能減少**兩條。

### 特權只能減少

傳統 Unix 的模型是「uid 0 什麼都能做」,並且可以透過密碼、setuid 程式把權限**升上去**。升權限的路徑正是歷來最多漏洞的地方。

這裡的規則只有一條:**沒有任何系統呼叫能讓行程得到它原本沒有的特權。**

- kernel 啟動的第一個行程(shell)是 root,擁有全部特權
- 子行程繼承父行程的身分與特權
- `setuid(uid, gid)`:有 `CAP_SETID` 的行程可以變成任何人;變成非 root 時**所有特權清除**
- `capdrop(mask)`:永久放棄指定的特權(仍然是 uid 0 也可以這樣做)

結果:

| | |
|---|---|
| 不需要密碼 | 沒有「證明你是誰來取得權限」這件事。`su alice` 是往下降,任何有特權的行程都可以做;沒有特權的人執行 `su root` 只會得到 `permission denied` |
| 「回到 root」 | 不是把身分升回去,而是結束降級後的 shell,回到一直等在那裡的父行程 |
| 被攻破時的損害 | 限於那個行程還留著的特權 |

特權切成四項,而不是「有或沒有」:

| 特權 | 允許 |
|---|---|
| `CAP_FS` | 不受檔案權限限制;更改任何檔案的擁有者與權限 |
| `CAP_KILL` | 對別人的行程送訊號 |
| `CAP_SETID` | 改變自己的 uid / gid |
| `CAP_ADMIN` | 保留給之後的系統管理功能(例如設定網路) |

一個只需要管理行程的服務可以只留下 `CAP_KILL`。即使它出錯或被利用,也碰不到別人的檔案。

### 檔案的權限

數值與 Unix 相同:三組各三個位元(擁有者、同群組、其他人 × 讀 4、寫 2、執行 1),習慣以八進位書寫(`0644`)。對目錄而言,讀 = 列出內容,寫 = 在裡面建立或刪除,執行 = 可以經過它。

檢查的位置與規則:

| 操作 | 需要 |
|---|---|
| 路徑上經過的每個目錄 | 該目錄的執行 |
| 開啟 | 依方式需要讀、寫;清空內容算寫入 |
| 建立、刪除、改名、連結 | **所在目錄**的寫入與執行(不看檔案本身的權限) |
| 執行程式(`spawn` / `exec`) | 檔案的執行;完全沒有執行位元的檔案連特權行程也不能執行 |
| `chdir` | 目錄的執行 |
| `chmod` | 是擁有者,或有 `CAP_FS` |
| `chown` | 有 `CAP_FS`(一般使用者不能把檔案「送」給別人) |

幾個值得說明的地方:

- **只看符合的那一組。** 擁有者就用擁有者那一組,不會因為「其他人」那一組比較寬就放行。所以擁有者把自己的檔案設成 `000`,連自己也打不開(但可以再 `chmod` 回來)。
- **權限在開啟的那一刻決定。** 之後的讀寫不再檢查:檔案開啟之後即使權限被改掉,這次開啟仍然有效。檢查因此只發生一次,讀寫的路徑沒有變慢。
- **新檔案屬於建立它的人**,預設 `0644`,目錄 `0755`。新建立的檔案沒有執行權限,複製來的程式要 `chmod` 才能執行(`cp` 會連權限一起複製)。
- **`/bin` 底下的程式是 `0755`,其餘是 `0644`**,由 kernel 解開開機檔案包時決定,不採用檔案包裡記的權限。原因:建置在 Windows 的磁碟上進行,那裡每個檔案都被當成可執行;由 kernel 決定,結果才固定。

### 低耦合:VFS 不認得行程

權限檢查在 VFS,但 VFS 不該知道「行程」這個結構。新增一個很小的標頭 `include/cred.h`:

```c
struct cred { uint32_t uid, gid, caps; };
const struct cred *cred_current(void);     /* 現在是誰在問?NULL = kernel 自己 */
```

VFS 只引用它;實作在 `proc.c`。kernel 自己的呼叫(開機時解開檔案包、掛載磁碟、自我測試)得到 NULL,不受限制。這也是之後網路等模組判斷特權的同一個入口。

### 磁碟上的格式:不增加大小,而且相容

WFS 的 inode 是 64 位元組,已經沒有空位。做法是把兩個**原本太寬**的欄位切開:

| 原本 | 現在 |
|---|---|
| `parent` 32 位元 | `parent` 16 位元(inode 最多 4096 個)+ `mode` 16 位元 |
| `size` 64 位元 | `size` 32 位元(單一檔案最大約 4MB)+ `uid` 16 + `gid` 16 |

數值是 little-endian,原本的低位部分留在原位,所以**舊的磁碟映像照常可讀**:新欄位在舊資料裡都是 0,即「擁有者 root、權限未記錄」。`mode` 的最高位元用來分辨「權限是 000」與「舊資料,沒有記錄」,後者使用預設值。編譯期有檢查確保這個結構維持 64 位元組。

新格式化的磁碟,根目錄是 `0777`:每個使用者都可以在 `/disk` 建立自己的東西。

### 訊號

同一個使用者的行程之間可以互相送訊號;對別人的行程需要 `CAP_KILL`。主控台的 Ctrl-C / Ctrl-Z 由 kernel 送出,不受這個限制(坐在終端機前的人永遠可以停下前景的程式)。

### 順帶修正

**一致性檢查對全新磁碟誤報 1 個問題(第 27 步的錯誤)。** 格式化時,位元圖最後一個區塊中「超出磁碟大小」的位元被標記為使用中(保護用),但檢查重新計算位元圖時沒有照做,於是全新的磁碟第一次檢查一定回報「位元圖不符」並把那些位元清掉(無害,但不正確)。之前沒發現,是因為測試用的映像檔都早已被檢查改寫過一次;這一步為了測試新的磁碟格式而刪掉映像檔重建,才暴露出來。教訓:**長期重複使用的測試資料會掩蓋「只在全新狀態出現」的問題**;回歸測試的跨開機階段本來就使用全新的映像檔,但它在第一次檢查之前先經過了故意的損壞,同樣沒有測到。

**多顆 CPU 同時輸出 panic 訊息。** 一顆 CPU panic 之後會讓所有的鎖失效以便印出訊息,其他 CPU 在被叫停之前可能因此看到不一致的狀態而跟著 panic,兩邊的訊息逐字交錯,真正的原因被蓋掉。現在只有第一顆 CPU 回報;之後才 panic 的別顆 CPU 直接停下;同一顆 CPU 在處理 panic 途中又出錯則直接結束。

### 驗證

使用者程式的測試(`UMODE_PERM`),檔案的部分**在記憶體檔案系統與磁碟上各做一次**。root 先建立測試用的檔案與目錄,再 `fork` 出降為一般使用者的子行程去嘗試:

| 一般使用者(1000)嘗試 | 結果 |
|---|---|
| 開啟 root 的 `0600` 檔案(讀或寫) | `E_ACCES`;但 `stat` 看得到它的屬性 |
| 開啟 root 的 `0644` 檔案:讀 / 寫 / 清空 | 成功 / `E_ACCES` / `E_ACCES` |
| `chmod`、`chown` 別人的檔案 | `E_PERM` |
| 在 root 的 `0755` 目錄裡建立、刪除、改名、連結 | `E_ACCES` |
| 經過 `0700` 的目錄去 `stat` 或開啟裡面的檔案;`chdir` 進去 | `E_ACCES` |
| 在 `0777` 的目錄裡建立檔案 | 成功;擁有者是自己,權限 `0644` |
| 把自己的檔案改成 `000` 再開啟 | `E_ACCES`;改回 `0600` 成功 |
| 把自己的檔案 `chown` 給 root | `E_PERM` |
| 執行沒有執行權限的檔案;加上權限後 | `E_ACCES`;`E_NOEXEC`(內容不是程式) |

| 另一個使用者(1001)對 1000 的 `0700` 檔案 | 結果 |
|---|---|
| 開啟、`chmod` | `E_ACCES`、`E_PERM` |

| 行程的身分與特權 | 結果 |
|---|---|
| 降為一般使用者後 `setuid(0)`、`setuid(別人)` | `E_PERM` |
| 一般使用者對 root 的行程送訊號;對自己 | `E_PERM`;成功 |
| root 只放棄 `CAP_FS` 之後開啟別人的 `0600` 檔案、`chown` | `E_ACCES`、`E_PERM`(仍然是 uid 0) |
| 放棄之後再查特權 | 沒有恢復 |
| 使用者 1001 終止使用者 1000 的行程(單一與整組) | `E_PERM`;行程還活著 |
| root 終止它 | 成功 |
| uid 超過 65535;負數 | `E_INVAL` |

跨開機測試新增:`disktest=1` 把檔案設成擁有者 1000:1001、權限 640;`disktest=2` 重新開機後讀回來相同。

既有的測試有三處因為權限而改變了預期(並補上對應的新情況):執行 `/etc/motd` 現在先得到 `E_ACCES`(沒有執行權限),加上權限後才是原本的 `E_NOEXEC`。

互動測試:

```
$ su alice
[uid 1000] $ id
uid=1000(alice) gid=1000 caps=none
[uid 1000] $ echo mine > /disk/a.txt
[uid 1000] $ ls -l /disk
-rw-r--r-- alice         5  2026-11-18 05:08  a.txt
[uid 1000] $ echo x > /etc/passwd
sh: cannot open /etc/passwd
[uid 1000] $ kill -9 1
kill: cannot signal 1
[uid 1000] $ su root
su: permission denied
[uid 1000] $ exit
$ id
uid=0(root) gid=0 caps=fs,kill,setid,admin
```

### 已知限制

- **沒有登入與密碼**:開機後就是 root。這是「特權只能減少」的直接結果,也表示不適合多人各自登入的用途。
- **每個行程只有一個群組**;沒有附加群組。
- **沒有 setuid 程式、sticky 位元、umask、存取控制清單**。`0777` 的目錄裡,任何人都可以刪除別人的檔案(Unix 以 sticky 位元避免)。
- **uid / gid 的上限是 65535**(磁碟上只有 16 位元)。
- **開啟之後不再檢查權限**:撤銷權限不影響已經開啟的檔案。
- **`/etc/passwd` 只是名稱對照表**,kernel 不讀它;`su` 一個不在表裡的數字編號也可以。
- **管線與裝置沒有個別的存取控制**(裝置一律 `0666`);`procinfo`、`sysinfo` 任何人都可以查詢所有行程。
- **資源沒有按使用者計量**(配額在下一步以行程為單位加入)。 **〔後續:第 35 步加入每個行程的記憶體配額。〕**

---

## 第 35 步:記憶體配額與匿名對映

### 目標

第 30 步的 heap 是「用到才配置」,而且 kernel 對每個要求都說好。真的用到、記憶體卻不夠時,只能終止那個行程(過度承諾,overcommit)。被終止的程式沒有做錯事,也沒有機會收拾。

對要求可靠的系統(這一步起對照航太飛行軟體的原則:**資源事先劃定,不在執行中途才發現不夠**),這個行為不能接受。這一步改成:

- **承諾記帳**:kernel 記著已經答應出去多少記憶體;每個要求當場成功或失敗
- **每個行程的配額**:一個行程失控,最多用掉自己的額度
- **保留給 kernel 的記憶體**:使用者行程不能把系統本身需要的部分要走
- **`mmap` / `munmap`**:可以個別歸還的大塊記憶體

### 承諾記帳

「承諾」(commit)= kernel 已經答應給行程、它隨時可以去用的頁數,**包括還沒真的配置的**。

| 要求 | 檢查 |
|---|---|
| `sbrk` 加大、`mmap` | 行程的承諾 + 這次要的 ≤ 行程的配額,**而且** 全系統的承諾總和 + 這次要的 ≤ 系統上限 |
| `fork` | 子行程需要和父行程一樣多的承諾(寫入時複製的最壞情況是每一頁都要另外一份) |
| `sbrk` 縮小、`munmap`、`exec`、行程結束 | 歸還 |

不通過就回傳 `E_NOMEM`,什麼都不改變。程式在「要」的時候得到明確的答案:`malloc` 回傳 `NULL`,可以釋放別的東西、改用較省的做法,或把工作有秩序地結束。**之後去使用那些已經答應的頁時不會再失敗**,因為答應的總量從來沒有超過實際有的。

頁本身仍然是用到才配置(第 30 步的機制不變):記帳決定「能不能」,配置決定「什麼時候」。所以要了很大一塊只用一點點的程式,佔用的是額度,不是實體記憶體。

系統上限 = 第一次使用時的空閒記憶體 − 保留量。保留量是全部記憶體的 1/16,至少 2MB,留給 kernel 自己:頁表、kernel heap、行程的 stack 與程式本體、磁碟快取。

### 配額

每個行程一個上限(預設 32MB),子行程沿用。`memlimit(kb)`:

- 調低:隨時可以
- 調高:需要 `CAP_ADMIN`(上一步的特權之一)

和上一步的特權是同一個原則:限制只會變嚴。shell 的內建命令 `limit [KB]` 設定 shell 自己的配額,之後啟動的每個命令都受它限制。

### 匿名對映

`mmap(len)` 回傳一塊內容為 0 的記憶體;`munmap(addr)` 整塊歸還。和 heap 的差別是每一塊各自獨立:heap 只有最尾端能還給系統,中間釋放的部分只能留在行程裡重複使用;對映可以任意順序歸還。

- 只支援**匿名**的(不對應任何檔案),所以稱為匿名對映
- 位址由 kernel 決定(另一段固定的範圍,在 heap 與 stack 之間),每個行程最多 16 塊
- 兩塊之間留一頁不對映:寫過頭會立刻出錯,而不是悄悄寫進下一塊
- 歸還之後那段位址不再重複使用(位址空間很大,簡單的做法就夠)

### 驗證

使用者程式的測試(`UMODE_MEMQUOTA`):

| 項目 | 結果 |
|---|---|
| 預設配額 | 32768 KB |
| 沒有 `CAP_ADMIN` 的行程調高配額 | `E_PERM`;調低成功 |
| 配額 2MB:`sbrk` 1MB;再要 2MB | 成功;`E_NOMEM`,結尾不變 |
| 把答應過的 1MB 每一頁都寫一次 | 全部成功 |
| `malloc` 100KB 一直要到失敗 | 第 10~20 次之間得到 `NULL`,行程沒有被終止;已得到的內容都正確 |
| 全部 `free` 之後再要 500KB | 成功 |
| `fork` 的子行程 | 配額相同,同樣被拒絕 |
| 子行程調低配額之後,父行程的配額 | 不變 |
| 配額調到 1GB(有特權)後要 200MB(機器只有 128MB) | `sbrk` 與 `mmap` 都當場 `E_NOMEM` |
| `mmap` 64KB | 位址對齊頁;還沒碰之前記憶體用量不變;內容全是 0 |
| `fork` 之後子行程改寫、歸還對映 | 父行程的內容不變 |
| `munmap` 對映中間的位址、別的位址、同一塊兩次 | `E_INVAL` |
| `munmap` 之後 | 記憶體用量立刻下降 |
| `mmap` 長度 0、1GB、超過配額 | `E_INVAL`、`E_INVAL`、`E_NOMEM` |
| 第 17 塊對映 | `E_NOMEM` |
| 碰已經歸還的對映;寫過一塊的結尾 | 行程被終止 |

互動示範:

```
$ limit 128
memory limit: 128 KB per process
$ sort /disk/big | wc
sort: out of memory            ← 程式自己印的:它得到了 NULL,有機會回報後結束
0 0 0
$ echo still running
still running
```

### 已知限制

- **保留量不是絕對的保證。** 記帳只涵蓋 heap 與匿名對映。行程的 stack(每條執行緒最多 1MB)、程式本體、頁表,以及記憶體檔案系統(ramfs)裡的檔案都不在帳上;它們用的是保留的那一塊。大量建立行程或在 `/` 底下寫入大檔案仍然可以把記憶體用完。
- **`fork` 之後馬上 `exec` 的常見用法,會短暫需要兩倍的承諾**,大型行程可能因此無法 `fork`(可以改用 `spawn`,它不需要)。
- **配額以行程為單位**,不是以使用者為單位;一個使用者可以開很多個行程。
- **`mmap` 不能對映檔案、不能指定位址與權限、不能只歸還一部分**;位址不重複使用。
- **`malloc` 沒有改用 `mmap` 配置大塊記憶體**,仍然全部來自 heap。
- **root 的 shell 可以把配額再調高**(它有 `CAP_ADMIN`);要讓限制不可逆,先用 `su` 降為一般使用者。
