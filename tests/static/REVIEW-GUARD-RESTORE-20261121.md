# Kernel stack guard 恢復路徑複核

執行 `python3 tests/static/probe_guard_restore_current.py`，兩層與三層 host
頁表模型共四項檢查通過，UBSan 無診斷，來源執行前後不變。

抽取真 `stack_alloc/stack_release`、`walk/new_table/split_large`、
`vm_map/vm_unmap/map_large` 與其計算 helper。從大分頁起始，先確認建立
stack 所需的拆頁表配置失敗時，stack_alloc 回 NULL 並歸還五頁區塊。
成功拆分後，拒絕所有新的單頁配置，再做 101 次 stack 分配／guard 移除／
恢復循環；恢復正確 PTE、每次歸還五頁、配置嘗試計數完全不再增加。

這支持目前成功 `vm_unmap` 留下中間表、`stack_release` 重用該表的契約。
目前 kernel vm_unmap 不回收中間表；不能僅因 vm_map 的一般介面可能配置，
就斷言此特定配對路徑在耗盡時必定配置失敗。

證據 `/tmp/welyos-guard-restore-n8c0lpzk/results.json` 與生成 C／執行檔。

| 來源 | SHA-256 |
| --- | --- |
| kernel/vm.c | bf6531b8960779f85538a552eed96fef540ced65cfe01c9d9045ae1915643b8d |
| kernel/sched.c | 7748c32b0ed821bde02985abe3cbb4eda57d4611b7f8e00baf3c3dfb74a0f3c8 |

限制：PTE 編碼、PMM、spinlock、TLB 是 fixture，非三架構實際 MMU、
SMP 或真實記憶體耗盡驗收；未涵蓋 stack_cache、thread pool 與延後回收
整體生命週期。未執行 QEMU／GPU，未修改產品。後續若 kernel 中間表
加入回收、large-page 合併或其他重建路徑，必須重驗此配對不變量。
