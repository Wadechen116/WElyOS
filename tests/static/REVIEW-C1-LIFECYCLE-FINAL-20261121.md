# c1 固定里程碑：I-330 lifecycle 與 T3 契約複核

本報告只讀複核 Claude181 的 `build/milestones/c32-c1-stack/source.patch`。
以 git show 872a25b 的各檔內容在/tmp重建基準，再以patch --fuzz=0套用完整
source.patch，manifest列出的15個來源檔**15/15完整hash符合**；不用正在變動的
c2工作樹代替c1。重建目錄與驗證：
`/tmp/welyos-c1-lifecycle-fixed-sbr0cz8y/verified.json`。
patch SHA-256：`6e4a028fc00f88b192b9435b9402a05d7c90df29ffc162b6d5bb09009d5e4a69`。

本輪未執行QEMU，也未把Claude的既有log說成我方獨立重跑；報告中的「固定」
指來源已綁定，不代表全部驗收工作已完成。

## I-330：實作已補、循序案例有證據、並行仍缺

固定c1的release_user_stack先在mem.mx內清ready，再逐頁退STACK資料配額與票，
最後放claimed格。consumer以tag=slot+1記錄自有格，supplied stack tag0不退格。
proc_thread_exit清自己的tag後release，之後detach address space並扣nthreads。
這條退出路徑足以修正既有「死過15條額外thread後永遠不能再建立」的循序限制。

UMODE_THREADCHURN使用每條512KB stack，main等churn_done後sleep2ms，再建
下一條，總共40條。Claude181報告三架構exit61，並有取消release後第15輪
失敗exit115的負對照；這支持**循序重用**範圍，不能延伸為精確最後使用者
同步或並行create/exit已驗。churn_done是在sys_thread_exit之前寫入，sleep2ms
是機會性等待，不是join/退出完成事件；不能用它當作「前一條確定已退票」的
逐步帳本證據。Claude181也明說並行create/exit和slot1-fork尚未涵蓋。

DEVLOG已明確改寫為exit開頭、扣nthreads前release，與程式一致。這個順序
保留自己的nthreads作space lifetime pin，防別條thread先成last並destroy。
但設計文件「(c)定案」仍保留detach/扣數之後的舊文字，2.5草案也寫非last
分支才release。應修文件為實際pin契約；不能為了對齊舊文字而直接把release
搬到扣數後。本輪未發現足以單靠靜態閱讀宣稱的新UAF。

fork_start保存呼叫thread的tag，child consumer採此tag；exec成功設tag1，
bitmaps設1。這些接線涵蓋slot1-survivor的意圖，但仍缺具體執行證據。I-330
可以按既有循序缺陷宣稱修復，**並行安全／slot1-fork驗收須另保留OPEN**。

## T3 頁表保留矛盾：固定包仍未釐清

source.patch重建後的設計2.5說「頁表票不退，中間表留到destroy，M上限」，
6.1 T3卻仍要求pt_tickets/nreserved等全部回原值。固定vm_space_release只退
STACK資料配額和data_tickets，不退頁表票或中間表；這不是新c2變動。

固定proc_thread_failure_probe已新增每行程reached計數並要求20次確實在stack
映射後才失敗，避免ustart提早拒絕造成空轉。可是它只回報slots/pages/reached，
**沒有回報STACK quota、data/pt票、PGTABLE quota、pmm reserved**；因此20次
通過仍不能證明T3目前文字的完整等值要求。

建議沿用BR5既定的有界保留，明確修T3而非要求不安全拆除共享中間表：

| 帳本 | prepare失敗後的正確檢查 |
| --- | --- |
| claimed/ready | 回原bitmap，失敗方只清自己的格 |
| STACK quota、data_tickets、user資料mapping/page | 回原值；包含ready期間長出的深頁 |
| PGTABLE quota | 容許增加，但等於真新建的受管理中間表與尚未用的新增pt票成本 |
| pt_tickets / global reserved | pt票可有有界保留；無其他工作時global reserved增量應等於pt票增量，不能冒稱0 |
| E+T | 始終不超M；重複失敗不得無限增長；票與表不能屬於已銷毀的space |
| destroy後 | 空間所有保留表/票/配額/reference完整退清 |

最小測試需分冷slot首失敗及暖slot重複失敗；預熱kernel stack/thread pool或
另外量測其快取，否則整體PMM free差異混入kernel快取會誤判stack票洩漏。
同一個slot的中間表若早由slot0共用，commit仍可能保守拿pt_need票但初始四頁
不用建新表，因此「沒有多出mapping」尤其不能推論「沒有多出保留票」。

## 尚缺並行create/exit：最小確定性方案

三架構、4CPU、隔離測試模式，加入僅selftest生效且依tid/request識別的hook；
不能靠降低同domain全域limit來指定單一creator失敗，否則成功方也會被干擾。
所有barrier/等待不持spinlock，controller不依賴被阻塞的mem.mx才能前進。

1. 同process有creator A/B與kernel controller；A在claim後、commit前停住，
   記錄其claimed=1/ready=0。B正常完成並啟動worker；注入A的commit拒絕。
   A回NULL後檢查只A格清除、B格ready及STACK255/票/mapping完整；B觸深頁
   讀回pattern後退出，等待**kernel退出完成事件**，核對B格可再配置。
2. C正常退出，在release清ready後、逐頁release前停住（持mem.mx但不持
   spinlock）；fault worker發起該格深頁fault、第二creator發起配置，controller
   只等待兩者已開始/被阻塞的事件，不等待它們完成，再放行C。結果不能在
   ready清除後重新長出舊頁；新creator不能在舊claimed清除前拿到同格。
   若同process讀取已退出owner stack按UB會殺process，採kernel selftest直接
   呼叫fault helper收集rc，避免把「預期拒絕」混成整個process提早死亡。
3. 兩個退出owner同時進proc_thread_exit，其中一個停在格回收後、detach前；
   另一個可完成但不能destroy仍被nthreads pin住的space。放行最後一個後
   destroy只執行一次；用tag清零、STACK票和space/domain ref實測，不只看exit0。

每組用少量確定性交錯＋20次重複，負對照至少包含ready提早設、claimed提早
清、漏release、tag未清。這是待實作方案，不是已有測試。

## 尚缺slot1唯一survivor再fork：最小可見方案

1. main(slot0/tag1)只建立一條worker(slot1/tag2)，完成握手後**sys_thread_exit**
   （不是結束整個process的sys_exit）。worker用procinfo.nthreads==1或kernel
   lifecycle事件確認main已detach/扣數，不能只sleep猜測。
2. kernel測試快照確認claimed=ready=bit1、tag2、STACK255，slot0資料頁/票已退。
   worker保存自身stack pattern與PID後fork；child consumer應仍tag2、slot1有票。
   父/子各寫既有stack頁(COW)及新深頁，讀回不同pattern；child退出後parent
   wait，確認沒有誤退不存在slot0、沒有傷父slot1。
3. 在另一分支令slot1 survivor exec測試程式：進新image確認tag1、bitmaps=1、
   舊slot1帳本隨old space清除。若c3 txn尚未做，使用寬鬆quota以隔離lifecycle
   檢查，不把exec邊界信用問題混入此測試。
4. 負對照child tag硬設1必須由slot/ledger斷言抓到，exec不重設tag也必須在
   新程式退出時抓到。成功標準需具名marker與child/parent退出碼，不能僅用
   whole selftest rc0。

caller-supplied ANON/mmap stack可用同一套kernel selftest另加小控制：建立
tag0 borrower後先退出，owner slot/tag/資料票不變；prepare失敗不碰caller
buffer。定案已將owner退出後仍保留其stack指標列為process內UB，不要求kernel
無條件保留任意指標borrower。

## 固定來源與限制

| File | SHA-256 |
| --- | --- |
| kernel/proc.c | 62c3e31788539e03c27e5abbced457c0a86cdd61ca1fa57d55575717021efa7c |
| kernel/proc_mem.c | d72016252e33cd65e1333af70bba5814a16b5a493054200d3df2b7ca3bfcc6b7 |
| kernel/proc_internal.h | 7d1074ded480e4ced9caf6218472d5f02c7a9ed6335542e61d9c4d142241f875 |
| kernel/vm.c | 689704bde4e3d374e8571549c41fcd54a3e8403f7bc13a0fa751c5c9f5e33ce4 |
| user/init.c | bd4fd8e97a990703fa534e7a86adbfaa02ebd0ca7dc886df0695617b5675bc0b |
| docs/design/C3.2-memory-accounting.md | e49db298d30f3198621c9a8e0b2c4365cdf4fdf1d7abe82d5b34f507f8b8652c |

只新增本報告和/tmp重建證據；不改共享產品，不QEMU/GPU、不commit。完整fork
ticket公式與固定boot獨立QEMU由其他agent/root負責，本報告不重複其驗收。
