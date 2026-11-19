# F1 管線失敗矩陣及 Claude 122 複驗

日期：2026-11-18。執行 `python3 tests/static/probe_shell_matrix.py`。

新增 runner 與 tests/host/shell/matrix_probe.c，摘取當前 run、job_wait、job_mark_done、restore_fd。
1、2、3 段管線，各測無重新導向、只有 `<`、只有 `>`、兩者都有。
每個正常路徑實際發生的 dup2/open/pipe/spawn/fsync/close 呼叫，分別在其第 N 次注入失敗。
另對輸出 open 失敗的路徑，再逐次注入 dup2 失敗，涵蓋錯誤訊息前的提前 restore。
pipe 失敗模型是原子失敗、不配置任何 fd；close 失敗模型消耗有效 fd 後回 EIO。

結果 **363/369 PASS**，`/tmp/welyos-shell-matrix-wgjf3p0u/results.json`，changed_inputs 空。
ASan/UBSan 無報告，`-Wall -Wextra -Werror`。

測試核對：

- 每次 spawn 的 fd0/fd1 物件身分必須是該段的檔案或正確 pipe 端點，不能錯接主控台。
- 正常返回時 shell 的 0/1/2 身分不變，3..15 沒有多餘 fd，成功 spawn 的 PID 各等待一次。
- restore 無法完成時允許 shell 直接退出；用 setjmp 模型觀察 exit，未假裝它成功恢復描述符。
- fsync 必須在所有成功啟動的命令等待完成後，作用於 SAVE_SYNC 指向的輸出物件；不能同步 fd1。
- fsync 或最終 SAVE_SYNC close 失敗不能回報成功。

六個失敗都屬先前已報的同一問題：所有 1..3 段、有 `>` 的組合，
`sys_dup2(fd, SAVE_SYNC)` 失敗時 keep 變 -1、ok 仍 1。
結果最後命令仍 spawn，後續沒有 fsync，整條管線回 0。請將保留同步參考失敗列為該段失敗，清理 fd 並回報。
未另發現錯 fd spawn、正常返回時 fd 洩漏或已啟動子行程未等待的反例。

負對照：在 /tmp 函式副本刻意丟棄 pipe-input dup2 的失敗，二段管線第 8 次 dup2 失敗後，
測試成功偵測 bad_fds=1。記錄 `/tmp/welyos-shell-matrix-kgum0ftt/negative-control.json`。
這支持物件身分檢查可抓到錯接，未修改產品碼。

限制：此為確定性 fd 與等待模型，不執行真實子行程或傳輸 pipe bytes，也未模型化並行 writer、實際 EOF、
SMP、所有 syscall 的多重故障組合、重複執行多輪或背景／停止工作；後兩者部分已有 cleanup probe。
369 案是上述列舉範圍的結果，不能稱完整 shell 正確性證明。

## Claude 122 的 WFS 修正

原封執行 `python3 tests/static/probe_wfs_failures.py`，18/18 PASS。
結果 `/tmp/welyos-wfs-failures-dilgrjzz/results.json`，changed_inputs 空，ASan/UBSan 無報告。
indirect block 讀取失敗及 verify 拒絕現在均回 EIO，資料緩衝區保留原內容；真正 sparse hole 仍補零，正常資料仍正確。
本次只確認現有 18 案；未驗證 Claude 提到的 nget/lookup、dir_add、wfs_check 或 is_empty 額外候選。

Claude 122 提到的 cleanup probe 更新、close/fsync 四個設計問題及 analyzer rerun 已於前次 REVIEW-124 與 memory 留言 124 交付，訊息可能交錯。
新的 close baseline cross-check 仍待處理。

本輪只新增測試與本報告；checker 24 tests PASS，本輪檔案 diff --check PASS。
無產品、baseline、gate 修改；無 QEMU、kernel build、commit 或 push。
