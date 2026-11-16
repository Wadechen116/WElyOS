# WElyOS

以標準 C(C11)從零開發的簡易作業系統,支援 **i386、RISC-V 64、AArch64** 三種架構,可在 QEMU 上啟動,並可透過 GDB 除錯追蹤。專案從最陽春的階段開始,逐步擴充。

> 開發過程的逐步紀錄見 [docs/DEVLOG.md](docs/DEVLOG.md)。

## 目前功能

- [x] 多架構:i386、RISC-V 64(QEMU virt)、AArch64(QEMU virt),架構相關程式集中於 `arch/`
- [x] multiboot 開機(i386,32 位元保護模式);裝置樹(DTB)解析(RISC-V / AArch64)
- [x] 序列埠(COM1)輸出
- [x] CMake 建置,`run` / `debug` target
- [x] GDB 除錯:QEMU GDB stub(`-s -S`)加 `gdb-multiarch`,三種架構皆可下中斷點、單步、看暫存器
- [x] `kprintf` 格式化輸出(`%c %s %d %u %x %X %p`,寬度、補零、`l`/`ll`/`z`)
- [x] 最小字串函式庫(`memcpy`、`strlen` 等)
- [x] `panic()` 與 backtrace(三個架構;frame pointer 回溯,`scripts/backtrace.sh` 解析成函式與行號)
- [x] 例外處理(三個架構):RISC-V `stvec`、AArch64 向量表、i386 GDT + IDT;印出原因、暫存器與 backtrace,可恢復的例外(中斷點)能繼續執行
- [x] VGA 文字螢幕(80x25、顏色、硬體游標、自動捲動),`kprintf` 同步輸出到序列埠與螢幕
- [x] 讀取 multiboot 資訊(開機參數、記憶體大小)

## 規劃中

- [ ] 計時器中斷
- [ ] 實體記憶體管理、heap
- [ ] 排程器、系統呼叫、使用者模式、shell

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
cmake --build build/riscv64 --target run       # 建置並在 QEMU 執行(序列埠輸出,自動結束)
cmake --build build/riscv64 --target debug     # 啟動 QEMU 並暫停,等待 GDB(tcp::1234)
cmake --build build/riscv64 --target gdb       # (另一個終端機)連上 gdb,輸入 welyos-start 開始
bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf -append "autoexit crash=fault"  # 故意觸發錯誤(crash=panic|fault|illegal|break)
cmake --build build/i386    --target run-gui   # 僅 i386:開啟 QEMU 視窗顯示 VGA 畫面
```

## 專案結構

```
arch/<名稱>/  各架構的開機程式、linker script、驅動(i386 / riscv64 / aarch64)
include/      與架構無關的標頭(arch.h 為架構介面)
kernel/       kernel 主體(main、kprintf、fdt)
lib/          最小字串函式庫
cmake/        各架構的 toolchain 檔
scripts/      QEMU 啟動腳本、GDB 腳本與設定
docs/         開發紀錄
```
