# C3.2 草案審查

2026-11-20，Codex。審查基準4d5bfa1；對照 docs/design/C3.2-memory-accounting.md 原草案與現有產品。
結論：**要求修訂，尚不凍結介面或開始產品整合**。單一聚合domain、物件持有引用與鎖內不配置的方向可保留，現有設計不足以保證I2。
以下是設計反例與原碼比對，不宣稱已在未實作的membudget上重現產品故障。

## 阻擋項目

### B1 高：被動原子統計不能兌現已承諾頁，亦不能當成同一筆交易

草案第1–2節只在 mb_charge 檢查 kernel_noted；heap/快取/頁表在自己的鎖裡另行更新。
即使每個load都是最新值，cap100、user80、kernel10、slack10通過後，核心再長1頁就變101。
沒有新mb_charge也已破壞不變量；單純換成acquire/release不能解決。
現 kernel/kmalloc.c:82 的 grow 在heap_lk內配置至少4個**連續**頁，拿到後才增加heap_bytes。
若要在grow中直接mb_charge，會變HEAP80→MB75的反向鎖序。

要求：所有可增長的非使用者實體配置也必須先取得同一全系統budget的預留票，再配置、成功結算/失敗退票；
或由同一個實體配置預留機制強制留下未兌現承諾。外部原子統計只供觀察，不能代替admission。
heap可先檢查空塊、放heap鎖取票、再拿heap鎖重查；多取得的票在鎖外退回，不能在80內拿75。
cache進出須同一記帳交易搬移，不可先uncharge再增加kernel_noted：舊總量100，先退10、另一人取得10、再加快取10，總量變110。
多個計數分別atomic也可能看見未完成搬移。所有release必須等實體已釋放或被同交易接管後，才釋出對應的全系統backing信用。

### B2 高：exec 的候選limit與搬移檢查矛盾

草案第5節：原limit100、保留KOBJ20，D'的limit80。image_build成功記80後搬20，mb_transfer按to.limit拒絕100>80。
最終100其實符合原配額；所以「第1步預留過，因此搬移不會失敗」不成立。
還缺少保留執行緒的domain get/rebind/put；只換p->mem.dom，thread仍指向D，最後要從哪個domain扣KOBJ？
目前sys_exec在kernel/proc.c:735後使用sched_lock發布p->space，不是只靠pstate_lk；新方案須保留其既有讀者同步。

要求：候選domain最終limit仍為L，image_build另有L-R的交易上限/保留欄位，R是精確存活物件清單。
commit在mem.mx及已定義的發布同步下，原子完成保留物件charge所有權、執行緒domain引用與proc指標搬移；
不得短暫把同一保留物件算兩份全系統backing。若無多物件原子操作，定義可回滾ticket/交易API，不只單一class mb_transfer。
舊ANON的配額可在commit後不再使用，但其尚未釋放的實體backing不能在destroy前先開放給其他承諾。
補出每個步驟前後的引用、charge、實體頁與鎖表，並對每個可失敗點注入失敗。

### B3 高：資料頁預付，頁表仍到fault才查配額，I2仍然落空

草案第4節的ANON成功只預付資料頁，PGTABLE在walk當場charge。
例如domain只剩1頁額度，mmap成功承諾1頁；首次觸及新頁表區域再要1頁PGTABLE就被拒絕，行程仍可能因fault終止。
STACK32/全額stack亦有同樣問題；root table也不可漏。
現vm_space_create在kernel/vm.c:329配置root，walk目前只有root/user布林，沒有domain參數，不能僅靠user=1判斷歸屬。
vm_space_unmap只清leaf、釋資料，不回收中間表；不得在munmap時錯退仍存活的PGTABLE額度。

要求：sbrk/mmap/stack承諾時一併預付缺少頁表的保守上限，fault消耗票、不得再次因quota拒絕；
或在申請時先建好所需頁表。fork亦要包含完整新root與中間表票、先承諾後複製。
已有的頁表可扣除，但必須在同一空間同步下避免兩個申請同用一張尚不存在的表的信用。
明確列 root、walk、split_large、部分建立失敗及destroy各自的ticket owner與回復。

### B4 高：全系統實體backing與domain配額混用，計數來源不互斥

heap總頁已包含proc/thread/frame/file/ramfs metadata等kmalloc物件；又將KOBJ/RAMFS metadata按頁加進user_charged會重複扣全系統容量。
現vm_table_pages(kernel/vm.c:91、704)含kernel與所有user表，不是草案所稱的「kernel頁表」；user PGTABLE再加又重複。
usable是在vm_init與開機測試後取剩餘free(kernel/main.c:263後)，取樣前的頁已不在usable，不能又扣全量kernel累計值。
保守重複扣有時只是提早拒絕，但不能支撐「每個實體頁唯一歸屬」與free+kernel_noted平衡的驗收。

要求：分開domain邏輯配額quota_used與全系統backing/reservation；前者可保守按物件算，後者對每個物理來源只算一次。
列一張完整partition：boot基準、live/cached heap、live/cached kstack、kernel/user頁表、ramfs資料、私有/COW承諾與共享image檔案頁。
選一個固定時間t0，若使用free(t0)，只加t0以後新增的實體占用，既有物件要有明確採納/基準規則；不要混總量與增量。
共享text由檔案backing計費而非每個space再次計物理頁，但引用仍須保住檔案與其帳務。

### B5 高：引用生命週期不完整，不能假設exec/exit後舊domain立刻歸零

現fd_inherit(kernel/proc_fs.c:95)共享struct file；pipe_release在最後inode引用才釋放pipe。
父行程退出後子行程仍持有pipe時，pipe/file的charge必須繼續存在；草案holder表卻沒有file/pipe物件引用。
proc_thread_exit(kernel/proc.c:293)釋放space後會進zombie；proc結構由wait的proc_free_list稍後才kfree。
thread_free(kernel/sched.c:438)在CPU切換後才回收kstack，且thread_struct進永久池；不可在proc最後一條離開時把這些全退。

要求：為共享file/pipe、zombie proc、延遲reaper、thread pool、domain自身配置列明holder/最後釋放者。
可以由物件保有建立者domain引用直到final release，但此時exec舊D不必歸零，且要說明共享物件在exec後是否仍佔當前行程總配額。
若選IPC獨立domain，必須另有對使用者的配額限制，不能變成繞過聚合上限的方法。
KOBJ不能只用一個總數盲目全搬或全退；需要保留/共享/暫存物件的明確清單。

### B6 中：總空閒頁足夠不代表能配置連續頁，kmalloc大小取整也不是實體上限

pmm_alloc_pages採連續first-fit；16頁中偶數頁空閒，有8頁卻拿不到2個連續頁。
kmalloc grow至少4頁且有header、alignment與fragmentation成本；ceil(payload/PAGE_SIZE)不能作為heap增長的物理上界。
已記32頁stack額度也不能使將來新頁表/核心物件需要的批次配置必定成功。

要求：I2限於已保留可兌現的單頁backing，不含其他資源/碎片造成的失敗；多頁kstack、heap擴充在公開成功前先實際取得或從有界pool預留。
KOBJ保守取整可作domain邏輯配額規則，寫清楚包含哪些header/allocated-size，不能拿它證明實體需要量。

### B7 中：memlimit既有語意與stack的承諾範圍改了，需明確規格

現sys_memlimit(kernel/proc_mem.c:244)允許把limit降至已用量以下，僅限制後續增加；草案mb_set_limit則拒絕，並非(a)所稱行為不變。
建議保留既有降低語意：已承諾不撤銷，後續正增量拒絕；查詢、歸還、同domain搬移均可繼續。若刻意改ABI需另列行為變更與測試。
stack建議預付整個可用slot：USER_STACK_SLOT/PAGE_SIZE-1 = **255頁**，含原本4頁，保護頁不算。
不接受把原先可成長的1MB slot悄悄縮成128KB可靠範圍，之後遇壓力再終止；若日後要可調stack承諾上限，另做顯式API。
fork繼承全部已承諾stack，不是第4節COW列的「已觸及頁」；與第6節一致。

### B8 中：測試最大觀察值不是KERNEL_SLACK的上界證明

不能以幾種memhog量到的峰值證明所有未記帳配置有上界。必須列出每個剩餘來源的容量常數、最大數量與頁/metadata公式。
真正固定且已在t0前配置者屬boot基準；t0後動態增加者走同一admission，或從已硬性保留且有程式上限的pool取。
網路/bcache若經heap，其物理頁不可在slack再重複計；但其可增長配置仍要有admission或嚴格上界。
測試只能驗證已列界限未被超過，不是產生界限的證明。

## 對五個問題的決定

1. **LK_MB=75有條件接受**：維持VM60→MB75，MB內不取heap/PMM/console。被動原子讀計數方案不接受；先補B1的交易與heap避免80→75流程。
2. **採255頁完整可用stack預付**，保留guard語意；同時預付頁表。32頁方案不凍結。
3. **KOBJ取整可作保守quota政策**，不當實體backing上界；明列headers、共享owner與pool結算。
4. **KERNEL_SLACK必須有來源清單與可強制的上界**；量測只補驗證。否則把動態來源納入admission。
5. **四批可以保留，但不能稱(a)完全行為不變**：
   (a) domain核心/引用/配額與ANON遷移，保留既有memlimit，先確定API/溢位/錯誤契約；
   (b) 全系統backing預留與kernel/pool結算，PGTABLE/KOBJ及fault所需頁表預付；
   (c) STACK/IMAGE/fork/exec完整交易與失敗注入；
   (d) RAMFS資料/metadata與admin路徑。
   未完成所有可增長來源前，不宣稱I2或I240結案。每批保持C3.1物理底線與現有回歸。

## 修訂版必須帶來的驗收

- 同步交錯：mb_charge對kernel grow、cache/domain搬移、釋放前再承諾；每步不超額，失敗不改狀態。
- 配額邊界：L100/R20/new80 exec成功；new81拒絕且舊程式與帳務不變；每個可失敗步驟注入。
- fork/thread建立：root/各層表、kernel stack、frame/thread建立失敗時清理，不能公布成功pid後再造一個因分配失敗死亡的子行程。
  現sys_fork(kernel/proc.c:843)有此後發布失敗路徑，需在新設計裡顯式調整。
- 已承諾匿名頁與整個stack在全局飽和後首次觸及、fork COW後首次寫入，仍可兌現；包含先前不存在的頁表。
- shared pipe跨fork/父退出、exec保留fd、zombie未wait、延遲reaper與cache往返的domain refs與charge逐步核對。
- 溢位/非法class/flags/refcount溢位、transfer同domain與超額uncharge的診斷；void API若要由呼叫者記黑盒子，須有可觀測回傳/延後事件介面。
- free物理平衡與邏輯commit平衡分開：尚未觸及的承諾不是實體頁，不能只比free+kernel_noted來證明帳務正確。

本輪僅新增審查紀錄，沒有修改Claude的草案或產品碼，未提交。
