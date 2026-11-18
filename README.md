# WElyOS

以標準 C(C11)從零開發的簡易作業系統,支援 **i386、RISC-V 64、AArch64** 三種架構,可在 QEMU(Quick Emulator) 上啟動,並可透過 GDB(GNU Debugger) 除錯追蹤。專案從最陽春的階段開始,逐步擴充。

> 開發過程的逐步紀錄見 [docs/DEVLOG.md](docs/DEVLOG.md),開頭附有專業名詞的中英對照表。

## 目前功能

- [x] 多架構:i386、RISC-V 64(QEMU virt)、AArch64(QEMU virt),架構相關程式集中於 `arch/`
- [x] multiboot 開機(i386,32 位元保護模式);裝置樹(Device Tree Blob,DTB)解析(RISC-V / AArch64)
- [x] 序列埠(COM1)輸出
- [x] CMake 建置,`run` / `debug` target
- [x] GDB 除錯:QEMU GDB stub(`-s -S`)加 `gdb-multiarch`,三種架構皆可下中斷點、單步、看暫存器
- [x] `kprintf` 格式化輸出(`%c %s %d %u %x %X %p`,寬度、補零、`l`/`ll`/`z`)
- [x] 最小字串函式庫(`memcpy`、`strlen` 等)
- [x] `panic()` 與 backtrace(三個架構;frame pointer 回溯,`scripts/backtrace.sh` 解析成函式與行號)
- [x] 例外處理(三個架構):RISC-V `stvec`、AArch64 向量表、i386 GDT(Global Descriptor Table) + IDT(Interrupt Descriptor Table);印出原因、暫存器與 backtrace,可恢復的例外(中斷點)能繼續執行
- [x] VGA(Video Graphics Array) 文字螢幕(80x25、顏色、硬體游標、自動捲動),`kprintf` 同步輸出到序列埠與螢幕
- [x] 讀取 multiboot 資訊(開機參數、記憶體大小)
- [x] 計時器中斷(1000Hz tick):RISC-V SBI(Supervisor Binary Interface) timer、AArch64 GICv2 + generic timer、i386 PIC(Programmable Interrupt Controller) + PIT(Programmable Interval Timer),並以 TSC(Time Stamp Counter)校正;中斷延遲時會補記遺失的 tick,不累積誤差
- [x] 實體記憶體管理(位元圖頁面配置器,`pmm_alloc`)與 kernel heap(`kmalloc` / `kfree`,first-fit 加相鄰合併,可偵測重複釋放)
- [x] 核心執行緒與排程器:三個架構的 context switch、round-robin、計時器搶佔(時間片 10ms)、`thread_yield` / `thread_sleep`
- [x] 同步機制:等待佇列、semaphore、mutex(偵測重複上鎖與非擁有者解鎖)、`thread_join`
- [x] 分頁(Memory Management Unit,MMU):i386 PAE、RISC-V Sv39、AArch64 4KB/39 位元,三者皆為 3 層;恆等對映、大分頁(2MB)與自動拆分、W^X(Write XOR Execute,程式碼唯讀、資料不可執行)、NULL 指標偵測
- [x] kernel stack 保護頁與獨立的例外 stack:溢位在發生當下被攔下並回報(i386 以 TSS(Task State Segment) 加 double fault 工作閘);閒置 stack 快取重複使用
- [x] 優先權排程:8 級固定優先權、搶佔式、同級輪流;mutex 的優先權繼承(防止優先權反轉)
- [x] 使用者模式與行程:每個行程獨立的位址空間、系統呼叫(i386 `int 0x80`、RISC-V `ecall`、AArch64 `svc`)、行程內多執行緒、`proc_spawn` / `proc_wait` / `proc_kill`;使用者程式以 C 撰寫、獨立連結後嵌入 kernel;出錯只終止該行程
- [x] 排程補強:防飢餓(老化)、完整的優先權繼承(多把鎖、連鎖等待)
- [x] 記憶體保護補強:i386 改用 PAE(Physical Address Extension) 分頁取得 NX(No-eXecute);SMEP(Supervisor Mode Execution Prevention) / SMAP(Supervisor Mode Access Prevention,i386)與 PAN(Privileged Access Never,AArch64)禁止 kernel 存取或執行使用者的頁;ASID(Address Space Identifier) 與全域頁避免切換行程時清空 TLB(Translation Lookaside Buffer)
- [x] futex 與使用者模式的 mutex
- [x] 以 `-O2` 編譯(`-DWELYOS_OPT=-O0` 可改回,方便 GDB 逐行追蹤)
- [x] 多核心第一段:spinlock(偵測重複上鎖、錯誤解鎖、持有時切換執行緒)、每顆 CPU 的狀態(`struct cpu`,RISC-V 用 `tp`、AArch64 用 `TPIDR_EL1`、i386 用工作暫存器 TR(Task Register) 取得)、所有共用資料改以鎖保護;目前仍只啟動一顆 CPU
- [x] 可替換的執行佇列(`include/runq.h`):`ring` 每顆 CPU 一組無鎖(lock-free)環狀佇列加工作竊取(預設)、`list` 全系統一組有鎖的串列,以 `-DWELYOS_RUNQ=` 選擇;主機端多執行緒測試(含 ThreadSanitizer)
- [x] 多核心第二段:啟動其他 CPU(最多 8 顆)— RISC-V 以 SBI HSM(Hart State Management)、AArch64 以 PSCI(Power State Coordination Interface)、i386 以 ACPI(Advanced Configuration and Power Interface)找出 CPU 並用 LAPIC(Local Advanced Programmable Interrupt Controller)的 INIT / SIPI(Startup Inter-Processor Interrupt)加真實模式跳板啟動;各 CPU 完成初始化與並行測試後待命
- [x] 多核心第三段:其他 CPU 參與排程 — 每顆 CPU 自己的計時器(i386 用 LAPIC 計時器)、處理器間中斷(Inter-Processor Interrupt,IPI)、跨 CPU 呼叫與 TLB shootdown、panic 時停止其他 CPU;執行緒與行程真正同時在多顆 CPU 上執行
- [x] 檔案系統:VFS(Virtual File System)介面、記憶體檔案系統(ramfs)、開機檔案包(tar 嵌入 kernel,開機時解開)、裝置檔案 `/dev/console`;系統呼叫 `open` / `close` / `read` / `write` / `readdir` / `mkdir` / `unlink` / `stat` 與每個行程的檔案描述符表;使用者程式改由 `/bin/init` 載入
- [x] 行程:多個使用者程式(`/bin/init`、`echo`、`cat`、`ls`)、`main(argc, argv)` 參數、`spawn` / `wait` / `exec` 系統呼叫、建立行程時繼承檔案描述符(可重新導向輸出);相同程式的行程共用程式碼頁,正在執行的程式檔不可修改
- [x] 主控台輸入(序列埠,含行編輯:退格、Ctrl-C、Ctrl-D)與 shell(`/bin/sh`):執行 `/bin` 下的程式、`>` / `>>` 重新導向、引號、script 檔;另有 `mkdir`、`rm`;`dup2` 系統呼叫。不帶 `autoexit` 開機時直接進入 shell
- [x] 三個架構相同的 WElyOS 文字開機標誌(由 kernel 印出;RISC-V 使用拿掉自身標誌的 OpenSBI)
- [x] 可恢復的 page fault:寫入時複製(copy-on-write,每頁有參考計數)、`fork` 系統呼叫、使用者 stack 自動成長(每條最多 1MB)
- [x] 管線:`pipe` 系統呼叫、shell 的 `命令 | 命令` 與 `< 檔案`;新增 `wc`;終止行程時會叫醒它所有正在睡的執行緒
- [x] 行程的父子關係(只能 `wait` 自己的子行程、`waitpid` 可等任何一個、孤兒行程自動回收)、訊號(`kill`:SIGINT / SIGKILL / SIGTERM,效果都是終止)、行程群組與主控台的前景群組;Ctrl-C 會終止執行中的命令(含整條管線);新增 `sleep`、`kill`
- [x] 中斷驅動的主控台輸入(i386 的 PIC IRQ 4、RISC-V 的 PLIC、AArch64 的 GIC 共用中斷);i386 螢幕改用硬體捲動;shell 的背景執行(`命令 &`、`jobs`、`fg`、`wait`);`ps` 列出行程;`waitpid` 的 `WNOHANG`、`procinfo` 系統呼叫、`/dev/null`;互動使用時 shell 結束即關機
- [x] 工作控制:暫停與繼續(`SIGSTOP` / `SIGTSTP` / `SIGCONT`)、Ctrl-Z 暫停前景的命令、shell 的 `bg` 與可繼續暫停工作的 `fg`、`waitpid` 的 `WUNTRACED`;只有前景可以讀鍵盤(背景行程讀取時被暫停);`kill -STOP` / `-CONT`
- [x] 磁碟:區塊裝置介面與輪詢式驅動程式(i386 的 ATA PIO;RISC-V / AArch64 的 virtio-blk);磁碟上的檔案系統 WFS(位元圖、inode、間接區塊、區塊快取),掛載在 `/disk`,內容跨開機保留;VFS 的掛載;沒有正常關機時開機自動檢查並修復;`sync` / `statfs` 系統呼叫;新增 `cp`、`df`、`sync`
- [x] `top`(類似 htop):全螢幕即時顯示各 CPU 的使用率、記憶體與磁碟用量、每個行程的 CPU% 與記憶體,可選取行程暫停、繼續、終止;`sysinfo` 系統呼叫與每顆 CPU 的忙碌 / 閒置記帳、行程的記憶體用量、主控台的 raw 輸入模式(`tcsetmode`)、i386 VGA 螢幕支援游標與顏色的控制序列
- [x] 檔案介面補齊:`lseek`、`rename`(可搬移目錄,防止搬進自己底下)、硬連結、目前的工作目錄(`chdir` / `getcwd`、相對路徑);shell 的 `cd`、`pwd` 與顯示目錄的提示符號;新增 `mv`
- [x] 使用者 heap(`sbrk`:用到才配置、縮小時歸還)與小型 C 函式庫 `user/libc.c`(`malloc` / `free` / `realloc`、`printf` 系列、字串函式;沒用到的部分不會連結進程式);新增 `sort`
- [x] 訊號處理函式:行程可以忽略訊號或登記處理函式(暫存器存到使用者 stack、`sigreturn` 還原且不採用特權欄位)、`SIGUSR1` / `SIGUSR2`、等待中的系統呼叫被訊號打斷時回傳 `E_INTR`
- [x] 日期時間:開機時讀取硬體時鐘(i386 的 CMOS RTC、RISC-V 的 Goldfish RTC、AArch64 的 PL031)、`time` 系統呼叫、檔案的修改時間(磁碟上跨開機保留);新增 `date`、`ls -l`
- [x] 磁碟錯誤的處理:可注入錯誤的區塊裝置包裝層(測試用)、寫入失敗後檔案系統轉為唯讀並於下次掛載時檢查
- [x] 使用者與權限:行程的 uid / gid 與四項可個別放棄的特權(只能減少、不能增加,所以不需要密碼)、檔案的擁有者與權限位元(VFS 檢查;磁碟上的 inode 不增加大小且相容舊映像)、送訊號的限制;新增 `id`、`su`、`chmod`、`chown`,`ls -l` 顯示權限與擁有者
- [x] 記憶體配額:承諾記帳(超過行程配額或系統上限的要求當場回報 `E_NOMEM`,不再事後終止行程)、保留給 kernel 的記憶體、`memlimit` 與 shell 的 `limit`;匿名對映 `mmap` / `munmap`
- [x] 網路(到 UDP):網路卡驅動(RISC-V / AArch64 的 virtio-net、i386 的 NE2000)、協定堆疊(乙太網路、ARP、IPv4、ICMP、UDP;純 C、不配置記憶體、主機端以 sanitizer 測試,由 Codex 實作)、整合層與以檔案描述符表示的 UDP socket;新增 `ping`、`net`、`tftp`(可從主機下載檔案)
- [x] DNS 查詢:`host`(訊息的組合與解析 `user/dns.h` 由 Codex 實作並以 sanitizer 測試)
- [x] 可靠性之一:看門狗(CPU 互相監看心跳,卡死時 panic)、黑盒子(磁碟最後 8KB 的事件紀錄,每筆有 CRC-32,panic 時以不上鎖的緊急路徑寫入,重新開機後可讀)、`supervise`(程式當掉時重新啟動,退避、有上限)、`faultlog`
- [x] 可靠性之二:磁碟每個區塊的 CRC-32(讀取時核對,損壞的資料不會交給程式)、superblock 兩份(主要的壞了用備份並修復,而不是誤判成新磁碟而格式化)、背景巡檢與 `scrub` 命令、位元翻轉的錯誤注入;磁碟格式 WFS2(舊格式仍可掛載)
- [x] 靜態檢查(由 Codex 實作):`scripts/static-check.sh` — 快速的規則掃描(遞迴、未檢查的錯誤回傳、禁用函式等)與按需的 GCC `-fanalyzer`;已據此修正 `printf` 寬度的溢位等缺陷
- [x] 可靠性之三:開機失敗計數(黑盒子記錄每次開機是否順利完成)與安全模式(連續 3 次失敗後只啟動最少的功能:不掛載磁碟、不啟動網路、單一 CPU,但仍可用 `faultlog` 診斷)

## 規劃中

- [ ] 多核心(Symmetric Multiprocessing,SMP)效能:縮小排程器的大鎖、喚醒時考慮上次執行的 CPU、mutex 先自旋再睡
- [ ] TCP;i386 的 PS/2 鍵盤;檔案系統的日誌(journaling)、以磁碟為根目錄
- [ ] 可靠性(參考航太飛行軟體的做法):期限排程;三重執行加投票
- [ ] 結構整理:中斷與裝置的註冊介面、kernel 與使用者共用的定義檔、拆分 `proc.c` / `main.c`、區塊快取獨立成一層

## 需求

- CMake ≥ 3.20、make
- 各架構的編譯器(WSL/Ubuntu):
  - i386:`gcc`(支援 `-m32`)
  - RISC-V 64:`sudo apt install gcc-riscv64-unknown-elf`
  - AArch64:`sudo apt install gcc-aarch64-linux-gnu`
- 除錯:`sudo apt install gdb-multiarch`
- QEMU(`qemu-system-i386` / `qemu-system-riscv64` / `qemu-system-aarch64`)。在 WSL 下可使用 Windows 版 QEMU,腳本會自動轉換路徑;也可用環境變數 `QEMU` 指定。

## 建置與執行

以 preset 選擇架構(`i386`、`riscv64`、`aarch64`),各自輸出到 `build/<架構>/`:

```
cmake --preset riscv64
cmake --build build/riscv64                    # 只建置
cmake --build build/riscv64 --target run       # 建置並在 QEMU 執行自我測試(序列埠輸出,自動結束)
cmake --build build/riscv64 --target debug     # 啟動 QEMU 並暫停,等待 GDB(tcp::1234)
cmake --build build/riscv64 --target gdb       # (另一個終端機)連上 gdb,輸入 welyos-start 開始
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit"              # 只開機,不跑測試(最快)
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit selftest"     # 開機並執行各子系統的自我測試與示範
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit crash=fault"  # 故意觸發錯誤(crash=panic|fault|illegal|break|dfree|stack|mainstack|thread|relock|null|wtext|exec|uaccess|uexec|spin|spinsleep|smp|wdog)
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit memtest"      # 完整記憶體測試:寫入並驗證每一個空閒頁
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit stress"       # 排程器壓力測試:建立 1000 條執行緒
cmake --build build/i386    --target run-gui   # 僅 i386:開啟 QEMU 視窗顯示 VGA 畫面
cmake --preset riscv64 -DWELYOS_RUNQ=list      # 換用另一種執行佇列的實作(預設 ring)
bash scripts/host-test.sh                      # 在主機上以多條執行緒測試執行佇列(不需要 QEMU)
bash scripts/regress.sh                        # 回歸測試:三個架構各 21 個情境加上磁碟跨開機測試(九次開機)與主機端的網路測試,核對結束碼(-r 6 另外重複測試,-q 只跑基本情境)
bash scripts/build-opensbi.sh                  # (選用)重新建置 RISC-V 的開機韌體;換 OpenSBI 版本時才需要
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf                                # 開機後進入 shell(輸入 help 看有哪些命令;top 看即時的系統狀態,q 離開)。Ctrl-C 終止、Ctrl-Z 暫停執行中的命令(fg / bg 繼續);輸入 exit 關機;Ctrl-] 強制結束 QEMU
python3 scripts/type-input.py 1.5 0.01 'ls /\nexit\n' | bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit shell"   # 自動化:模擬打字餵給 shell
QEMU_SMP=8 bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit selftest"   # 指定 CPU 數(預設 4,最多 8;1 = 單核心)
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "safe"                 # 強制以安全模式開機(連續 3 次開機失敗時會自動進入)
QEMU_NET=none bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf                  # 不接網路卡(預設接上;QEMU_TFTP=目錄 可指定以 tftp 分享給客體的主機目錄,預設 rootfs/)
bash scripts/host-test-net.sh                  # 在主機上測試網路的協定堆疊(不需要 QEMU)
bash scripts/static-check.sh                   # 靜態檢查(快速模式約 2 秒;--mode analyze 以 GCC 分析器檢查三個架構)
bash scripts/static-refresh.sh                 # 處理完新增的提示之後,更新回歸測試用來比較的參考報告
QEMU_DISK=none bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf                 # 不接磁碟(預設接上 build/<架構>/disk.img,掛載在 /disk;QEMU_DISK=路徑 可指定別的映像檔)
```

## 專案結構

```
arch/<名稱>/  各架構的開機程式、linker script、驅動(i386 / riscv64 / aarch64)
include/      與架構無關的標頭(arch.h 為架構介面;cred.h 為身分與特權;net.h 為網路的介面)
kernel/       kernel 主體(main、kprintf、fdt、panic、timer、pmm、kmalloc、sched、runq_ring / runq_list、spinlock、smp、sync、vm、proc、vfs、ramfs、wfs、virtio_blk、virtio_net、blkfault、netif、blackbox、watchdog、initrd、console、pipe;net/ 為網路的協定堆疊)
lib/          最小字串函式庫、CRC-32
cmake/        各架構的 toolchain 檔
user/         使用者程式(各自獨立連結後放進開機檔案包的 /bin/):init(測試)、sh(shell)、echo、cat、ls、wc、sort、date、mkdir、rm、cp、mv、sleep、kill、ps、top、df、sync、id、su、chmod、chown、ping、net、tftp、host、faultlog、supervise、scrub;crt0.c 為共用的啟動程式,libc.c / libc.h 為小型 C 函式庫,ulib.h 為較早的小工具
firmware/     RISC-V 的開機韌體(拿掉自己標誌的 OpenSBI;說明見 firmware/README.md)
rootfs/       開機檔案包的內容(建置時打包成 tar 嵌入 kernel,開機時解開到檔案系統)
tests/host/   在主機上執行的測試(執行佇列的並行測試;net/ 為網路堆疊的測試)
scripts/      QEMU 啟動腳本、GDB 腳本與設定、回歸測試(regress.sh)、主機測試、模擬打字(type-input.py)
docs/         開發紀錄
```
