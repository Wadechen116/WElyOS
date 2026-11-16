# WElyOS

以標準 C(C11)從零開發的簡易作業系統,可在 QEMU 上啟動,並可透過 GDB 除錯追蹤。專案從最陽春的階段開始,逐步擴充。

> 開發過程的逐步紀錄見 [docs/DEVLOG.md](docs/DEVLOG.md)。

## 目前功能

- [x] multiboot 開機(i386,32 位元保護模式)
- [x] 序列埠(COM1)輸出
- [x] CMake 建置,`run` / `debug` target
- [x] QEMU GDB stub 除錯入口(`-s -S`)
- [x] `kprintf` 格式化輸出(`%c %s %d %u %x %X %p`,寬度與補零)

## 規劃中

- [ ] VGA 文字螢幕
- [ ] GDB 除錯流程(`.gdbinit`)
- [ ] GDT / IDT、例外處理、panic
- [ ] 計時器中斷
- [ ] 實體記憶體管理、heap
- [ ] 排程器、系統呼叫、使用者模式、shell

## 需求

- gcc(支援 `-m32`)、binutils、CMake ≥ 3.20、make
- QEMU(`qemu-system-i386`)。在 WSL 下可使用 Windows 版 QEMU,腳本會自動轉換路徑;也可用環境變數 `QEMU` 指定。

## 建置與執行

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i386.cmake
cmake --build build                    # 只建置
cmake --build build --target run       # 建置並在 QEMU 執行
cmake --build build --target debug     # 啟動 QEMU 並等待 GDB(tcp::1234)
```

## 專案結構

```
arch/i386/    開機程式與 linker script
kernel/       kernel 主體(main、serial、kprintf)
include/      標頭檔(io.h、serial.h、kprintf.h)
cmake/        toolchain 檔
scripts/      QEMU 啟動腳本
docs/         開發紀錄
```
