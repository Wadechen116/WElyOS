# C3.2 持續審查狀態（2026-11-21）

本輪為證據與測試變動審查；沒有修改產品、共享 build 或執行 QEMU。owner 表示建議分工，產品仍由 Claude 持有，Codex 負責獨立驗證。

後續 root 更新：`regress-sr3.txt` 已完整結束並列「全部符合預期」，全文
SHA-256 `947ce1c27e0ee88e5980433d868cc6d35d4e3f322a3ec998c9fcea5da3b51e9e`。
Claude 176 已交付 `build/milestones/c32-b2-stackfix/`；root 核對15來源、4個
boot與6份log hash前綴符合，並確認三架構獨立QEMU通過的觀察副本與正式boot
逐byte相同。詳細見 REVIEW-STACKFIX-MILESTONE-20261121.md。下文早期sr3
尚未完成的敘述是歷史觀察點，不能當作目前狀態；sr2根因仍未定位，I-331保留。

新增未完成項：RAMFS零byte寫入回錯誤仍擴EOF已由真函式probe重現，最小
`done>0`修正為未套用提案。ustart KOBJ亦已有隔離patch與24項host檢查，
兩patch三架構proc/ramfs編譯均通過，但未套產品；ustart整合必須調整現有
stack故障測試，避免提前在新charge失敗而根本未測stack回退。fork_start/frame
仍待下一小批。參見 REVIEW-RAMFS-WRITE-CURRENT、REVIEW-TEMPOBJ-KOBJ-PROPOSAL、
REVIEW-PROPOSALS-BUILD-20261121.md。

## regress-sr2 與排程斷言

`regress-sr2.txt` 已完成且結果失敗，不能以後次單一 PASS 抹掉。其 SHA-256 為 `326ed3dad84ed1b910fb75a778a91efda9ae1cac1221755e811abccde0504b2b`：i386 selftest wrapper rc3、AArch64 selftest rc1；預期皆 0。

本輪看到 `kernel/test/sched_test.c` 的 git diff **只新增失敗時數值列印**，保留完全相同的嚴格條件：`releases == 10 && misses == 0 && overruns == 0 && period_ms == 20`。這不是放寬斷言，也不是排程器修復；能讓下一次重現區分哪個計數不符。但現有 sr2 失敗沒有這段診斷，無法追溯是哪個欄位造成。

當前 `kernel/sched.c` hash `7748c32b0ed821bde02985abe3cbb4eda57d4611b7f8e00baf3c3dfb74a0f3c8` 與歷史 filepipe manifest 一致；本輪未見新增排程產品修正。`sched_test.c` hash 為 `6dacb837ea6a327a19ca9eaeec2bf84f70b5afc6a0304cc75edc8f0b8bfa47d1`。

時間與共享 log 不可混用：

| 檔案 | 本輪所見時間（+08:00） | 內容／侷限 |
| --- | --- | --- |
| `build/c32a/regress-sr2.txt` | 12:00:09 | 完整 FAIL summary；未記 boot/log 全 hash |
| `kernel/test/sched_test.c` | 12:03:14 | 診斷列印的更新時間，晚於 sr2 |
| `build/regress/i386-selftest.txt` | 12:03:54 | 已為後次 guest exit0，RT 子測試 PASS；不能回填 sr2 的 rc3 |
| `build/regress/aarch64-selftest.txt` | 11:58:29 | 舊 `sched_test.c:872` 嚴格斷言 PANIC，無失敗數值；只能支持此 log 的失敗 |
| `build/c32a/regress-sr3.txt` | 初看12:05:44尚未完成；後續已完整 | 全文 hash `947ce1c27e0ee88e5980433d868cc6d35d4e3f322a3ec998c9fcea5da3b51e9e`，尾「全部符合預期」；成功不等於定位 sr2 失敗 |

本輪 i386 log hash 為 `e6cf4f8919bf14121519f367e25e15bba8a8e9b3dffc46e0f5f027edf2f9ea75`，AArch64 為 `3ba34e52084573c678f493fdb8d8232240c9b41fd4d4b17dd06df05cd2936285`；共享檔後續可能覆寫。

## 斷言實際測了什麼

`rt_task` 在一般工作吃滿 CPU 時，執行十次等待週期與一 tick 工作，檢查不早醒與週期統計。真實 deadline 計數可由 `rt_check_deadlines` 或 `thread_wait_period` 的過期迴圈增加；`rt_overruns` 則由實際 charge tick 超過 budget 增加。因此 miss 或 overrun 本身不足以證明產品錯誤，也不足以證明主機停頓；需對應這十個週期內的時間與 CPU 記帳證據。

後續 overrun 子測試已有獨立、量測得到的 timer burst／undercharge 條件才跳過部分斷言；這不是前十週期零 miss 條件的證據。新診斷只列最終統計與最晚 lateness，沒有逐週期 release/deadline/CPU charge 或限定同一區間的 timer burst，因此對根因定位仍不完整。

若 Claude 未來調整前十週期容忍度，應要求：不早醒、release 次數及 period 設定等無關條件保持嚴格；例外必須由同一受測區間的獨立時間證據界定，不能用「主機可能卡」或單看失敗數字自我豁免；並保留可確定抓到錯誤的對照（例如真缺少喚醒、錯 deadline 或 budget 邏輯）。本輪診斷補丁沒有降低既有驗收強度，亦未解釋 sr2 失敗。

## C3.2 待辦與完成條件

| 項目 | Owner | 狀態 | 完成條件 |
| --- | --- | --- | --- |
| sr2 失敗定位 | Claude 產品／測試；Codex 覆核 | OPEN | 保存失敗 boot、cmdline、完整 log、hash 與診斷數字；說明根因。完整重跑通過可作穩定性證據，但不能冒稱已定位舊失敗 |
| 174/175 固定驗收包 | Claude | 已交付，待 root 核對 | 正式 `build/milestones/c32-b2-stackfix/` 已交付；由 root 核對來源、immutable boot、manifest、logs 與 regress，保留失敗記錄 |
| 新固定包獨立驗證 | Codex root | 觀察副本三架構成功；正式 manifest 待核對 | root 已完成觀察副本的真 guest exit0／必要 markers；正式 manifest 與副本對應由 root 另核，不在本文件代宣稱完成 |
| stack 交易／並行回退 | Claude 修正；Codex stack agent | 審查中 | 每初始頁 PMM/map 失敗、prepare 失敗、重試、supplied stack、兩 creator 與 fault 深頁交錯均平衡；mem.mx 排除同址替換洩漏；來源 hash 固定 |
| fork caller 失敗原子性 | Claude 修正；Codex fork agent | 審查中 | 新 domain→prepare→space 順序於各失敗點父 PTE/tickets/refs/reservations 不變、子與暫存配置清空、未公布／未啟動 |
| file/pipe 精確 KOBJ 驗收 | Claude | OPEN | QEMU 以獨立實際 allocation-size 推導精確成本，測差一頁拒絕與 unwind；不能僅 `>=3` |
| thread 精確 KOBJ | Claude 已加；Codex 覆核 | 待固定包 | 保留 4+1+ceil(size/PAGE) 精確斷言與少算 guard 負對照，在新固定包獨立驗證 |
| 暫時物件 ustart/fork_start/frame | Claude | OPEN | 掛上 KOBJ/domain 生命周期；各配置／charge／prepare 失敗及成功交接、child main 釋放全部平衡 |
| ustart 隔離提案 | Claude 整合；Codex 覆核 | 待整合 | 將隔離提案落入產品介面與生命週期；證明失敗清理、成功交接及 KOBJ 歸屬均一致，提案本身不視為完成 |
| 全 fork 配置 fault injection | Claude 實作／Codex 獨立 | OPEN | 根表、各子表、frame、kernel stack、thread 結構等逐點失敗，不只第一子表 quota |
| (c) STACK／IMAGE／exec | Claude 設計與產品；Codex 驗證 | OPEN | stack 255 頁與頁表票、image 私有頁、exec transaction；耗盡後已承諾首次觸頁成功，失敗回 -E_NOMEM 且舊狀態不動 |
| I-330 stack 格位回收 | Claude，併 (c) | OPEN | thread 真不再使用後在 mem.mx 下解除全格、退頁與票、放回自身 slot；反覆建立超過15條及並行安全 |
| (d) RAMFS | Claude；Codex 驗證 | OPEN | FS domain 資料頁與 metadata 記帳、部分寫入失敗／最後引用釋放／quota 邊界及 memhog=file 平衡 |
| RAMFS 零 byte 錯誤增加 EOF | Claude 修正；Codex 獨立驗證 | 待修 | 失敗寫入且實際寫入 0 byte 時不得增加 EOF；測錯誤前後檔案長度、內容與配額不變，另保留部分成功寫入的正確 EOF 行為 |

只追蹤 C3.2 範圍；沒有將全專案 330 項 issue 都納入本輪完成條件。歷史 b2/filepipe 固定包仍有效，細節見 `REVIEW-PARALLEL-EVIDENCE-20261121.md`；成功子里程碑不等於 I-240 全部結案。

## 後續觀察快照驗證

root 已對 Windows Temp `codex-welyos-current-x_bgjsbp` 的固定觀察副本完成三架構普通 selftest；本 agent 獨立重新計算 manifest 全部複製檔 hash、三 independent log hash、實際 boot hash與七項必要 marker，全部符合，無 PANIC。詳見 `REVIEW-CURRENT-SNAPSHOT-QEMU-20261121.md`。

此快照的 `observed_sources_not_build_attestation` 明確只記錄當時來源，沒有宣稱這些來源產生這些 binaries，也沒有宣稱所複製 regress logs 來自這些 boot 副本。Claude 後續已交正式 `build/milestones/c32-b2-stackfix/`；正式 manifest 核對與觀察副本對應由 root 另做，本文件不代其宣稱完成。sr2 失敗與 I-331 未解狀態繼續保留，sr3 全文 PASS（hash `947ce1c27e0ee88e5980433d868cc6d35d4e3f322a3ec998c9fcea5da3b51e9e`）及本次三架構成功不等於定位 sr2 根因。
