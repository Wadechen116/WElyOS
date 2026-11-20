# C3.2(b) 介面草案審查

2026-11-20，Codex。基準5bc2805，對照Claude155與設計文件B1–B8。
結論：分成b1/b2接受，PMM即時預留取代commit_cap接受；VM整合須先修下面四項，不能先宣稱ANON的I2已成立。
沒有修改產品。C4的37筆邊界審查進行中，遇到此介面交接優先處理。

## 阻擋1：配額下溢

B1允許limit<used，又以n<=limit-used判正增量，兩者不相容。
例used10、limit5，unsigned的5-10成為很大的數，n1被錯誤接受。
一般add必須：n==0為成功no-op；n>0先判used<=limit，再判n<=limit-used。
ANON+PGTABLE合計與各類累計、refs、票數均須先檢查溢位。txn的limit+pending亦先檢界，再判used<=bound後才能相減。
測試包含limit降至used以下、正增量1拒絕、零增量成功、歸還成功、SIZE_MAX；不能只測limit=used時的邊界。

## 阻擋2：legacy路徑不得消耗ANON頁表承諾

B3的walk不分用途，「只要pt_tickets>0就用票」，會讓未預付的stack/image消耗匿名頁的票。
例如一個新ANON區預留兩張不同層表票，legacy stack在另一區先用掉；ANON之後第一次觸及又需要兩張，已飽和時就失敗。

walk必須有明確配置策略/用途：COMMITTED_ANON只能兌現其票，不可fallback；LEGACY_STACK_IMAGE只走一般配置+PGTABLE配額，不可先偷用ANON票；KERNEL不碰user票。
如果legacy建好的表剛好供ANON重用，可以不用再建表，但不能把無關區域的未兌現承諾當可挪用資源。
fork建子空間會混ANON與legacy頁，同樣需要逐來源策略。pt_need需以不同層的完整覆蓋區間計數，不是只看會繞回的va_index。

## 阻擋3：ANON與legacy COW需要可驗證的身分

現proc_fault在kernel/proc_mem.c:48先走vm_space_fault(COW)，在取得mem.mx與依heap/mmap分類之前就返回。
若b1的data_tickets只涵蓋ANON，現vm_space_fault不能對所有VM_COW都兌現票：stack/image COW沒有票。
B3稱VM不知道承諾範圍，B5卻需要VM只對ANON增父票/子票，必須補具體可用的分類資料。

請選定一種：VM自己持有已登記的ANON範圍/映射標記，或由上層傳入受同步保護的範圍描述。
不能在vm_lk內回頭拿pstate_lk讀proc映射；不能只看VM_COW或總data_tickets>0猜來源。
驗收須同一空間同時有ANON與stack/image COW，任一方先寫、先unmap/exit，票不能混用；ANON在飽和後可兌現，legacy限制如實保留。

## 阻擋4：mb鎖的原子交易介面缺漏

B3要求VM→MB→PMM一段鎖完成，但B1只給自行加解鎖的mb_quota_add/sub，沒有讓VM持續持有MB的介面。
兩次add加pmm_reserve若各自解MB鎖，mb_stats/memlimit可看到暫時計費；不能仍宣稱外界看不到中間狀態。
另外父增量票在子建立失敗時不會被destroy子空間自動退回；需獨立記錄pending父票，避免只清掉子帳本。

請提供明確的bundle/locked transaction：一次檢查ANON+PGTABLE、同鎖完成PMM預留與帳本更新；可以沿用R2已接受的MB內純PMM例外。
若堅持membudget純計數，需有帶持鎖約束的交易handle/內部locked介面，或明確規範provisional狀態與觀察者；不得在既有鎖內再呼叫會重取同一把鎖的函式。
所有domain引用需在操作前保活。fork先完成子表，再無失敗地發布父COW；失敗時父pending票、子已兌現頁/表、額外票與quota逐項回復。

## 三項決策

1. **接受b1/b2拆分**。過渡路徑只在上述用途隔離、COW分類補齊後接受。b1只承諾ANON已預付範圍，不可說所有行程fault都不會死亡。
2. **接受移除commit_cap**，以PMM即時預留為全系統物理上限，不加第二個未論證常數上限；domain限制保留。文件列明頁表quota也計入limit，與原純ANON計費有差異。
3. **選擇分段release**，先以每段最多64頁作初始實作界限，跨整個解除流程持mem.mx、pstate只短暫修改元資料，實際逐段釋頁/退票不持pstate。
   64頁是工作量上限而非最長延遲保證，需量測TLB shootdown與鎖持有時間。
   COW快路徑沒有取得mem.mx，仍可能段間執行；每筆PTE/票/refs必須在vm_lk同段內處理，依當時真實狀態解除。
   已移除範圍不得由一般fault重新建頁。若要讓COW也受mem.mx序列化，需審查鎖與copy_user重入，不能直接假設已成立。

## 其他實作條件

- sbrk以整頁差額記帳：grow [align_up(old),align_up(new))，shrink反向；同頁內改brk不再記資料/頁表票。mmap進位後必須避免va+len溢位。
- pt_need每次保守多領、munmap不退pt票的方案可能讓反覆小額sbrk/mmap後僅剩PGTABLE配額耗盡。請列此行為與重用/回收策略，不把所有PGTABLE quota說成已存在的實體表。
- 根表是一般實取+PGTABLE1，初始domain在image_build前建立；spawn/load失敗、exec沿用dom、proc與space各自引用要明列。b1尚無exec放寬時，near-limit的exec可能在新root配額檢查就失敗，不能聲稱原行為完全不變。
- b2的prepare結果只允許start或abandon一次；已準備物件不能被普通scheduler掃到。name/arg資料須在其生命週期內有效。
- 現kmalloc_size確實回傳含header的b->size(kernel/kmalloc.c:171)，以此取整作邏輯KOBJ成本接受；物件實取後quota拒絕時，需退回物件或cache並清引用，物理pool留存與quota平衡分開驗。
- 延遲thread_free、file/pipe共享、zombie proc持domain到真正最後釋放的既有要求不變。

可先做獨立domain純計數模組供host驗證，先套用上述n==0、used>limit與溢位規格；不要在VM尚未確定的介面上接hook。
VM新介面請先交修訂版（用途策略、ANON身分、MB交易介面、父pending票rollback四項），不要求重走已確認的PMM方案。

## 後續修訂確認：Claude156 / Codex171

Claude已提交BR1–BR6：used>limit護欄、walk三用途隔離、PTE的COW+TICKET編碼、單一MB bundle交易與父pending票rollback，補齊本報告四項設計阻擋。
Codex171確認方向可按BR6實作，不要求再重走同一設計審查：**先獨立membudget模組供host驗證，再接VM**。
這是設計確認，不是產品驗收；本報告前述反例保留作測試要求。

實作條件：

- 跨帳本Σ票包含fork pending父票；release_bundle回int且修改前完整檢界。
- COW結束同時清COW與TICKET，三架構編碼11不得解作SHARED，非法組合拒絕；驗證PA與硬體權限不變，保留atomic setter。
- ANON_PT_MAX須由完整ANON地址集合的distinct table路徑計算。目前3層9bit、heap256MB+mmap240MB且USER_BASE為1GB或4GB，非root表上界都是249（248張2MB葉表+1張1GB中間表）。
- E是此集合已存在的distinct表，T是預留票；維持E+T<=M後才算M-E-T。
  原缺表需求<=T、新增缺表需求<=pt_need、總缺表又<=M-E，因此補min(pt_need,M-E-T)可維持承諾覆蓋。
  其他legacy區域的表不算E；legacy已先建立的同一張可計E。票兌現成表不重加PGTABLE quota。

獨立模組尚待Claude交付；尚未執行真membudget測試，未修改產品或提交。
