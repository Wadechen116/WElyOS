# 平行獨立檢查：fork 失敗原子性（2026-11-21）

本次 host 結果支持 Claude 175 的順序修正，未重現原先 thread_prepare 失敗後父行程殘留 COW/ticket 的反例。未執行 QEMU、修改產品或既有 probe、commit/push。

## 實測

- probe_fork_caller_rollback.py：兩層與三層各 320 組 VM failure/lifecycle 案例 PASS；三個突變（漏父 pending 退票、漏 destroy child、漏 root quota rollback）在兩種深度均以 exit 90 抓到。證據 /tmp/welyos-fork-caller-vcfju7f4/results.json，changed_input=false。
- 舊 probe_tempobj_fork_late_failure.py 失敗於抽取已刪除的 pmem_fork，ValueError: pmem_fork。這是 probe 介面過時，不是現版產品反例或驗收 PASS。
- /tmp 獨立調整版抽取現版原 sys_fork、fork_child_main、pmem_fork_domain、pmem_fork_space、release/domain/KOBJ helpers、VM fork/destroy 與 membudget 函式，沿用舊 fixture，將新 thread_abandon 邊界以釋放未啟動 fixture thread 實現，失敗 oracle 改為父完全不變。
- 1–4 張 writable resident anonymous 頁各測 7 個失敗情境：明確拒絕 thread_prepare，以及連續第 1–6 次 heap allocation 失敗（proc、fork_start、frame、domain、prepared thread、VM space metadata），共 28 組皆回 -E_NOMEM，父 tickets=0、reservation delta=0、changed PTEs=0、child live delta=0，沒有公布子行程或啟動 thread。最後 heap/reservation/physical references/table pages 全歸零。
- 另 12 組成功/child dying/thread killed 的實際 fork_child_main 控制 PASS，fork_start/frame 在 fixture lifecycle 下沒有漏釋放。
- 故意漏掉 caller 的 thread_abandon(ct) 突變，在 VM metadata allocation 失敗路徑以 exit 10 抓到。UBSan 無診斷、編譯無警告。證據 /tmp/welyos-parallel-fork-qxwakjc8/{results.json,caller-current.c,omit-abandon.c,reproduce.py}，changed_inputs=[]。末句沿用舊 probe 的「3 early allocation failures」文字，實際迴圈已擴成六個 heap 點。

## 來源 SHA-256

| 檔案 | SHA-256 |
| --- | --- |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
| kernel/vm.c | bf6531b8960779f85538a552eed96fef540ced65cfe01c9d9045ae1915643b8d |
| kernel/membudget.c | 4a7fc43c3c4f4275b9def828801df8ed9e07aa471ab2c25343b1d6f251ca7c50 |

## 判讀與限制

現版 proc.c:886–906 是 domain → thread_prepare → fork_space，VM 失敗 abandon prepared thread 並 release domain，之後才公布/start。pmem_fork_space 持 parent mem.mx。測試支持此順序解決指定失敗原子性問題。

這不是全 fork integration/SMP/實際 PMM 耗盡驗收。thread/domain limit、locks、MMU walk/PTE codecs、PMM、VFS/signal 等仍有 fixture；caller 調整版未同時注入 VM 每一內部 PMM/quota 點，該部分由 VM probe 分開涵蓋。fixture thread 未驗真 thread_prepare domain KOBJ，VFS 成功後步驟不會由此證明永不失敗。暫時物件 fork_start/frame 尚未記 KOBJ 是另一項 open 工作，不能以本次無 heap leak 宣稱完成記帳。stack/image/exec 等缺口不在本次結案範圍。
