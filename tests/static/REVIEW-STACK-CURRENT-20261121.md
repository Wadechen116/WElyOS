# 現版 stack 交易：可重跑 host 與 pthread 驗證

`python3 tests/static/probe_stack_transaction_current.py` 成功完成。新工具獨立於
舊版反例 probe，抽取現版七個 stack/故障 helper，加上真正產品的
`pmem_lock`、`pmem_unlock`、`proc_fault` wrapper；所有產品函式原文不改。
fixture 的 mutex 與 pstate lock 使用真正 `pthread_mutex_t`，並以 condition
variable 協調兩個實際 host 執行緒，沒有遞迴呼叫 creator 假裝並行。

## 結果

- 13 組 sequential 檢查通過：每個初始頁位置的 PMM／VM-map 失敗、後段
  kernel prepare 失敗、失敗後成功重試，以及 caller-supplied stack 不回收。
  其中 prepare 失敗是四次重複檢查，並非四種獨立故障點。
- 100 組兩 creator 並行通過：兩者都完成各自 stack 配置並停在
  thread_prepare barrier，一方回傳失敗、一方成功。每次 slot 不同；失敗方
  rollback 後 bitmap 只留原 slot0 與成功方 slot；成功方四頁 mapping 保留，
  live page=4、ustart 無殘留。這不是 kernel thread 實際啟動測試。
- 200 組 creator／fault 並行通過：creator 在第一次 VM-map callback 仍持
  mem.mx 時暫停，另一 pthread 確認開始發動真實 proc_fault wrapper，再讓
  creator 繼續。fault 在 mutex 解鎖後才可映射，thread_prepare 等 fault
  完成後故意失敗。深頁與初始同頁各 100 次，rollback 後 slot/mapping/page
  回原值，無 PTE replacement。釋放 slot 後再 fault 均被拒絕。
- 三突變皆在指定出口抓到：漏 partial unmap → 10；漏 slot release → 10；
  整格回退縮成初始四頁 → 深頁檢查 20。
- baseline exit 0、UBSan 無診斷、產品輸入執行前後完全一致。

證據：`/tmp/welyos-stack-current-lyz7ghvx/results.json`，同目錄保留四份 C、
四個執行檔。工具正常重跑會建立另一個獨立 `/tmp/welyos-stack-current-*`
目錄並記錄新來源 hash。

| File | SHA-256 |
| --- | --- |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
| kernel/proc_internal.h | b41a25b8e6ee39ea4e4b41063866289d81b2bf98a2a5d41b525a1f2a46b4ee1b |

## 限制與下一步

這是實際多 host 執行緒搭配抽取產品 helper 的測試。PMM、VM、heap、
kernel thread_prepare、mutex primitive 均為 fixture，不能推論產品 scheduler、
真正 SMP/TLB shootdown、kernel mutex IRQ/lock-order、配額 ticket 的完整行為。
UBSan 不等於 data-race sanitizer。未測實際 thread_start/正常退出、未執行
QEMU/GPU，未改產品、舊共用 probe 或 baseline，未 commit/push。

最有價值的下一項：將相同「兩 creator 一成一敗」與深頁 fault 時序搬到
不可變 kernel milestone 的真實 SMP QEMU 故障注入，連同實際 VM/page-ticket
與 mutex 鎖序檢查；這能補 host fixture 無法提供的整合證據。I-330 正常
thread 退出時的 slot 回收與 I-240 stack 預留仍屬產品設計待辦，沒有因本次
交易 rollback 通過而結案。
