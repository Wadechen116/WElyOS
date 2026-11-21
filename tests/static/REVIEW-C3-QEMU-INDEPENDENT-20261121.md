# c2+c3 固定版本獨立 QEMU 驗收

固定包：`build/milestones/c32-c3-exec/`。root 複製三 ELF 與實際由
AArch64 載入的 .bin 到 Windows Temp 隔離目錄，逐架構順序執行
`autoexit selftest` 與 `autoexit memhog=fork`；4CPU、無磁碟、無網路。

六次均 rc0，必要 guest markers 齊全，無 PANIC，boot hash 前後一致。
selftest 包括 STACK255、IMAGE8、exec transaction 與使用者記憶體回收；
memhog 皆報 fork exit0。boot/log SHA-256、原始 log 與結果在
`/mnt/c/Users/user/AppData/Local/Temp/codex-welyos-c3-wpgyyzlt/independent-results.json`。
執行命令與環境設定保存在 `/tmp/welyos-c3-acceptance-runner.py`；結果 JSON
本身沒有命令欄位。

初次沙箱阻止 WSL 啟動 Windows QEMU，沒有 guest 輸出，不計產品失敗或
成功；其後經批准沙箱外完成上述六次。已通知 Claude QEMU 時段結束。

此結果支持固定 binary 的已測功能，不能取代 exec 憑證單一 new_vs 綁定
契約與逐失敗路徑審查。該項獨立審查仍進行中，因此本報告不宣告(c)整體
驗收完成。來源重建與 owner 證據另見 REVIEW-C3-EVIDENCE-20261121.md；
遞迴 baseline 另由 fork reviewer 窄幅核對。
