# c2+c3 固定來源獨立 exec 複核

**結論：單一 new_vs 憑證綁定契約尚未實作，列為 c3 驗收阻擋。**
現有 sys_exec 只建立一個新空間，成功前清其txn，未見現有user syscall可濫用
此缺口；不能把內部API契約反例描述成已存在的使用者提權或配額繞過。

## 固定來源與實際驗證

以git show 512e496重建基準，在/tmp套用固定包source.patch（--fuzz=0）；
manifest14個來源hash全部符合。證據：
`/tmp/welyos-c3-independent-7b2qirwz/verified.json`。
source.patch SHA-256：`14b5b6873b854e3d6698117c22f02d42b0f6e2f780acd3c8d609cd61515481b9`。
本輪未使用正在變動的共享工作樹代替固定來源，未執行QEMU。

最小host probe抽取固定來源的**全部membudget函式**與真
vm_space_create_txn/clear_txn/quota_total。stub僅替代鎖、heap、PMM預留和
root-table配置；不需要大型image/檔案fixture來證明綁定問題。

執行結果（compile0、exit0、UBSan無診斷）：

1. L=100、已用舊IMAGE80、pending80，begin一次後，兩個不同vm_space a/b
   都成功執行vm_space_create_txn(d,&t)，a->txn、b->txn都等於&t。
2. 兩空間各自帶其txn新增IMAGE30均成功，加兩張根表，domain total142，
   超普通limit100；同一憑證實際供兩個帳本借用信用。
3. 此時普通mb_charge_object(d,1)正確拒絕，total/refs均不變。
4. 最終check在limit100時接受total142-old80=62；改limit50後正確拒絕。
5. 清兩空間txn並destroy fixture退帳、mb_exec_end後，舊total80/limit50，
   再帶已結束憑證加1正確按普通上限拒絕。最後heap/reserved/errors全零。

完整可編譯C、binary、結果與來源hash：
`/tmp/welyos-c3-bound-host-91ovfhi6/probe.c`、`results.json`。
可重跑該binary，或以 `cc -std=c11 -O1 -fsanitize=undefined
-fno-sanitize-recover=all probe.c -o probe` 重新編譯。exit0包含成功重現契約
反例，不是c3驗收PASS。

## 阻擋：憑證沒有唯一 bound owner

include/membudget.h的mb_exec_txn只有d/pending/active；vm_space_create_txn
直接`vs->txn = dom ? txn : NULL`，沒有bind檢查/登記。membudget只確認active
與domain相同，所以第二個空間根表與後續ledger可共享同一憑證。這與(c)定案
要求「綁定新空間」以及R4「只有記入那個new_vs帳本」不一致。

最小修正方向：加入唯一bound-space身分，建立新空間時原子claim，拒絕同一
active txn綁第二空間或不同domain；root配置失敗、建立中止/成功clear時按
明訂生命週期解除。所有VM txn記帳應確認vs就是bound；mb_exec_end後拒絕
重新借信用。不能只加註解要求caller不要傳第二次，否則沒有可驗證的綁定契約。
修正後用同一反例驗第二create遭拒、第一ledger完整，並測首次root失敗後
允許何種retry與stale/ended憑證拒絕。

generic mb_quota_add_txn(d,MB_KOBJ,1,&t)目前也接受有效信用；host已展示，但
**未觀察到產品以此方式記長命KOBJ**。若該API明確保留為受信VM內部介面，
此項屬防誤用邊界而非另稱現存濫用。owner可限制txn允許的類別/owner來強化
保證，不能把普通mb_charge_object路徑錯報為已借pending。

## 已核對的正常 caller／生命週期接線

- sys_exec先get args、單thread檢查、access，再begin；begin/access失敗不使用
  未初始化txn。begin成功後image_build失敗會由其image_discard清新space/新exe，
  再end一次；final-check失敗先discard再end，舊p->space/exe尚未發布。
- image_build的vfs_open在create_txn前，而且沒有txn參數；mb_charge_object
  使用普通mb_quota_add。新exe file與保留proc/thread/kernel stack/file/pipe
  KOBJ因此目前不借pending。pending來自舊space quota總和，正常不含KOBJ。
- 成功final-check之後clear新vs->txn，再發布p->space，更新image範圍與slot0
  身分（tag=1），切space、destroy舊space，最後end。沒有發現成功返回user mode
  後vs仍保存呼叫者stack上txn指標的現存出口。
- final-check真函式依當下limit重新計算total-old_now，能抓到check之前的limit
  調低。check放鎖後到publish不是一個全程持鎖區段；應把check視為admission
  完成點。其後合法memlimit再調低允許used>limit，不能要求永久final≤limit。
- txn不改PMM實體預留條件；尚未看到以pending額度當作實際free頁使用。

以上sys_exec分支為真來源靜態逐出口審查；本輪host沒有執行整個sys_exec或
image_build故障注入，不將接線檢查冒充完整caller動態驗收。現有owner
exec_txn_test測一般成功/超限/普通KOBJ拒絕/ended憑證，沒有測第二space綁定。
全部begin後image讀檔/配置故障點及中途memlimit調整的caller整合仍應列獨立
測試強度缺口，但本輪沒有據此虛構新產品反例。

## 來源 hash 與範圍

| File | SHA-256 |
| --- | --- |
| kernel/membudget.c | 6ca2e40ffad35ff75014d33109fea4c8dc4d710d07f258bf407b273fc501c9da |
| include/membudget.h | a60dad3462fdc01e9f0b6447f14ea62044e6863eb68c289877d12264378e6bec |
| kernel/vm.c | ab09abc24f6d3d61cab4a078142289a296409b630ea3432a84c42580919d8872 |
| kernel/proc.c | 1c9ba1326bcbf8c9cdb06809aec0b4c89edfb73a4b519f03814d1dadfb2b2c0f |

只新增此報告與/tmp只讀重建/host證據；未改產品、baseline或共享probe，
未QEMU/GPU、commit/push。root另做boot/log/QEMU驗收，本報告不重複主張。
