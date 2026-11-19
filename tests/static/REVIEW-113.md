# Codex 完成 109 剩餘審查（2026-11-18）

範圍：B1 排程器、B3 黑盒子、B4 wfs_probe、D 使用者程式提示分類，並複驗 Claude 112 的 TMR／SEU 修正。
起始 HEAD fbc3404；產品修正由 Claude 同步完成。Codex 僅修改 tests/static/、tests/host/，
未執行 QEMU、未寫 build/、未提交／推送。所有主機重現皆只使用 RAM／暫存檔，不存取真實磁碟。

## B4：未知磁碟的黑盒子寫入與最小容量

**原始失敗路徑**：disk_init 呼叫 blackbox_init(dev, wfs_probe(dev))。
wfs_probe 在未知內容、讀取失敗、配置失敗或容量不足時回傳 0，明訂呼叫者不可以碰磁碟；
blackbox_init 卻把 0 當成很小的合法檔案系統，啟用後會寫磁碟最後 8KB。
RAM 磁碟填滿 0xA5 模擬外來資料，實測 probe=0、enabled=1、blackbox_log 確實寫入一個磁區。
原始結果 `/tmp/welyos-boot-boundaries-z3mso_2c/results.json`。

**修正與複驗**：Claude 在 kernel/blackbox.c 的 blackbox_init 加入 fs_blocks==0 拒絕條件；
獨立測試未知內容、primary/backup 讀失敗（回報失敗前先寫部分 buffer）、pmm 失敗，均停用且零寫入。

另一問題：64／65 個 4KB 區塊的空白磁碟會被預測格式化為62／63個檔案系統區塊，
但 sb_valid 要求至少64，導致自己格式化的檔案系統下次不認得。
Claude 將 probe／mount 最小容量統一為66，留下64個FS區塊及2個黑盒子區塊。
63、64、65 均拒絕；66 正常；合法primary、合法backup與占滿磁碟的舊格式（黑盒子停用）均通過。
此最小容量政策也會拒絕64／65區塊的歷史小型舊格式磁碟；若要保留它們，需另設只掛載舊格式的例外。

未發現讀取失敗後使用未初始化 buffer、probe 配置未釋放或容量邊界越界。
primary讀失敗時不嘗試backup是目前的保守停用策略；改善可用性可另行設計，不能因此容許寫未知磁碟。

## B3：黑盒子記錄與序號繞回

- failed/test/healthy 記錄組合正確；包含 `(test)` 的 boot 不算失敗。
  掃描上界固定64筆，每筆 marker 掃描上界為已驗證的 len<=100，沒有未受限迴圈。
- 70筆 boot 進入64格環狀記錄後，只能看見最新64筆，符合儲存容量限制。
  若單次失敗開機產生超過64筆其他事件，boot marker 被覆蓋後便無法重建完整失敗開機次數；
  這是目前可觀測性限制，不聲稱固定64格可保留無限歷史。一般使用者不能寫任意 BB_BOOT/HEALTHY。
- 隨機損壞使 boot 號極大且 CRC 不符時，load 會移除該筆，不會採用其 boot 號。
- **已重現的序號排序錯誤**：合法CRC、seq依時序為 UINT32_MAX-1、UINT32_MAX、0 時，
  舊 load 以普通 >= 選成 UINT32_MAX，重啟後可能反覆覆寫錯誤位置。
  Claude 改採有 have 旗標的 modular 比較，seq-wrap 案例現已正確選0。
- boot號 UINT32_MAX 在舊版會繞成0；原先屬未定義的政策問題。
  Claude 明確選擇飽和到 UINT32_MAX，現在 boot-wrap 案例也固定驗證這個政策。

`python3 tests/static/probe_boot_boundaries.py`：16/16通過，ASan/UBSan無報告，-Wall -Wextra -Werror。
最新結果 `/tmp/welyos-boot-boundaries-2orjw8cd/results.json`。
原始 blackbox.c、CRC實作與WFS函式摘取的來源hash均保存在artifact；只替換裝置、配置器、鎖與時鐘等環境。

## B1：RT admission、配額記帳與期限反例

**已重現的原始問題**：所有即時工作同為 PRIO_RT=8，使用同級10ms時間片，
總使用率不超過70%並不足以保證不同週期的工作及時被執行。
long(period1000ms,budget100ms)與short(period2ms,budget1ms)都被接受、util=600。
short完成第一輪後睡到t=2，long此時尚有時間片；t=2 short醒來卻不搶佔同級long，
t=4 short已miss一次且CPU tick仍為0。此情境不需要工作超用其宣告預算。
原始結果 `/tmp/welyos-periodic-e4amzpla/results.json`。

**Claude修正**：最多8個RT工作依不同週期分配8..15優先權，較短週期較高。
rt_assign_levels 為最多8×8×8的有限迴圈；重宣告／取消均重新分層。
獨立摘取實際 update_prio、rt_assign_levels、rt_* helpers、thread_set_periodic、sched_tick，
主機單CPU環境尊重 need_resched，短工作消耗一個tick便完成並阻塞到下個release。
現在相同反例 long prio8、short prio9，short CPU tick1、miss0。

另外通過64種輸入邊界及20000次重宣告／取消的表格、利用率總和與排序一致性檢查。
在目前 timer_init(1000) 下，60000ms 上界讓 ms×hz 與 budget×1000 的32位元算術安全。
移除採 rt_period==0 快速返回，正常鎖與登記表不變量成立時，不會重複扣除利用率。
這些結論不適用於未受限的新timer_hz或任意記憶體毀損。

thread_wait_period 的 catch-up while 沒有語法上的固定上界；但目前CPU0每次只把timer_ticks加1，
隨即 sched_tick 逐tick推進 deadlines（架構catchup也是重複呼叫timer_tick），
一般停滯工作不會讓 release 落後數分鐘。因此未把人為跳躍now的模型當作已可達缺陷。
若將來引入一次跳過大量tick的時鐘介面，必須重新檢查這條鎖內迴圈。

`python3 tests/static/probe_periodic.py` 通過，UBSan無報告，-Wall -Wextra -Werror；
最新 `/tmp/welyos-periodic-oavztlfx/results.json`。
主機環境不包含真實context switch、跨CPU排程、IPC阻塞與中斷延遲；
此結果只關閉具體反例，不是所有實際工作集的WCET或無條件期限保證。
budget throttle 的量測仍只有tick粒度，>budget 的判斷也允許到下一個tick才降級，
不可把宣告利用率當作硬體上的精確CPU使用上限。

## D：122筆使用者程式提示已逐筆分類

交付 [UNCHECKED-ERROR-TRIAGE.md](UNCHECKED-ERROR-TRIAGE.md) 與 unchecked-error-triage.json：
31 real建議處理（產品19、測試健全性12）、91 benign限當前前提的建議。
每筆包含精確ID、函式hash、來源行、理由、影響類別；未加任何baseline。
real是人工錯誤路徑判讀，不代表全部都已故障注入重現。

主要建議：輸出helper／printf吞負值與短寫、shell dup2/SIGCONT失敗繼續執行、
top無效sysinfo樣本／畫面輸出，以及測試裡fork負碼被當負PID傳給kill、必要訊號或stdout復原失敗造成等待。
exec成功不返回、回來立刻報錯的su等呼叫則屬已處理錯誤。

重要界線：sys_close目前只有EBADF可回報；vfs_close與wfs_closed是void，flush錯誤無法傳回使用者。
檢查close不等於檢查持久化，因此沒有把所有有效fd清理誤判成「能收到磁碟錯誤卻沒看」。
若要提供cp/tftp可靠的完成訊號，需另處理sync或核心錯誤傳遞。
lexical提示也不是所有unchecked calls的完整清單：例如cp的chmod、TFTP send仍有人工可見但本次未報的忽略回傳。

## 112修正複驗及探針維護

- 舊107 TMR探針改成相容入口，委派新版完整矩陣，避免維護缺少E_INTR/WNOHANG的重複stub。
  `python3 tests/static/probe_tmr_failures.py` 26/26通過，包含先前read錯誤覆蓋與一次wait中斷。
  `/tmp/welyos-tmr-matrix-tn9o6kod/results.json`。
- SEU探針不再綁舊exchange條件；同時摘取reader與SYS_seu writer。
  queued A時B回-E_BUSY且不覆蓋，reader快照解鎖後再發布B成功，兩次消費最終[A,B]=[1,2]、pending0。
  `python3 tests/static/probe_seu_race.py` 通過，`/tmp/welyos-seu-race-9bsl6xqr/results.json`。
  只驗證確定交錯，不取代真實多執行緒／QEMU壓力測試。
- 對fbc3404的暫存來源執行新boot及periodic探針，仍以exit1重現原始問題，沒有編譯錯誤，
  避免測試僅適用修正後而失去回歸價值。負對照快照：`/tmp/welyos-113-before-49ilc87v/negative-controls.json`。
- allocator 1024次realloc／sbrk失敗及formatter三模式均通過；工具單元測試21/21通過。
- fast比較當時4個新提示皆style，895既有、總899未審，無新hard rule，changed_inputs/tools皆空。
  `/tmp/welyos-codex-113-fast`。Claude負責產品整合與fast reference更新；本輪未重跑GCC analyzer。

本輪B1/B3/B4/D的審查與測試交付完成；31筆real仍由Claude進行產品／測試修正後再複驗。
