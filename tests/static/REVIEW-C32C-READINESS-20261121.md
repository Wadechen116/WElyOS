# C3.2 (c) 開工前現況與最小驗收矩陣

這是只讀盤點，不是新 (c) 設計的核准或已完成宣告。依據設計文件第 R1/R2/R4、
BR2/BR3 與實際現版程式；文件早期版本有被後文取代的規則，不能沿用 refcount
猜票、全域 MB_F_EXEC 或新 exec domain。Claude 尚未交新的 (c) 草案。

## 現況：可定位的缺口

| 範圍 | 現版路徑 | 還缺什麼 |
| --- | --- | --- |
| STACK 初始與成長 | proc.c:map_user_stack → map_page → pmm_alloc；proc_mem.c:proc_fault_locked stack 分支 → pmm_alloc + vm_space_map_new | 成功承諾整格 255 頁與必要頁表票；初始四頁和成長都以自己的票兌現；失敗完整退帳；不碰 guard |
| slot0 | image_build → map_user_stack(space,0)；proc_create/sys_exec 設 stack_slots=1 | image 建立時就記 STACK/票，勿到 user_thread_prepare 看到非零 usp 又重記；spawn/exec 第一條 thread 的 slot 身分必須明確 |
| IMAGE 私有頁 | image_build 中非 shared 分支 map_page，一般配置但沒有 IMAGE 配額 | 私有 data/BSS 與無共享頁可用時的唯讀 text fallback 分類、配額與預付；初始映射用票兌現；失敗 destroy 正確反向 |
| 檔案 shared text | image_build 的 VM_SHARED 直接引用 vfs_file_page | 維持檔案 owner 及 executable hold；不可誤記成私有 IMAGE 資料票，也不可 free 檔案頁 |
| fork STACK/IMAGE | pmem_fork_space 只傳 heap/mmap 範圍；vm_space_fork 子 bundle 只複製 MB_ANON；fork_table 只為 in_anon 頁加 VM_TICKET | 傳入每個已承諾區域/類別；子所有配額及未兌現/新COW票；父 writable→COW 新增票；IMAGE 唯讀私有頁不需 COW 票；STACK 未兌現頁仍需票 |
| COW 保證 | vm_space_fault 無 VM_TICKET 時仍 pmm_alloc | (c) 私有可寫 STACK/IMAGE COW 必須帶票，ref>1 用票、ref=1 退票；不能因舊 legacy 分支仍存在就宣稱 I2 |
| 頁表範圍 | WALK_LEGACY 供 stack/image；anon_pt_max 只覆蓋 ANON windows | 對 STACK/IMAGE 的新頁表票、可共享表的上限與 quota 歸屬；fault 不可消耗其他用途承諾的票；建立失敗多餘表的保留契約 |
| exec | sys_exec 呼叫 image_build(同domain)；mb_commit_bundle 無 txn 參數；membudget 無 mb_exec_txn | 綁 old_vs/new_vs/domain 的憑證、pending、最終 limit 驗證、成功/失敗清除、與 memlimit 調降同步；新根表也要正確受交易約束 |
| I-330 | proc_thread_exit 非最後一條只 thread_set_space(NULL)、nthreads--，不解除 stack 或清 bit；ustart 無 slot metadata | 身分從 prepare 經 start/abandon/exit 傳遞；最後使用者判定、同步回退與安全重用；不能只在 exit 猜 usp 或盲清某 bit |

已知且可由程式直接定位的 I-330 路徑：`SYS_thread_create` 每次要求 auto stack；
`proc_add_thread → user_thread_prepare` 成功後設一個新 bit；thread 正常離開走
`proc_thread_exit`，若主 thread 還活著則不進 vm_space_destroy，亦無
free_user_stack_slot。因此串行建立/結束 15 條後，第16條額外 thread 無空格。
這是 docs/ISSUES.md 已列的既有問題，不是本次新反例；本輪未新做執行反例。

## 所有權不能只看建立者

caller-supplied usp 現由內部 proc_add_thread 介面支援，公開 SYS_thread_create
目前固定傳 0；spawn 將 image 的 usp 傳入同一個 prepare helper。非零 usp
必須區分「image slot0 已承諾」「借用既有 auto slot」「外部 ANON/mmap buffer」
等情況，不能一律新增 STACK255，也不能一律當成可在退出時解除的自有 stack。

user memory 可被同 process 任意 thread 指向/使用，不能從一個初始 usp 自動
推斷沒其他借用者。新設計須明定支援契約：顯式 stack owner/borrower refs、
API 禁止共享並驗證，或將保留/釋放明確交給 caller。未定契約前不接受
「owner exit 就 unmap」作為 I-330 修正，這會有其他 thread 的 UAF 風險。
目前未宣稱有已實作回收造成的 UAF；現版根本尚未回收。

正常 exit 已先 thread_set_space(NULL) 再扣 nthreads，保護最後一條的全空間
destroy；未來每格回收需延續此安全順序，同時在 mem.mx 內完成對映/票/bit
原子切換並完成 VM 的跨 CPU TLB 失效，才可讓下一 creator 重用。不可為等
mutex 而持 sched_lock/spinlock，也不可在 p 已成 zombie 可被 wait 回收後再讀 p。
如果最後一條選擇由 vm_space_destroy 統一收尾，必須避免 per-slot 與全空間雙退。

fork 只限制 nthreads==1，不代表唯一 surviving thread 一定是原主 thread。
主 thread 可先退出而 worker 留在 slot1；之後 worker fork 的 child 繼承其
register/stack 與整個空間。需要保留實際 active slot、其他未回收格的語意；
不能把 child 一律標成 slot0。exec 則相反：成功後 active stack 必須換成
新空間 slot0，舊 slot owner/ref 不可留在同一 kernel thread metadata 上。

## exec pending 的必要邊界

pending 只能來自必然銷毀的 old_vs 帳本（ANON/STACK/IMAGE/PGTABLE），不能算
proc/thread/kernel-stack/file/pipe KOBJ。exec domain 不換，跨 fork/exec/父退出
仍有人持有的 IPC 物件繼續記原 domain。

即使 image_build 中開啟的新 executable file「看起來是建 image 的一步」，
它是可在成功後長期存活的 KOBJ，不得持 new_vs 憑證借 pending 額度。
新增 root/VM/image allocation 的信用要在 API 邊界綁定 new_vs，不能靠 TLS
「正在exec」或 domain 全域寬限。其他 thread/外部對同domain建立物件照一般
limit；實體 pmm 預留照常，不能以 pending 當真正可用記憶體。

publish 前必須依當下 limit 核對 new_vs + 所有保留物件帳本；期間 admin 調低
limit 要拒絕新 exec 並保留舊程式。舊空間真的 destroy 完才退舊票/配額，
不得先退再建新，使 kernel 配置偷走尚需保證的實體預留。

## 最小驗收矩陣

以下為待做驗收，**本輪沒有執行，也沒有宣稱產品通過**。

| 群組 | 最少案例 | 必須觀察 |
| --- | --- | --- |
| STACK 邊界 | STACK255剛好/少1頁、額度足但pmm票不足、頁表額度不足；slot0與一般slot | 失敗bit/VM/ledger/domain/pmm/ref不變；成功255承諾含4已兌現，guard無映射 |
| fault保證 | 飽和後逐觸255頁、需新中間表；guard、未claim格 | 有票觸頁永遠成功、guard/未claim拒絕、Σ各ledger票=pmm reserved |
| prepare回退 | 第1~4初始頁、每層表、ustart、kernel stack/thread結構各故障；深頁fault插入 | 不公布thread；slot/頁/票退回且不傷他人；可立即成功重試 |
| pthread/SMP | 雙creator一敗一成；同頁/深頁fault與prepare rollback；雙exit與新creator | 位址不重疊、沒有重用前殘留、無雙退/UAF、TLB一致；host後需真SMP QEMU |
| caller supplied | 外部ANON、借auto slot、image slot0；prepare失敗和正常退出 | 不重複STACK配額、不解除caller buffer；借用者仍活時不提前回收；last-user契約可驗 |
| I330反覆 | 主thread長駐，串行建立/結束遠多於16次；owner先退/borrower先退；兩者同退 | slot可重用、255承諾和頁歸還、無持續成長；最後全空間destroy不雙退 |
| 非slot0唯一thread | 主thread先退、slot1 worker成唯一thread後fork/exec/exit | fork保留實際slot所有權，child成長成功；exec改slot0；destroy完整 |
| fork類別 | ANON+STACK+IMAGE+shared text混合；父/子先寫/先exit；連續fork | 每類quota正確；writable私有映射各持COW票；readonly/shared無誤票；ref=1退票 |
| fork故障 | 子bundle、父新增票、根表及每層/每張子表、frame/thread前置失敗 | 父PTE/TLB/票完全不變、子帳歸零、不公布pid；成功後不再可失敗 |
| IMAGE輸入 | data/BSS、非整頁末尾、可shared text和fallback text、讀檔短讀 | 私有IMAGE精確、shared不誤free；每一步失敗原程式/帳本不變 |
| exec信用 | L100舊可退80新80成功、新101失敗；另有長命KOBJ時重新算邊界 | 最終new_vs+retained≤limit；pending只來自old_vs，不能補KOBJ |
| exec隔離 | 新exe file/pipe charge正好不足；其他domain/其他new_vs/過期txn重用；信用加總溢位 | 一律拒絕借錯信用，失敗無副作用；新根表合法使用且綁對空間 |
| exec提交/中止 | 每個配置/讀檔/取票點失敗；交易中memlimit調低；成功後再exec | 失敗保留舊可執行狀態；txn結束後信用不可重用；成功舊空間退清、新票完整 |
| 正常生命週期 | exec保留FD、父先exit child持IPC、zombie未wait、thread deferred free | 各owner ref/charge各退一次；wait或kernel快取不提前/延遲誤退 user stack票 |
| 負對照 | 漏STACK票、漏IMAGE charge、COW漏VM_TICKET、借pending給file、exit漏退/早退 | 每一類有指定斷言/失敗，不能只看guest exit0 |

## 本輪來源與限制

| File | SHA-256 |
| --- | --- |
| docs/design/C3.2-memory-accounting.md | 7856f463749ff22b462326721966b06060eeb519c7ce6ca7333a2696c723d24e |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
| kernel/vm.c | bf6531b8960779f85538a552eed96fef540ced65cfe01c9d9045ae1915643b8d |
| kernel/membudget.c | 4a7fc43c3c4f4275b9def828801df8ed9e07aa471ab2c25343b1d6f251ca7c50 |
| include/membudget.h | 11af72d1ef7e7fa5604e8bcbc219d154d1088ae225850d22562ba182da708099 |

只新增此報告；沒有產品改動、QEMU/GPU、commit/push。caller supplied 的借用、
pending隔離及正常回收風險是新設計必須回答的條件，不冒稱為現版已發生的新bug。
