# 176 stackfix 固定里程碑獨立驗證

正式 `build/milestones/c32-b2-stackfix/manifest.txt` 的 15 個來源及四個
boot 檔完整 SHA-256 全符合；boot 檔均無 write bits。六份 owner QEMU log
與 manifest 所列 hash 前綴符合（原 manifest 只提供 16 位前綴）。
完整 regress-sr3 hash 為
`947ce1c27e0ee88e5980433d868cc6d35d4e3f322a3ec998c9fcea5da3b51e9e`，
尾列「全部符合預期」。

root 在正式包公布前，已將目前三 ELF 與 AArch64 .bin 保存為唯讀觀察
副本，並在 sr3 完成後獨立跑三架構 QEMU。收到 176 後逐 byte 比對，
這四份副本與正式固定包完全相同，因此可將獨立執行證據綁到正式
boot hash，無須重跑。觀察來源 hash 本身不是建置證明；來源對應仍依
owner manifest 聲明及本次完整核對，不冒稱獨立重建了同一 binary。

| 架構 | 獨立 rc | Boot SHA-256 | 獨立 log SHA-256 |
| --- | --- | --- | --- |
| i386 | 0 | 0bcd88ced2510b40d04295e4762c84fc1d67cc251eacd9c50fa818af00deb107 | f3915049da2904d70599a059b00af325f23d8104c2a8ee796f02037b4fa637e5 |
| riscv64 | 0 | 7c74fe160d87ed08e5853db52ed1d96a2cc4b408a9be0335057ea81477a3819f | 1b265364761778f8020488d818b9cc462a00ac28040d51a07924ed048c294b10 |
| aarch64 (.bin) | 0 | 03aaca9f1c60d51be71b63e5f1f352d69dac4cddc9bed9de5ea7d63b2f56c581 | cb0ec568379931b574e6e4152f09f93a106e570adf4d51b3216680e0e6bb2f95 |

每次 4CPU、無磁碟、無網路、`autoexit selftest`。domain、匿名 fork tickets、
thread KOBJ、pipe creator lifetime、user memory reclaimed、20 次 thread
建立失敗 slots 0x1→0x1，以及 guest exit0 markers 皆有；boot 執行前後未變。
最初沙箱 WSL socket 失敗且無客體輸出的 wrapper rc0 被排除；之後經批准
沙箱外執行 Windows QEMU。另一次 i386 直接顯示的診斷執行也成功，但本表
僅採保存完整 log 的三次執行。

完整證據：
`/mnt/c/Users/user/AppData/Local/Temp/codex-welyos-current-x_bgjsbp/`
內的 `observed-manifest.json`、`formal-manifest-check.json`、
`independent-results.json`、`*-independent.log` 及唯讀 boot 副本。

本次未獨立跑 memhog=fork；其三架構成功為 owner 固定 log 證據，且不能
替代 stack 預留保證。host pthread 測試亦未轉化成新的 QEMU 故障注入。
sr2 嚴格週期斷言失敗仍為 I-331：sr3 與本次成功提供後次穩定性證據，
不代表定位或消除 sr2 根因。STACK/IMAGE/exec、I-330、RAMFS、暫時物件
及精確 pipe KOBJ 仍 open。未改產品、未 commit。
