# 開機韌體

RISC-V 開機時最先執行的不是 kernel,而是 M-mode 的韌體 OpenSBI(Open Source Supervisor Binary Interface)。QEMU 內附的版本會在開機時印出一個 "OpenSBI" 的大字標誌。

WElyOS 的開機標誌由 kernel 自己印出,三個架構完全相同(`kernel/main.c` 的 `boot_logo`)。RISC-V 若再多一個韌體的標誌,就和另外兩個架構不一致,所以這裡放的是**拿掉那個標誌**之後重新建置的 OpenSBI。

| 檔案 | 說明 |
|---|---|
| `opensbi-riscv64-welyos.bin` | 建置好的韌體,`scripts/qemu.sh` 預設使用它 |
| `LICENSE-OpenSBI.txt` | OpenSBI 的授權條款(BSD 2-Clause),隨韌體一併散布 |

## 內容與原版的差異

以 OpenSBI **v1.3** 的原始碼建置,只改了一處:拿掉 `lib/sbi/sbi_init.c` 裡印出標誌的那一行。

其餘功能與行為完全沒有更動。韌體仍會印出它的版本(`OpenSBI v1.3`)與平台資訊,如實標示執行的是哪個韌體。

## 為什麼是 v1.3 而不是 QEMU 內附的 v1.4

v1.4 起韌體一律建置成「可重新定位」的形式,連結時需要工具鏈支援;建置 kernel 用的裸機工具鏈(`riscv64-unknown-elf-`)沒有這項功能。v1.3 可以建置成固定位址的版本。WElyOS 用到的 SBI 功能(計時器、處理器間中斷、啟動其他 hart)兩個版本都有。

若之後安裝了 `gcc-riscv64-linux-gnu`,可以把 `scripts/build-opensbi.sh` 裡的版本與工具鏈前綴改掉,改用 v1.4。

## 重新建置

```
bash scripts/build-opensbi.sh
```

會下載 OpenSBI 的原始碼到 `build/opensbi-src`(不進版本庫)、套用上述修改、建置並覆蓋本目錄的 `.bin`。一般不需要執行,只有要換 OpenSBI 版本時才需要。

## 改回 QEMU 內附的韌體

```
QEMU_BIOS=default bash scripts/qemu.sh riscv64 build/riscv64/welyos.elf
```

這樣開機時會先出現 OpenSBI 自己的標誌,再出現 WElyOS 的。
