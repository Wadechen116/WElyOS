# 現版記憶體交錯 probe（2026-11-21）

新增 `tests/static/probe_memory_races_current.py` 與自有 fixture `tests/host/proc/memory_race_current_probe.c`；沒有修改舊 probe、舊 fixture、產品、baseline 或共享 build。沒有執行 QEMU/GPU、commit/push。

舊工具仍要求已移除的 `commit_change_locked`，fixture 還是 `nstacks`／舊 commit 欄位；新工具改用現行 `stack_slots` 與 `vm_space_commit_anon`、`vm_space_release_anon`、`vm_space_fault_anon` 邊界。

## 結果

| 執行 | 結果 | 證據目錄 |
| --- | --- | --- |
| `python3 tests/static/probe_memory_races_current.py` | 9/9 PASS，工具 exit0 | `/tmp/welyos-memory-races-current-u98ysfgd/` |
| 同上加 `--mutate no-memmx` | 指定案例1、4失敗，另7項通過；工具 exit1 | `/tmp/welyos-memory-races-current-ogppz9c_/` |
| 同上加 `--mutate copy-outside-lock` | 指定案例2、3失敗，另7項通過；工具 exit1 | `/tmp/welyos-memory-races-current-8bv8v_q3/` |

以上三次均 `changed_inputs=[]`、`mutation_oracle_ok=True`、`sanitizer_clean=True`。使用 ASan+UBSan、關閉恢復；未見 sanitizer 診斷。每份 results.json 記錄完整 stdout/stderr、源檔與 fixture hash。負對照 exit1 是預期抓到突變，不是產品測試失敗；工具也獨立檢查失敗案例集合與指定原因，非任意編譯錯誤／crash 都算抓到。

原九項 oracle 保留：

1. fault 判定合法後與 munmap 交錯：完成後不得殘留 mapping、實體頁或模型 commitment。
2. copy_to_user 與 unmap／重用交錯：不能寫到重用後受害者頁。
3. copy_from_user 與同樣交錯：不能讀到受害者資料。
4. heap fault 與 shrink 交錯：不得殘留已釋放範圍。
5. 正常寫入。
6. 正常讀出。
7. 正常匿名 fault。
8. shrink→regrow：新區域保持模型 commitment 一致，沒有殘留舊頁；若有映射則內容為零。
9. 同頁第二次 fault：兩次皆成功，只有一頁、無頁面洩漏。

移除 mem.mx 的案例1、4會先解除區域並退票，隨後真 `vm_space_fault_anon` 觸發 `anonymous fault ... without a ticket`；fixture 的 panic 為指定 exit10。這反映現版 stale eligibility 的失敗形態，不能再期待舊版默默復活對映才算抓到。copy 突變案例2真的把受害者首位元組 `V`（86）改成 `X`（88）；案例3讀到 `V`（86）而非原本 `U`（85），兩者安全 oracle 為0、指定 exit1。

## 抽取的真函式與交錯位置

從現版原檔直接抽取、未改函式體（突變模式除外）：

- `include/vm.h`: `vm_sw_flags_ok`。
- `kernel/vm.c`: `flags_ok`、`user_range`、`vm_space_copy/map_new/unmap`、`vm_space_commit_anon/fault_anon/release_anon`。
- `kernel/proc_mem.c`: `pages_for`、`sys_munmap/sbrk`、`proc_fault_locked/proc_fault`、`copy_user/from_user/to_user`。

現版匿名 fault 在 vm_lk 內分配，故不能沿用舊 fixture 在分配器內強行插入會重入 vm_lk 的 munmap。新交錯點設在 **即將取得 vm_lk 之前**，也就是 proc 已完成合法性判定、尚未開始 VM 操作的允許窗口。mem.mx 已持有時，另一操作延到 mutex_unlock 後執行；突變拿掉 mem.mx 才能立即插入。copy 的 unmap／重用則安排在 vm_lk 解鎖時；正常版已複製完，突變版在複製前解鎖，故會暴露 stale 實體頁。

## 證據邊界

`walk`、PTE codec、mutex/spinlock、排程、TLB、兩頁 PMM pool、reserved allocator、`pt_need`、domain 與 `mb_commit_bundle/mb_release_bundle` 均是 fixture。bundle 只用簡化數值同步 ANON commitment／reserved；頁表需求設0、不建真表樹。不驗真 PMM backing、domain 配額與參考生命週期、完整 ticket/bundle overflow／rollback、COW、SMP 或架構 TLB 行為。真 VM 函式的 data_tickets 增減與缺票檢查有執行，但這不能宣稱真預留或完整帳本已驗收。

`vm_space_fault` 的 COW 快路徑為拒絕 stub；測試只針對既有九項 mutex/copy 交錯，沒有新增「fork/copy 或 fault/exec 全部競態通過」的宣稱。初始單頁 ANON 模型狀態直接設定，沒有把 heap/mmap 視窗當成完整產品帳本。

## 來源 hash

| 檔案 | SHA-256 |
| --- | --- |
| `kernel/vm.c` | `bf6531b8960779f85538a552eed96fef540ced65cfe01c9d9045ae1915643b8d` |
| `kernel/proc_mem.c` | `f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a` |
| `include/vm.h` | `b47e709635455db61709c8496337f963fec484d4e93cf05726a529ae2129f1b1` |
| `include/uapi/errno.h` | `10414b65f6a3543545a79aea66577b5031b440284332a5639353870103e2bebd` |
| 自有 fixture | `93f97d56edc70c5b0b5ff62e60b26496034692a83ffb0f2cbfcc2eed1529c5a8` |
