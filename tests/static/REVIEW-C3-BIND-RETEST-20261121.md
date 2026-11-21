# c3-bind 固定包獨立複驗

**結論：原來「同一 active txn 可供兩個 new_vs 借信用」的阻擋已修正，可結案。**
本輪指定的建立失敗／綁定拒絕／clear-end-destroy生命週期範圍，未發現新產品
阻擋。成功建立後的bound tombstone屬一次性憑證契約限制，與目前sys_exec
使用方式相容，不另外擴大為新缺陷。

## 固定來源核對

以 git show fbdfbe4 重建各基準檔，再在/tmp用patch --fuzz=0套用
`build/milestones/c32-c3-bind/source.patch`。manifest五個來源完整hash
**5/5相符**；證據 `/tmp/welyos-c3-bind-retest-sc3b23lt/verified.json`。

Patch SHA-256：`f004804c9cc44bdcdacf5e3375499403c633de45adbd00fad081d550dff2f14e`。

| File | SHA-256 |
| --- | --- |
| include/membudget.h | 76660c3ef7840f00468d61e28cbb9832ea26fa551428a47678b40785e35d0bbf |
| kernel/membudget.c | c7278428cf2e5718cfb7884ea4d21d35e6a92496238d246f93e8f7ea88736c87 |
| kernel/vm.c | 6f8f8639f6e87754e9b2f575f658f9c043c89d5f382b07421156f37e80ba0701 |
| kernel/test/mem_test.c | 6f68df1f29c34875bb845b504fcd1fe930990bc26ec9857e1d8e003dd033bb86 |
| tests/static/baseline.txt | 89e959af0675c70e68ab1515ae0b572c3cfc9a78e83e0d1281f162c61e33a3b2 |

baseline只讀核hash，未修改或重新核准其他基準變更。

## 真函式 host 驗證

新增可重跑工具：`python3 tests/static/probe_c3_bind_retest.py`。
工具每次自行重建固定來源，核對三個抽取來源hash，編譯真實的全部membudget
函式及VM的create_txn、clear_txn、quota_total、destroy。鎖fixture使用真正
pthread mutex；100組並行呼叫使用兩個實際pthread，不以遞迴代替並行。

105組控制通過（4組建立失敗回退＋1組拒絕/lifecycle群組＋100組雙create）：

| 情況 | 實際結果 |
| --- | --- |
| vs heap配置拒絕 | 沒有bind、quota/ref/heap/table不變；同active t重試成功 |
| mb_domain_get refs飽和拒絕 | 已bind的暫存owner解除，refs保持注入前MAX值、無charge；恢復refs後同t重試成功 |
| 根表PGTABLE quota拒絕 | get取得的ref退回、bound清除、quota/heap不變；恢復limit後重試成功 |
| 根表實體配置拒絕 | 根表quota/ref完整退回、bound清除、table與heap不殘留；同t重試成功 |
| 第二space／foreign domain／NULL domain | 全部create拒絕；原domain、foreign domain的各class/total/limit/refs、heap數與原binding均不變 |
| 非owner unbind | 不清除真正owner的binding |
| clear_txn | vs->txn清除；t.bound仍保留原owner；第二create仍拒絕 |
| destroy bound space | 真destroy退根表quota/ref/table/heap；t.bound保留tombstone，同active t仍不再綁 |
| end與新begin | ended t無法create；明確新begin清bound，新交易可綁新space |
| 100組同時create | 每組恰一成功、一拒絕；bound對應成功space；只有一張根表的quota/ref增量，destroy後歸零 |

兩個故意破壞的負對照均被抓到：漏get-failure unbind → 指定exit11；
允許覆寫既有binding → 指定exit21。baseline編譯0/exit0，UBSan無診斷。
全部測試最後heap/reserved/table/errors歸零。

證據：`/tmp/welyos-c3-bind-host-oa97w_v6/results.json`，同目錄保留固定套用
後來源、三份C與三個執行檔。最初fixture用未初始化padding做mb_stats memcmp
造成假失敗；已將fixture快照零初始化後完整重跑，未改產品。

## 實作生命週期判讀

mb_exec_bind在mb_lk內同時驗active/domain/未綁/owner非空並登記owner；
vm_space_create_txn在任何quota前完成此claim。get、quota、root三種後段
失敗出口都有mb_exec_unbind，且只有owner本人能解除。原反例中的第二space
因此無法進入根表信用記帳，真pthread互斥驗證也支持這個claim完成點。

clear_txn只清vs->txn，destroy與end不清t.bound；bound僅作不解參考的身分
標記保留到下一begin。這意謂「一個交易成功建立過一個space後，即使該space
destroy，也不在同一交易重綁」。目前sys_exec失敗直接discard/end，成功clear、
destroy舊space/end，沒有依賴成功建立後同t重試，故此限制不阻擋原修正。
後續若要支援image建立成功後的重試流程，須另定生命週期，不能偷偷當現有
unbind API已提供自動重綁保證。

本修正未將generic txn quota API變成可驗證owner身分的公開capability；它仍
是受信kernel呼叫者介面。現有VM只在create成功的vs保存txn，普通長命KOBJ
仍走不帶txn的charge，沒有重新引入已見到的第二space漏洞。本輪不擴測或
主張任意kernel caller故意偽造owner/憑證的場景。

## 範圍與限制

host替代heap/PMM/root配置、PTE codec與SMP/TLB primitive。roots沒有leaf
mapping，所以真destroy的root/quota/ref與binding生命週期已跑，但不涵蓋
任意多層mapped-data遞迴回收。ref飽和和quota/實體拒絕為確定性fixture注入，
不是實機OOM。pthread驗綁定競爭，不替代kernel scheduler/SMP整合。

未修改共享產品/baseline，未執行QEMU/GPU、未commit/push。root另做固定
boot/log及三架構獨立QEMU，本報告不把其工作重複計為我方執行證據。
