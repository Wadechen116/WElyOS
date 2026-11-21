# 目前觀察快照：獨立 QEMU 執行證據核對

root 已完成三架構 QEMU。本 agent 沒有自行重跑，只讀重新計算固定副本與 log hash、核對 marker 和 results。本輪沒有改產品、共享 build 或 runner 結果。

證據根目錄：`/mnt/c/Users/user/AppData/Local/Temp/codex-welyos-current-x_bgjsbp/`，包含 `observed-manifest.json`、`independent-results.json`、三份 `*-independent.log`、四份 boot artifacts 與所複製 regress 證據。

## 本輪獨立核對

`observed-manifest.json` 的九個複製檔案 hash 全符合：三 ELF、AArch64 `.bin`、sr2/sr3 summary 與三個原 selftest log。下列實際 boot artifact hash 與 runner 記錄也全符合；runner 報告 `changed_boot=false`。

| 架構 | rc | boot SHA-256 | independent log SHA-256 |
| --- | --- | --- | --- |
| i386 | 0 | `0bcd88ced2510b40d04295e4762c84fc1d67cc251eacd9c50fa818af00deb107` | `f3915049da2904d70599a059b00af325f23d8104c2a8ee796f02037b4fa637e5` |
| RISC-V | 0 | `7c74fe160d87ed08e5853db52ed1d96a2cc4b408a9be0335057ea81477a3819f` | `1b265364761778f8020488d818b9cc462a00ac28040d51a07924ed048c294b10` |
| AArch64 | 0 | `03aaca9f1c60d51be71b63e5f1f352d69dac4cddc9bed9de5ea7d63b2f56c581`（實際載入 .bin） | `cb0ec568379931b574e6e4152f09f93a106e570adf4d51b3216680e0e6bb2f95` |

三份 log 皆實際含有七項必要 marker，且無 `PANIC`：membudget selftest、anonymous fork ticket、thread KOBJ、pipe/file creator lifetime、user memory reclaimed、20次建立失敗 slots 0x1→0x1，以及 guest `exit: code 0 to the emulator`。20次建立失敗行也記 pages 6→6。這支持其實際測到的普通 selftest 路徑，不是只依 wrapper rc0 判定。

依 root runner 說明，使用四CPU、無disk/net 的 `autoexit selftest`。初次沙箱 WSL socket 問題沒有 guest 輸出，已排除；上表實際執行經批准在沙箱外啟動 Windows QEMU。這些執行由 root 完成，本 agent 僅核對留下的證據。

## 成功與歷史失敗均保留

固定複本 `build/c32a/regress-sr3.txt` hash 為 `947ce1c27e0ee88e5980433d868cc6d35d4e3f322a3ec998c9fcea5da3b51e9e`，全文尾「全部符合預期」。它是新成功記錄，不代表 sr2 的原因已定位。

固定複本 sr2 hash `326ed3dad84ed1b910fb75a778a91efda9ae1cac1221755e811abccde0504b2b` 的完整失敗結果繼續保存。該次 i386 selftest wrapper rc3、AArch64 rc1 仍需分析；共享 log 被後次覆寫的問題不能靠 sr3 PASS 回填。現行 sched_test 的變動只加診斷、沒有放寬零miss／零overrun斷言，詳見 `CONTINUOUS-REVIEW-STATUS-20261121.md`。

## 證據邊界

manifest 明載：`Observed copies only; no claim these sources produced these binaries or that copied logs came from these exact copies.` 來源欄名也是 `observed_sources_not_build_attestation`。因此本報告不能將觀察來源 hash 當 build attestation，不能宣稱對 Claude 174/175 的精確來源完成正式 binary 驗收。正式新 immutable milestone／來源建置對應仍待 Claude 提供。

本輪只有固定普通 selftest，沒有新增 pthread 的逐頁失敗／交錯故障注入，沒有全 fork 配置失敗點測試，也不閉合 STACK/IMAGE/exec、暫時物件 KOBJ、RAMFS、I-330。20次失敗中具體觸發點強度仍須依原測試限制判讀，不能聲稱20次都是post-stack prepare失敗。pipe精確成本斷言缺口也不因 log 寫4頁而自動消失。
