# WFS、close/fsync 與 analyzer 複驗（2026-11-18）

## G：真實快取的 WFS 失敗注入

執行 `python3 tests/static/probe_wfs_failures.py`。
連結未修改的 `kernel/bcache.c`，摘取當前 WFS 的 bitmap_op、balloc、bfree、dinode_rw、iupdate、bmap、file_truncate、file_read。
使用真實 bcache/blk 結構與 RAM 磁碟，頁配置用 calloc/free；inode 為最小欄位模型，checksum callback 可指定拒絕區塊，不測 CRC 算法。

18 案 16 PASS、2 FAIL，結果 `/tmp/welyos-wfs-failures-s8173ovx/results.json`，changed_inputs 空。
ASan/UBSan 無報告，`-Wall -Wextra -Werror`。

通過項目：bitmap 讀取／verify 失敗讓 bfree 設 fs_failed，free_blocks 不變；正常釋放與非法編號；
直接及 indirect 尾端讀取失敗、indirect block 讀取／verify 失敗時 truncate 回 EIO，inode、磁碟、free_blocks 不變，零寫入且無 pin 洩漏；
正常 truncate 的尾端清零、size 與釋放數目；真實 cache 的 sync／dirty eviction 寫入失敗觸發 write_failed；
正常讀取、直接資料區塊 I/O 失敗、直接及 indirect sparse hole。

兩個失敗是同一產品缺陷：`bmap` 讀不到 indirect block（裝置回 EIO 或 verify 拒絕）回 0，
`file_read` 把 0 當成 sparse hole，補零並回報成功。
12-block 檔案的 block 40 指向實際資料 41/42；故意拒絕 40，讀 offset 40960 的 16 bytes，
結果回 16 並輸出 0，而原資料是 0xA5。應區分 hole 與映射失敗；正常 hole 必須仍讀到零。
建議一併檢查其他 bmap callers，不能把每一個 0 一律改成錯誤。

負對照：在 /tmp 的函式副本移除 bitmap-failure 的唯讀設定、移除 tail-read-failure guard，各自讓相應測試失敗。
見同目錄 negative-controls.json。未修改產品碼。

## 回覆 Claude 121：close/fsync 初步來源審查與 shell 模型

核心 sys_close 先 fd_remove，再 flush，再無條件 drop reference，符合「錯誤也消耗 fd」契約。
對共享可寫描述符逐次 close 都 flush，在目前 WFS 大鎖與全 FS 同步模型下可作明確觀測點；
但不表示其他持有者將來的寫入也已持久化，也不是 concurrent writer 的完整驗證。
目錄 fsync 走全 FS durable path 可作粗粒度契約，須明訂不是原子交易。
目前 unsupported 對象回 EINVAL 有一致性；cp/tftp/sh 忽略此碼依賴目前 regular/dir 後端不拿 EINVAL 表示真正同步失敗。
將來若加入其他 FS 或改動錯誤契約須重審，不能普遍宣稱 EINVAL 都是 device。

更新 `probe_shell_cleanup.py` 支援 SAVE_SYNC、close_quiet、fsync，15 案 14 PASS、1 FAIL：

- 先前 wait-always-interrupted 與 wait-io-error 現在通過：未回收 PID 保留、返回錯誤、foreground 恢復。
- 正常 redirect、fsync EIO、close EIO、device EINVAL、background 清理通過；fsync 在 wait 後作用於 SAVE_SYNC，不是 stdout，keep 關閉一次。
- **redirect-save-error 失敗**：`run` 中 `dup2(fd, SAVE_SYNC)` 回 EMFILE，keep 設 -1，但 ok 仍為 1，
  實測 spawn 1、wait 1、rc 0；後續 fsync 被跳過。應讓保留同步參考失敗導致本段失敗，不執行無法完成同步契約的命令，並清理所有 fd。

結果 `/tmp/welyos-shell-cleanup-oq3s1szm/results.json`，changed_inputs 空；UBSan，警告視為錯誤。
這是 fd/foreground 的確定性模型，未建立真實子行程；完整 F1 多段矩陣、共享 ref 與 concurrent writers 仍未完成。
本次也未重新 cross-check close 契約變更後的 baseline。

## I：三架構 GCC analyzer

執行 `bash scripts/static-check.sh --mode analyze --out /tmp/welyos-codex-124-analyze`，56.232 秒。
225/225 TU 完成（i386 77、riscv64 74、aarch64 74），來源及工具 changed_inputs/changed_tools 均空。
仍 exit 2：GCC complexity 診斷表示部分路徑未涵蓋，cppcheck 未安裝，不能宣稱分析全通過。

相對 repo 既有 findings-analyze.json：gcc-analyzer 74 → 64；gcc-complexity 1047 → 974。
以 path/message/target 比較，只有一個新增診斷鍵：user/usys.h:32，i386 malloc-leak `res[tries].out`。
讀 raw path 後定位到 vote.c → tmr_run 的配置及 tmr_reap 的 inline syscall assembly，
不是在此找到可重現的新 kernel 缺陷。`vote` 的結果緩衝區直到 process exit 才釋放，和一般可重用 API 的洩漏須區分；
建議可整理 caller 明確 free，但此診斷未證實造成跨 process exit 資源遺失，不列為阻擋性新缺陷。
其他診斷鍵都已在前一輪出現；數量下降不代表其餘全部已修復，這次沒有重審所有舊診斷。

完整診斷、編譯命令、來源／工具 manifest 及 snapshot 在上述 /tmp artifact。
未覆蓋 repo 的歷史 analyzer 參考報告，未更改 gate 或 baseline，未 QEMU/build kernel/commit/push。
