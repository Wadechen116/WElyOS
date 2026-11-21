# (c1) STACK 建立／回退／exit/tag：進行中只讀審查

讀到 Claude179 的 c32-tempobj 固定里程碑後，另對**尚在修改中的(c1)**做只讀
快照；沒有將179的QEMU結果移植為(c1)通過。來源凍結讀取目錄：
`/tmp/welyos-c1-readonly-ern60uzp`，其 hashes.json 包含全部來源。

## 已見到的實作連線（不是最終驗收）

- alloc_user_stack 在pstate_lk下claim唯一slot，放鎖後commit STACK255 + pt；
  commit_user_stack用票兌現初始4頁，最後才設ready。commit失敗只清自己的
  claimed bit。這避免讓fault看到尚未承諾的slot並耗錯票。
- proc_fault_locked 同時檢查claimed/ready和guard，並以票兌現；不是原先
  pmm_alloc的一般配置。ready未開時回拒絕，沒有遞迴配置假裝commit。
- release_user_stack 在mem.mx內先清ready、逐頁release STACK資料頁/票，
  解鎖後才清claimed；下一creator不能在舊頁還沒退完時重用同slot。
- user_thread_prepare記錄owned_slot及mine。auto建立失敗才回收mine；
  caller-supplied usp採sentinel，失敗不退其buffer。image_build已承諾slot0，
  proc_create再傳owned_slot=0，不重複承諾。
- ustart.slot在consumer設成thread tag=slot+1；供給stack tag=0。
  fork_start.tag繼承呼叫thread現有tag，fork_child_main採同一tag；
  非slot0唯一存活worker再fork不再被硬改成slot0。
- sys_exec成功後bitmaps都設1且thread tag=1，覆蓋舊slot身分；thread pool
  復用經memset清零，不會把舊tag無條件帶到新kernel thread。

## 需要先釐清的具體驗收矛盾：頁表保留

路徑為：alloc_user_stack → commit_user_stack → vm_space_commit(STACK)
記入STACK255/PGTABLEpt/data票/pt票 → 初始4頁兌現 → thread_prepare失敗 →
release_user_stack → vm_space_release(STACK)。

後者只減`quota[STACK]`與data_tickets，**不退pt_tickets或PGTABLE配額，也不
釋放新建中間表**。因此對尚未到M上限的空間，首次建立失敗仍可留下新PGTABLE
配額與未使用的pt票：即便該slot的初始頁表早由slot0共用，commit仍保守拿pt_need
票，fault初始頁不一定會用完。STACK頁/bit歸零並不代表整個domain used或
nreserved回原值。

這符合既定BR5「中間表/多餘pt票直到destroy保留、總量受M界限」方向，但與
(c)設計6.1 T3逐字要求`pt_tickets/nreserved/quota全部回原值`衝突。
需要owner明訂：若沿用有界保留，T3分別核對STACK資料承諾完整回退、
PGTABLE真實建表與保留票增量、M界限、最後destroy歸零；若堅持整筆失敗
無帳務增量，則須另實作安全的頁表/票回滾。不能只看slots/pages前後相同
就稱「所有配額/票無殘留」。本輪為真程式路徑分析，未新執行反例。

## exit順序與「最後使用者」

現版proc_thread_exit先清自己的tag並release_user_stack，**之後**才
thread_set_space(NULL)、nthreads--。這與(c)定案文字「換掉位址空間、扣數之後」
不同，也與草案「非last才逐格回收、last直接destroy」不同。

目前順序有明確理由：保留自己的nthreads pin，使其他exit不能先成last並
destroy(p->space)。進入此函式後已不回user mode，執行於kernel stack，
逐頁release有VM鎖/TLB失效；所以只憑先後不同**不能判定新UAF**。
owner應將實際契約寫清楚並以真SMP驗：A回收後尚未detach、B同時exit、C想
重用A slot，A被搶佔再回來，最後一條destroy；要保證沒有舊user stack存取、
沒有p生命週期失效或重複退票。不可簡單照文字把release搬到nthreads--後，
那會失去pin；若換順序需新的lifetime協定。

tag先歸零可防同一owner重入再次逐格release，但proc_thread_exit為不返回介面，
它不是「同thread重複exit呼叫」的公開保證。真正要測的是已排程但未進入user
mode的dying/killed consumer：tag已設、KOBJ arg已退，仍須恰好歸還一次stack。

定案已宣告其他thread在owner真正退出後仍保留其stack指標屬未定義行為，
因此不能把任意指標持有者誤當kernel應永久保留的borrower。但caller-supplied
stack建立出的thread tag為0，退出不得清其他owner slot；測「borrower先退」
仍必須核對owner格/頁/票不變。此處不主張現版有borrower-ref實作。

## 下一批最小實驗與仍未完成

1. 兩個真正creator，一個commit拒絕一個成功，claim/ready/ledger分別核對；
   ready設前fault須拒絕，設後深頁fault→prepare失敗須完整退STACK資料票。
2. 20次以上建立/正常exit，同一格反覆重用；kernel thread延遲回收與user
   stack即時退帳分開觀察；最後全space destroy不能再退已release的STACK。
3. 主thread(slot0)先exit，slot1 worker保持運行再fork；child tag=2、ready
   與data票有效；worker再exec後tag=1、只新slot0。fork票公式由另一agent
   另審，本報告不重複驗收。
4. supplied ANON/mmap stack的prepare失敗、正常exit、dying/killed，確認
   tag0與該ANON buffer不動；borrower先exit不退owner slot。
5. 真SMP下release對深頁fault、COW快路徑、其他exit及slot重用交錯；fault
   快路徑不拿mem.mx，但VM逐頁鎖與TLB契約仍須證據。
6. 飽和後全部255頁+需新中間表皆兌現；guard保持拒絕；準備失敗時頁表保留
   必須按上節定案驗證，不沿用舊probe的「所有帳務等值」oracle。

目前c2 IMAGE、c3 exec txn仍未因c1程式出現而完成；image私有頁legacy COW
仍可能用一般配置，不能宣稱整個I-240/C3.2已結案。

## 綁定來源

| File | SHA-256 |
| --- | --- |
| kernel/proc.c | 62c3e31788539e03c27e5abbced457c0a86cdd61ca1fa57d55575717021efa7c |
| kernel/proc_mem.c | d72016252e33cd65e1333af70bba5814a16b5a493054200d3df2b7ca3bfcc6b7 |
| kernel/proc_internal.h | 7d1074ded480e4ced9caf6218472d5f02c7a9ed6335542e61d9c4d142241f875 |
| kernel/vm.c | 689704bde4e3d374e8571549c41fcd54a3e8403f7bc13a0fa751c5c9f5e33ce4 |
| kernel/sched.c | 64ebf47a94173e949d2bdb25cee241d6101d8cc384bae66812aabb800717c453 |
| docs/design/C3.2-memory-accounting.md | e49db298d30f3198621c9a8e0b2c4365cdf4fdf1d7abe82d5b34f507f8b8652c |

只新增本報告，產品/共享probe/baseline不改，無QEMU/GPU/commit。快照不是
immutable boot milestone，不做最終產品驗收；後續來源改動須另綁新hash。
