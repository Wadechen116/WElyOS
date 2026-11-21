# OS 工作佇列（2026-11-21）

依 `docs/ISSUES.md` 尚未完成項與 `docs/design/C3.2-memory-accounting.md` 定案分段整理。本輪只讀盤點、只新增本文件，沒有執行測試或修改產品。這是可分派的工作順序；不把提案、設計理由或偶然未重現當結案證據。

先完成現行 C3.2 主線；I-331 保持獨立調查軌。其後列五個避開 Claude 記憶體核心檔案的較小工作。owner 是建議分工：產品修改仍由領取該範圍的 owner 執行，Codex 可先做獨立 probe／審查，交界前透過 memory 宣告檔案範圍。

## 主線與獨立調查

| 順序 | 工作／來源 | 建議 owner／檔案範圍 | 依賴 | 完成條件 |
| --- | --- | --- | --- | --- |
| M0 | b2 收尾：暫時物件與既有驗收缺口；C3.2 設計 B6／B8，`CONTINUOUS-REVIEW-STATUS-20261121.md` | Claude：proc/sched/vfs/pipe；Codex：自有 host probe | 新 stackfix 固定包由 root 核對 | ustart 隔離提案整合並驗 KOBJ 交接；fork_start/frame 記帳與失敗退回；pipe 精確成本與差一頁拒絕；逐配置點失敗不公布子行程。既有 b2 子里程碑不因此重做，但未驗項不得省略 |
| M1 | **C3.2 (c1) STACK**；I-240、I-330；設計末節「第 (c) 批定案與實作分段」 | Claude：vm/proc_mem/proc/thread 生命周期；Codex：故障／交錯 probe | M0 的生命周期介面穩定；正式 stackfix evidence 核對 | 三承諾窗、分類 commit/fault/release、claimed/ready；每格255頁加頁表票，先兌現最上4頁；fault 只認 ready 且用票。每頁失敗／prepare失敗／深頁fault交錯完整退回。thread 真不再執行後回收自身格位與所有頁／票；連續建立結束超過15條、主執行緒先結束後非0格fork、caller提供stack、借用者先結束、重複結束皆驗證。fork按承諾stack範圍帶票，不能假設第0格 |
| M2 | **C3.2 (c2) IMAGE**；I-240；設計同節 | Claude：vm/image_build/proc_mem；Codex：image/fork failure probe | M1 分類帳本與承諾範圍介面穩定 | 實作 vm_space_map_charged；私有可寫映像與無法共用時的唯讀私有頁都記 IMAGE，一般配置在公布前完成；可寫映像範圍交給fork。每次配置/記帳失敗退回映像與帳本、父狀態不變，共用檔案頁不重複收費 |
| M3 | **C3.2 (c3) exec 交易**；I-240；設計 R4 與末節五項決定 | Claude：membudget/vm/exec；Codex：交易／失敗矩陣 | M1+M2，舊新空間帳本可完整列出 | caller stack上的不配置txn綁新空間；begin/check_final/end及create_txn；pending只含舊空間帳本，長壽命KOBJ與image_build新開file不可借額度。L100/舊80/新80成功、新101拒絕；同時降低limit、每失敗點與成功後清理都驗證；失敗保留舊程式／domain／fd，成功用量回到有效上限 |
| M4 | **C3.2 (d) RAMFS**；I-240；設計 FS domain／RAMFS 類別 | Claude：ramfs與必要FS-domain初始化；Codex：真函式配置失敗probe | M0物件charge契約穩定；依主線排M3後整合 | FS domain涵蓋資料頁、區塊表、inode、目錄項；建立／擴充／unlink最後引用／失敗半途配額平衡；quota邊界、部分寫入與memhog=file證據。**先處理已發現的零byte錯誤增加EOF**：實際寫0且錯誤時長度／內容不能變，部分成功寫入的EOF仍正確。不以一般PMM底線取代RAMFS記帳 |
| R1 | **I-331 偶發RT selftest失敗**；`docs/ISSUES.md:I-331`、sr2/sr3 | Claude：診斷與產品；Codex：證據核對；共享sched檔不與M1同時改 | 可並行只讀／自有host probe；動sched須與M1排程 | 保存失敗實際boot、cmdline、log/hash與releases/misses/overruns/late；以同受測區間時間與CPU記帳證據定位原因，建立能抓該原因的測試／負對照。sr2失敗保留；sr3全文PASS與後次成功不視為定位。ISSUES所述同版／修改前各6次共24次未重現只轉述為Claude證據，不冒稱Codex實測 |

每個主線分段完成後都交付 immutable 三ELF加AArch64實際boot `.bin`、來源／artifact／完整log hash與回歸摘要。獨立驗證使用固定副本。`c32-b2-stackfix` 已交付；正式manifest核對由root負責，本輪不重複宣告或自行重跑QEMU。

## 五個可另領的小項

下列都仍在 ISSUES 登記待處理。本輪只判定為可控制範圍的候選，沒有宣稱已重現或已修。順序先工具隔離，再單一使用者程式，再shell與網路；各自不要擴張到整個子系統重寫。

| 順序／issue | 具體第一交付與來源 | 預計檔案範圍 | 依賴／與記憶體主線的界線 | 完成條件 |
| --- | --- | --- | --- | --- |
| S1 **I-191 映像檔並行使用保護**（第27步） | 為qemu launcher對磁碟映像持有全執行期互斥claim；來源 `scripts/qemu.sh` 的QEMU_DISK／DISK_ARGS | `scripts/qemu.sh`、自有launcher測試；Windows launcher如有則同契約 | 不改kernel；先盤點直接Windows啟動與其他launcher，對canonical path／別名與none模式訂明範圍；不可因鎖過舊逕自刪除 | 同一映像第二個launcher在QEMU啟動前明確拒絕；不同映像可並行；disk=none不誤鎖；正常退出、錯誤、signal都釋放；鎖建立原子且整個子程序生命期不失守。先用受控假QEMU驗證互斥，再在隔離磁碟做整合，不碰日用共享映像 |
| S2 **I-194 top更新間隔可調**（第28步） | 新增有範圍檢查的delay參數／互動設定；來源 `user/top.c` 主迴圈固定 `sys_sleep(1000)`，batch已有`-b` | `user/top.c`、自有參數／採樣fixture | 不改kernel timer/sched/syscall；避免將變數 `interval`（實測tick差）當設定值 | 合法間隔影響等待，0／負數／溢位／缺值／衝突參數明確拒絕；原預設保留；batch與互動退出都清理終端。CPU%、切換率仍以實際tick差計算，不以要求delay代替量測 |
| S3 **I-122 script遇錯可停止**（第21步） | 提供明確fail-fast script模式，先定義builtin、外部命令、pipeline、背景job與語法錯誤的退出政策；來源 `user/sh.c` script/main/status分支 | `user/sh.c`、自有shell host fixture | 不改proc/spawn/fd核心；接續現有shell矩陣與輸出確認契約。此為shell產品scope，與其他shell owner分開領取 | 啟用模式下前景命令／builtin／解析失敗回非0並不執行後續行；保留最後pipeline status的明確語意。背景失敗不能被聲稱已同步取得；wait/exit仍正確確認輸出並保存fsync錯誤。未啟用與互動操作不被意外改變；對應故障矩陣與負對照通過 |
| S4 **I-251 同下一站ARP待送佇列**（第36步） | 將同hop只能一包改為小型有界FIFO；來源 `kernel/net/stack.c:route/pending_find/learn/net_tick` 現行重複hop回E_BUSY | `kernel/net/stack.c`、net內部header與自有host網路fixture | 用固定有界storage，不接membudget／heap；容量、每hop公平、逾時政策先明定。與S5可能共用網路檔，按序整合 | 同hop兩包可依序送達；多hop不互相誤放；容量滿明確E_BUSY且無覆寫；ARP重試／逾時／重複回覆／偽造回覆處理不退化；重置不留舊包。不能僅把SLOTS調大就當同hop佇列完成 |
| S5 **I-254 ping發送速率限制**（第36步） | 在實際kernel送出邊界加入明確上限與burst的可測限速；來源 `kernel/netif.c:netif_ping`、`kernel/net/stack.c:net_ping_send`，ISSUES已指出MAX_PINGS只限pending | `kernel/netif.c`或明確選定net發送層、相關自有host fixture | 只在網路範圍；不改proc cred／記憶體核心。先選全域／每來源政策，不能自行宣稱現有四pending就等同限速；排在S4穩定後 | 在可控制時鐘下burst邊界、持續速率、回填、時間wrap、同時呼叫、send失敗與timeout清理都驗證；被拒絕不耗盡pinger slot。明確回傳錯誤且正常低頻ping可用。是否需特權是另政策，不拿特權檢查替代限速 |

S2與S1可在產品檔owner確認後各自獨立進行；S3不碰記憶體核心但仍需保護既有shell I/O故障驗收。S4、S5使用網路固定buffer不代表免費：須在變更說明列新增靜態記憶體成本；真正是否需要分類記帳另與C3.2 owner協調。

## 結案與相依性規則

每項以「能重現的測試 → 產品修正 → 原測試通過 → 還原／突變必失敗」為基本證據。對新能力列明拒絕、邊界與生命週期案例。編譯、host fixture、QEMU與實機證據分開記錄；未跑者直列未驗證。不因本文件把某項排進佇列就改ISSUES狀態。

I-240是複合項，須M0~M4所屬記帳／預留能力與耗盡驗收實際完成才評估整項結案；I-330隨M1單獨驗收。I-331與I-283等既有取樣限制也不互相替代。全專案其餘未完成項本輪不展開，五項候選完成後再依風險與owner範圍重排。
