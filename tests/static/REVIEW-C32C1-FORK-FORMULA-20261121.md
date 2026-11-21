# C3.2(c1) fork 配額／ticket公式靜態審查（2026-11-21）

此報告是owner仍開發中c1工作樹的只讀公式審查，非c32-tempobj歷史版本，也不是host/QEMU驗收。未改產品或執行測試。本輪分工只看fork範圍、quota與票；stack slot退出／重用由另一agent審查。

## 現版公式

pmem_fork_space把heap、有效mmap及每個stack_ready bit對應的255頁（排除guard）放入r，最多1+PROC_MAX_MAPS+PROC_MAX_THREADS筆，容量足夠；持parent mem.mx涵蓋範圍快照與vm_space_fork。child先接收同一ready圖；slots稍後在sys_fork複製，publish/start之前子尚未執行。guard不在承諾範圍，stack窗已加入proc_mem_boot的第三個anon window。

VM現在不再把子資料票誤當只等於ANON quota，而是：

- pending = r範圍內父resident、valid、level0且WRITE的頁數。
- child data tickets q = parent data_tickets + pending。
- child邏輯quota分別複製parent quota[ANON]及quota[STACK]；PGTABLE另加本次pt，root已由vm_space_create一般配置並記1頁。
- pt = min(sum(pt_need(each r)), anon_pt_max)，child bundle預留q+pt，再單獨替父預留pending。
- 第1趟fork_table只建子；第2趟才把父WRITE變COW+TICKET，marked必須等於pending，父data_tickets增加marked。第1趟失敗退pending並destroy子，父PTE還沒改。

在現階段只有ANON/STACK承諾來源、範圍不重疊、每個已承諾頁符合R1的前提下，這個q正確：令A為未配置頁、B為帶VM_TICKET的COW映射、X為可寫獨佔頁，父票=A+B，pending=X，子需A+B+X；父新增需X。fork_table對B直接複製ticket標誌，對X生成ticket，對A保持空洞，子已取齊其後fault所需票。

推導例（非執行測試）：一格255頁stack已兌現4頁，父data_tickets=251、pending=4，子q=255，父新增4；已fork的票式COW父再fork時pending可為0，子仍複製父A+B張票。若父COW已變成獨佔但仍帶票，應仍算B而不是X，不能按PMM refcount直接少算它；實際fault reuse會先清ticket並退票，之後WRITE時才進X。混合heap/mmap/stack只做各集合聯集，class quota不影響共享data_tickets池的R1總量。

## 失敗與整合範圍

sys_fork仍在fork_space之前完成子proc/domain、fork_start+frame聚合KOBJ、thread_prepare。子bundle拒絕、父reserve拒絕或第1趟子表建立失敗都在父標記前；caller會abandon prepared thread、release proc domain與tempobj，不新增提交後可回錯誤步驟。此為靜態呼叫順序核對，沒有故障注入結果。

child fork_table仍使用WALK_LEGACY建立現存映射的中間表。它會一般配置、額外記PGTABLE實際頁，anon_table_built僅在E+T超過全域M時扣重複票。因此最終child PGTABLE不一定只是root+最初pt，不能以「fork建表已全數兌現預付票」作測試oracle；這是現有保守策略，可能提早拒絕，但不等於成功後承諾票不足。需要後續實測核對E/T/M與實際class帳務，不能將root+pt簡化公式當真。

## 尚需驗證／不能由本報告結案

- 不重疊是pending與marked一致的前提；當前固定heap/mmap/stack地址配置提供此條件，VM API本身沒有對傳入ranges做通用去重。未來新增IMAGE ranges時必須延續不重疊，否則重數pending可在提交後panic。
- WRITE+SHARED不應存在於承諾範圍；pending迴圈只看WRITE，fork_table則排除SHARED，故若未來允許此組合，須同步改計數／分類。當前私有heap/mmap/stack producer沒有創造該狀態；未宣稱已找到可達反例。
- parent data_tickets必須只對應此次r所涵蓋的已承諾資料；目前ANON/STACK符合。IMAGE尚未納入quota複製／r／票式COW。非r內的可寫image頁仍走無票COW，一般配置可失敗。**不能因c1 stack公式正確就宣布所有fork COW飽和保證完成**；這是c2/image尚待工作的依賴。
- 案例至少應含：只stack無ANON；stack初始4頁／深頁已兌現／仍有票的COW／COW reuse後WRITE；第二次fork；父先退出與子先退出；ANON+STACK混合；每個quota/reserve/子根表及後續中間表故障。核對父PTE/票不變、child所有class quota/ref與PMM清空、publish0，並把仍保留的PGTABLE票與真正漏票分開。
- 舊probe_fork_caller_current的fixture只含MB_ANON/PGTABLE/KOBJ及舊stack欄位，不能直接拿它對c1執行失配後宣稱產品壞，也不能拿舊PASS綁新公式。等owner固定c1來源與boot再更新隔離測試。

## 本次工作樹來源hash

| 檔案 | SHA-256 |
| --- | --- |
| kernel/proc.c | 62c3e31788539e03c27e5abbced457c0a86cdd61ca1fa57d55575717021efa7c |
| kernel/proc_mem.c | d72016252e33cd65e1333af70bba5814a16b5a493054200d3df2b7ca3bfcc6b7 |
| kernel/vm.c | 689704bde4e3d374e8571549c41fcd54a3e8403f7bc13a0fa751c5c9f5e33ce4 |
| kernel/proc_internal.h | 7d1074ded480e4ced9caf6218472d5f02c7a9ed6335542e61d9c4d142241f875 |
| include/vm.h | 07253a1e9c01b4cca76eccd5b1c6cb85422c39f021e4613b0b747a1baa557519 |

來源可能隨owner繼續開發而變；此hash是本次閱讀綁定，不是不可變c1里程碑驗收。
