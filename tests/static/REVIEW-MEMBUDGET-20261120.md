# C3.2(b1)：獨立membudget模組驗證

2026-11-20，Codex。基準5bc2805加Claude157的未提交模組（未接VM）。

## 結論

正常配額/預留/引用規則通過；找到 **mb_release_bundle的失敗原子性缺陷**，須修後才整合VM。
交付 `patches/membudget-release-atomic.patch`（C與header），git apply --check通過；不直接修改產品。

### 真原碼反例

1. domain持ANON2、PGTABLE1，PMM預留3。歸還ANON1、RAMFS1、預留1，其中RAMFS原本為0。
   原版先減ANON、RAMFS夾0報錯，最後仍退1張票；回-1時配額與預留皆已變動。
2. domain持ANON2、預留3。歸還ANON1、預留4。
   原版先減ANON，PMM發現全域退票超額才panic，quota已不再等於停止前狀態。

同fixture、真membudget.c+真pmm.c：原版6組中4組通過，上述2組失敗；artifact `/tmp/welyos-membudget-ohp6epjz`。
這是主機確定性重現，不是對尚未整合VM的推測。

### 補丁

- 先檢查整筆bundle所有quota；任一超額則errors++、回-1、整筆拒絕，quota與PMM預留不變。
- 類別都合法後先pmm_unreserve；若全域帳務錯而停止，quota仍未動。
- PMM成功後才扣已完整檢界的quota，此後不再有可失敗步驟。
- 單類mb_quota_sub仍保留超額夾到0的原語意，header明確區分單類與bundle。

PMM只知道全域預留，bundle呼叫者仍須驗證自己持有的未兌現票；本模組沒有自動偵測A退B的票。
因此不能以本次通過取代之後VM ledger的owner與生命週期驗證。

## 驗證範圍

新 `probe_membudget.py` + `tests/host/membudget/budget_probe.c` 編譯完整兩份產品原碼。
合成PMM位圖/refs/對齊arena；rank-checking pthread鎖替換MB75/PMM90 spin/IRQ鎖；host calloc/free替換kernel heap。
驗證觀察到的配置/釋放在MB鎖外；沒有執行真heap、VM、硬體中斷或QEMU。
主機unsigned long為64位元；有限狀態集合不代表所有整數寬度窮舉。

| 組別 | 修後結果 |
|---|---|
| scalar：六類別add/sub、0、limit低於used、夾限與error計數 | 129,948組PASS |
| bundle：兩類配額、PMM可用量、背景他人預留、成功歸還/失敗不變 | 148,500組PASS |
| edges：ULONG_MAX、加總溢位、SIZE_MAX實體預留、非法class、refs上限、get/put、配置失敗、帶用量last put | PASS |
| release-error | 整批超額拒絕、其他配額/預留不變PASS |
| release-pmm-error | PMM全域超額在quota修改前停止PASS |
| parallel | 八執行緒120,000輪get/commit/consume/release/set_limit/stats/put，最後quota=0、reserved=0、free恢復、refs=1再最後釋放PASS |

ASan/UBSan完整6組PASS：`/tmp/welyos-membudget-k4ngn59_`。
TSan並行PASS：`/tmp/welyos-membudget-n3regmuk`。
原/修後及負對照各次changed_inputs=[]；完整原碼與results.json保留在artifact。

panic錯誤案例只在單執行緒攔截停止點核對資料，之後手動釋放替身mutex跑下一例；不宣稱真核心可從panic復原。
計數exhaustive採合成既有domain值；refs上限亦由測試直接設至邊界，再調用真API，不建立數十億引用。

## 五種負對照

| mutation | 捕獲 | artifact |
|---|---|---|
| no-used-guard | 調低limit後unsigned下溢放過增量 | /tmp/welyos-membudget-9coc7n1c |
| bundle-overflow | 多類總和溢位未拒絕 | /tmp/welyos-membudget-t2lthcj0 |
| quota-before-reserve | PMM預留失敗卻已加quota | /tmp/welyos-membudget-45cv161l |
| get-overflow | refs上限未拒絕 | /tmp/welyos-membudget-9l9glw7v |
| stats-no-lock | stats無鎖讀與get/put/配額寫競態，TSan明確報data race | /tmp/welyos-membudget-2nmx1rq1 |

以上只突變暫存副本。原版兩個release錯誤案例作為補丁的直接負對照，非僅測試新增實作分支。

原與修後的完整membudget.c分別gcc -m32 freestanding -Wall/-Wextra/-Werror編譯PASS；
輸出 `/tmp/welyos-membudget-i386.o`、`/tmp/welyos-membudget-fixed-i386.o`。
checker26tests PASS、我方diff --check乾淨。

## Claude整合

memory173已交補丁與重現，待Claude套用並跑：

```sh
python3 tests/static/probe_membudget.py
python3 tests/static/probe_membudget.py --sanitizer thread --case parallel
```

整合前可用`--fix-release`只套暫存C修正；整合後不加此選項。
其QEMU membudget_test/鎖序與回歸結果仍待回報；本輪未修改產品、未commit/push。
BR2–BR5的方向確認仍見memory171，須先完成本獨立模組修補與驗證再接VM。
