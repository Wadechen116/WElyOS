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

## 高效能、高穩定度、高彈性的工程目標

使用者的新方向是面向航太工程用途的系統能力。以下是 WElyOS 自身可驗證的工程追蹤，不引用或推測 SpaceX 內部規格，也不代表符合任何正式航太認證。目標數值須由實際任務、硬體與工作負載定義，尚未量測或決定的欄位維持「待定」；不以 QEMU 的觀察最大值當實機最壞時間保證。

**順序維持不變：先完成 C3.2 正確性與失敗清理，再以固定基準擴充效能及資源彈性。** 可以提前設計與加入量測，但不得用縮小 stack、取消預留、放寬錯誤斷言或略過失敗路徑來換取表面吞吐量。

| 追蹤 | 要量測／定義的工程指標 | 第一交付與完成條件 | 依賴與 owner |
| --- | --- | --- | --- |
| E1 latency／jitter | 每種週期的實際喚醒延遲、release jitter、deadline miss／budget overrun；syscall、context switch、IPC延遲；irq-off／持鎖區間時間。記錄分布、樣本數、觀察最大值、測試時長與負載，門檻待任務定義 | 固定 workload、CPU數、時鐘來源與儀器；idle、CPU飽和、I/O、記憶體壓力各有基準及退化比較。I-331先定位，miss不能只被刪除或當host雜訊。硬即時保證另需最壞路徑分析，不以百分位取代 | C3.2穩定後比較；Claude排程／timer owner，Codex量測與證據覆核 |
| E2 每thread實體／承諾成本 | user stack初始實體頁／承諾頁／頁表票、kernel stack與guard、thread/ustart物件KOBJ、domain refs；cache／pool保留實體另列。區分每thread增量與共享頁 | c1先量255頁承諾、最上4頁兌現及metadata成本；列建立→ready→fault成長→退出→延遲回收各點帳本與PMM差值。單位含pages/bytes，實體不重複加總；20次以上反覆建立後回到基準。若新增小stack API，明確指定大小與拒絕行為，不默默降低預付承諾 | M0/M1；Claude記憶體owner，Codex精確成本與生命週期probe |
| E3 可配置資源限制 | thread/process數、stack大小、domain配額、檔案／管線／socket／待送佇列容量與限速；編譯期與執行期配置分開，硬上限及預設成本可見 | 先建capacity表與合法範圍；每參數有最小／最大／溢位／互相衝突／低於已用量案例。變更失敗不破壞現有資源；縮限不撤銷已保證承諾。容量擴張須驗時間複雜度與總記憶體成本，不能只增常數 | C3.2 quota語意先穩定；各子系統owner，Codex邊界矩陣 |
| E4 失敗原子性 | 每個配置、記帳、票取得、fd／pid公布、VM commit邊界的注入覆蓋；失敗前後頁／票／配額／引用／可見狀態差 | 保存每入口的成功完成點與最後可失敗步驟；逐失敗點快照比對。fork失敗父完全不變、exec失敗舊程式可繼續、thread失敗不留slot/mapping、零byte RAMFS失敗不增EOF；至少一個移除回退的負對照須被抓到 | M0~M4必備，先於效能優化；Claude修正，Codex獨立真函式probe |
| E5 故障隔離／可恢復性 | 故障影響範圍、偵測延遲、受影響任務／domain、資源回收量、恢復時間與恢復後資料完整性；不可恢復故障另列 | 先定義可恢復／需重啟／必須停機的故障模型與責任邊界。對quota耗盡、任務崩潰、I/O失敗、watchdog／supervise無進度逐項驗證；恢復後重新服務且無跨domain洩漏。panic與重開機不是自動等於完成恢復；不得在共享狀態未知時繼續執行 | C3.2隔離是基礎；後接I-258/I-264等可靠性工作，獨立排產品範圍 |
| E6 重現性與變更追溯 | commit／未提交diff、工具鏈／建置選項、來源與實際boot hashes、cmdline、seed／故障點、host／guest平台、完整log、測試版本與結果 | 每里程碑immutable包含真boot artifact（AA64含.bin）、完整manifest與成功／失敗證據；獨立驗證只用固定副本。來源觀察hash與build attestation分開；重現程序能由第二位執行者使用，未知與未重現保持open | 全階段；Claude交包，Codex root核對與整合 |
| E7 硬體驗證 | 明列支援板卡／CPU／記憶體／裝置／韌體版本；實機時鐘、SMP/TLB/cache、interrupt、MMIO、裝置失敗與壓力行為，平台限制可追蹤 | 先選一個具體實機與可回復測試環境，再建立最小boot→timer→SMP→memory→I/O bring-up清單及可量測觀測點。QEMU與實機結果分欄；MMU順序、memory map／reserved-memory與驅動假設逐項消除。未做實機就明示未驗證，不把模擬器倍率或通過率外推 | M1~M4介面穩定後分階段導入；需具體硬體與測試條件，owner待平台選定 |

每個功能里程碑與效能改動都應附 E1~E7 的適用項、量測值與未驗部分。正式航太認證的適用標準、危害分析、保證等級、獨立性與流程需另立明確計畫；本佇列沒有聲稱已取得認證、飛行許可或任務安全保證。

## 無人機「Drong Cam」需求軌：飛控與圖控操縱

使用者已澄清「Drong Cam」泛指無人機相機／圖傳，沒有特定品牌或產品。本需求軌採品牌無關、可替換硬體與通訊介面的方向，不預設廠商協定。還需確定 WElyOS 的部署角色：直接承擔飛控、機載相機／運算伴隨電腦、地面控制端，或分別部署；角色會決定安全邊界、硬體與deadline，不能把三者視為同一套已存在的功能。

### Repo 盤點與現有邊界

本輪只讀盤點 `kernel/`、`include/`、`arch/`、`user/` 與設計文件，未搜尋外部、未測試、未改產品：

- 已見週期排程、timer與高精度計數介面（`kernel/sched.c`、`include/timer.h`），但無感測擷取timestamp或致動輸出鏈的硬體deadline驗收。
- `kernel/watchdog.c` 是CPU間timer heartbeat偵測，單CPU不生效、偵測後panic；不等於獨立硬體watchdog，也不是致動器command timeout／飛控失效處置。
- `include/net.h`／`kernel/net/stack.c` 提供固定buffer的Ethernet、ARP、IPv4、ICMP、UDP；`NET_UDP_MAX=1472`且不做IP分段。這是一般通訊基礎，不代表已有圖傳、遙測或飛控控制協定，也不代表無線鏈路或控制訊息已認證。
- 現有 `include/console.h`、`arch/i386/vga.c` 是文字主控台／VGA相關能力；不等於camera preview、地圖、姿態儀表或飛控圖形操縱介面。主控台前景群組權限也不能代替無人機控制權管理。
- 檔名與特定完整詞彙盤點未找到可識別的camera、IMU、PWM、codec或無人機協定專用實作。這是目前盤點範圍內的結果，不是所有間接硬體能力均不存在的證明；`flight recorder`一詞是blackbox事件紀錄註解，不是飛控支援。

### 可追蹤需求與完成條件

| 追蹤 | 需求／待定參數 | 第一交付與完成條件 | 依賴與分工 |
| --- | --- | --- | --- |
| D0 平台與角色 | 板卡／SoC、CPU核心、RAM、獨立飛控器、相機與介面、感測器匯流排、致動介面、編碼器、鏈路、地面輸入裝置；採品牌無關及可替換介面，目標硬體與性能數字待定 | 建立實際硬體清單、責任分割與資料流圖；逐介面標明已有／需port／需新driver／需外部元件，以adapter／能力描述隔離設備差異。每個protocol由實際設備規格決定，不能先宣稱相容。數字目標與測試平台確認前只列待定 | 跨平台需求owner待定；可先文件盤點，驅動整合在平台選定後 |
| D1 飛控即時鏈 | 逐迴路sensor sample→估測→控制→actuator的週期、deadline、jitter、容許sample age／掉樣、時鐘精度與跨sensor同步誤差，數字均待任務定義 | timestamp在擷取／硬體事件邊界產生，標記時鐘domain、序號、有效性；明確處理過期、重複、亂序與時鐘跳變。量測端到端延遲與每段預算，CPU／影像／I/O飽和下仍符合已定門檻；不得只驗thread準時醒來 | E1/E7、I-331與C3.2正確性；飛控／sensor driver owner待硬體決定 |
| D2 致動watchdog／失效模式 | command freshness、致動更新期限、感測失效、deadline miss、任務崩潰、CPU卡死、供電／鏈路失效、重啟後行為與arming狀態；timeout數字待定 | 為每種故障明定可檢測條件與系統／獨立硬體的責任，設計命令序號／期限與可觀測ack。對不同飛行狀態採經需求確認的受控處置；不自行把停止馬達、保持輸出或返航當通用安全答案。台架／HIL證明過期命令不持續生效、恢復不自動重放或擅自解鎖；CPU timer heartbeat不能替代此驗收 | D0/D1、E5/E7；需具體致動器／飛控器與安全狀態規格；不得靠panic單獨完成 |
| D3 相機擷取／編碼／圖傳預算 | 解析度、fps、pixel格式、codec／profile、碼率／burst、擷取到顯示延遲、畫面age、容許掉幀；DMA／buffer數、CPU／加速器、RAM／承諾頁、記憶體與網路頻寬、功耗／熱限制均待硬體及目標定義 | 先算buffer與搬移成本（每frame實際stride×height，乘佇列深度與副本數），再定有界queue、背壓／掉幀政策。擷取／編碼／傳輸各stage記timestamp與frame ID；encoder reset、consumer慢、鏈路擁塞、重啟皆不漏buffer／DMA ownership。過載時降級圖像服務而不吞噬飛控已保留的CPU／memory／I/O預算 | D0、C3.2配額／票／失敗清理；沒有現成camera/codec driver承諾。是否zero-copy需硬體cache/DMA一致性證據，不能只靠指標傳遞 |
| D4 地面UI／遙測／圖控操縱 | 姿態／位置／電量／狀態的來源、更新率與age門檻；相機preview、雲台／鏡頭、joystick／鍵盤／觸控、任務／飛行模式；各數字及裝置待定 | 首先定義遙測與命令schema、單位、座標系、時間與版本；畫面呈現stale／未知狀態，不能把最後一幀當即時狀態。操作採明確命令／回覆／超時狀態，不將按鈕按下當致動成功；UI阻塞／crash不影響機載控制迴路。圖像、遙測與操作事件可用共同時間軸回放 | D0/D1/D3；先決定地面UI是否在WElyOS或外部程式，未選GUI框架／驅動或飛控protocol |
| D5 控制權／鏈路中斷 | 操作者身份與授權、機載／地面／手動控制仲裁、命令去重與新鮮度、replay、亂序、延遲、重連、認證／完整性；失聯與恢復期限均待定 | 明確單一有效控制權或可證明的仲裁狀態機，控制權轉移可追溯；未授權／過期／重放命令被拒。測link loss／抖動／burst loss／半開／重連時，舊命令不回灌、恢復需確認當前模式與權限。控制／遙測／video有可測的佇列與頻寬隔離，video flooding不阻塞控制；失聯處置依D2任務狀態定義 | D0/D2/D4及E3/E5；UDP checksum／ARP檢查不等於身份認證或重放保護；未宣稱已有無線driver／security protocol |
| D6 可重現整合驗證 | sensor錄製／回放、虛擬致動模型、相機測試源、網路故障注入、HIL／實機；任務scenario、數值門檻與時長待定 | 先離線replay與有界資源／失敗注入，再上固定硬體台架／HIL驗driver與同步；保存firmware/config／輸入／輸出／clock與完整版本hash。控制誤差、deadline、影片延遲、掉樣、恢復時間及資源峰值分別報告。模擬成功不改寫成飛行驗收或認證；真實飛行驗證需另有具體程序與條件 | D0~D5、E6/E7；先C3.2正確性，平台及需求明確後分段整合 |

這條需求軌補充 M0~M4 與 E1~E7，不打斷先完成記憶體承諾、失敗原子性與生命週期的工作。可並行完成需求與台架設計；產品功能、驅動或協定支援只在實作與相應證據完成後標為已提供。
