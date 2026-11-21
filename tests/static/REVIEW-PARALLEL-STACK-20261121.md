# Stack 交易修正版獨立複核（2026-11-21）

現版原始碼支持 Claude 174/175 所述修正：`user_thread_prepare` 在
`pmem_lock` 內執行佔格與初始對映；準備 kernel thread 失敗時，再於同一把
mutex 內解除整格頁面並釋放自己的 slot。`proc_fault` 的 stack-growth 分支亦
持有 `mem.mx`。因此原先「初始對映途中 fault 插入並覆寫 PTE」的時序已被
序列化；對映結束到 thread_prepare 失敗之間仍可長出深頁，但整格回退涵蓋它。
spinlock 只包 slot bitmap 的短操作，配置頁面不在 pstate spinlock 內。

## 實際執行結果

直接執行既有 `python3 tests/static/probe_stackfix_transaction.py` **編譯失敗**，
不是產品執行失敗：舊 fixture 未定義新加入的 `pmem_lock/pmem_unlock`。
編譯診斷留在 `/tmp/welyos-stackfix-transaction-cxx2cqrx/none.c` 可重現。
舊報告描述的反例不應直接當作現版仍然存在的證據。

本次未修改共用 probe，僅在 `/tmp` 衍生 fixture，仍抽取現版真實的
map_page/map_user_stack/unmap_user_stack/alloc_user_stack/free_user_stack_slot/
user_thread_prepare/proc_fault_locked 七個函式。fixture 將 mapping callback
期間的 fault 延後至 mem.mx 解鎖，以模擬允許的序列化時序，並改成檢查
修正後應當無殘留的結果。

- 13 項 sequential allocation/map/prepare failure、retry、supplied-stack
  preservation 檢查通過。輸出沿用舊模板含 `creator uniqueness` 字樣，惟本次
  已移除不符合 mutex 模型的遞迴 creator 注入，**不宣稱測過並行 creator**。
- 深頁 fault 發生於初始對映完成後、kernel prepare 失敗前：fault rc=0，
  rollback 後 bitmap 回原值、mapping=0、physical page=0。
- 第一張初始 stack 頁的同頁 fault：無 PTE replacement，rollback 後 mapping=0、
  physical page=0。
- 移除部分對映回退：指定 exit 10 抓到。
- 移除 bitmap 放回：指定 exit 10 抓到。
- 把整格解除改回只解除初始四頁：深頁測試指定 exit 20 抓到。
- baseline exit 0，UBSan 無診斷；三個產品輸入執行前後一致。

完整衍生 C、執行檔與結果：
`/tmp/welyos-parallel-stack-3m8hpy8k/results.json`。

## 綁定來源與限制

| File | SHA-256 |
| --- | --- |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
| kernel/proc_internal.h | b41a25b8e6ee39ea4e4b41063866289d81b2bf98a2a5d41b525a1f2a46b4ee1b |
| tests/static/probe_stackfix_transaction.py | 44cdc6402c87e7cb2dd135baaaf1e12e9d6bef01e6c6fd53c4407154cfe36f59 |

PMM/VM/heap/scheduler 與 mutex 是 host fixture；這次只驗 helper 邏輯及受允許的
故障時序，不是實際 SMP 或真實 mutex 鎖序驗收。未執行 QEMU/GPU、未改產品、
共用 probe、baseline，未 commit/push。並行 creator 一成一敗、exec/fork 與建立
thread 同時交錯，以及 thread 正常退出後 slot 回收（I-330）仍須另驗。
stack 預留保證（I-240）亦未因本次 rollback 通過而結案。
