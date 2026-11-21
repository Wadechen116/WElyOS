# 最新 fork caller 可重跑檢查（2026-11-21）

新增工具 `python3 tests/static/probe_fork_caller_current.py`，將先前 /tmp 的新介面 caller 檢查納入專案；沒有改既有 probe、產品、baseline，沒有 QEMU/GPU/commit。

## 結果

- 28 組失敗案例 PASS：1–4 張 writable anonymous resident 頁，各測強制 thread_prepare 失敗與第 1–6 個 heap allocation 失敗（proc/fork_start/frame/domain/thread/VM metadata）。確認 -E_NOMEM、未公布/start、父 PTE/tickets/reservations 不變、子 heap 無殘留；最終 refs/table/heap/reservations 清空。
- 12 組實際 fork_child_main 成功、dying、killed lifecycle 控制 PASS，fork_start/frame 無 heap leak。
- 刻意漏 thread_abandon 突變被 exit 10 抓到；原碼 exit 0，UBSan 無診斷、編譯 stderr 空。
- 證據 `/tmp/welyos-fork-current-3l7w6u0u/results.json` 與生成 C，changed_inputs=[]。工具記錄產品與依賴 fixture 的 SHA-256，任何測試中來源變動均非 PASS。沿用既有 probe 的 PRE/MAIN fixture，但重新抽取 split pmem helpers，並使用父不變 oracle；不會呼叫舊 probe 已失配的 main。

| 來源 | SHA-256 |
| --- | --- |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
| kernel/vm.c | bf6531b8960779f85538a552eed96fef540ced65cfe01c9d9045ae1915643b8d |
| kernel/membudget.c | 4a7fc43c3c4f4275b9def828801df8ed9e07aa471ab2c25343b1d6f251ca7c50 |

## 成功 VM commit 之後的 VFS 前提

目前真碼可以支持「一般有效狀態不再有可回報的配置失敗」：

- proc.c:909 的 files_init_cwd（proc_fs.c:123）僅持 pstate_lk 複製固定 cwd 字串，無配置。
- vfs_dup（vfs.c:333）持 inode_lk 增加既有 file 的 refs，直接回原指標，沒有配置或錯誤分支。
- vfs_exec_hold（vfs.c:454）唯一負回傳為 inode type != I_FILE。父 exe 的來源 image_build（proc.c:243）在接受映像之前已呼叫此函式成功；fork 只 dup 同 file，故在有效父 exe/type 不變契約下此拒絕條件不成立。函式持 inode mutex 增 exec_count，沒有配置。
- fd_inherit（proc_fs.c:95）在 pstate_lk 下有限迴圈（PROC_MAX_FD=16）對每個既有 fd 呼叫 vfs_dup，沒有新 file 物件或 allocation。這是繼承既有 file 的 reference，不是把建立者 KOBJ 重新記到 child。

具體防禦缺口：file refs 與 inode exec_count 都是 unsigned，增量沒有上限檢查。抽取真 vfs_dup/exec_hold 的小型 host 邊界 probe 顯示，fixture 初始 UINT_MAX 時兩者均繞回 0，exec_hold 仍回 0；另普通 I_FILE 成功、non-I_FILE 拒絕控制均符合真碼。證據 `/tmp/welyos-fork-vfs-contract-upneu_n4/results.json`，UBSan 無診斷、來源不變。這證明算術邊界缺保護，但未證明正常有限記憶體系統能達到 UINT_MAX 個同時持有者，不能宣稱正常可觸發的 fork bug 或將它升級為本批阻擋。若日後加入會回失敗的 refs/exec_count 飽和保護，必須同步重設 fork commit 順序，不能在父 COW 提交後直接新增可回傳錯誤。

VFS 檢查綁定：kernel/vfs.c f34897f8af4065ad09395aa441dc142fa096fb9dbe1332c143093914497a4c44；kernel/proc_fs.c 01be2e9b57172cbbdd450dca8e2e5f3a6d50e4273a7057106b98fdd3ad49b768；include/fs.h bc4bd4b4f08d513c4a208b7b0859a286b1af85d2cc9bba3d4d57e7205dc8b1ba。

## 保留缺口

這是抽取真 caller/helper 的 host 檢查，PMM、locks、thread scheduler/domain limit、PTE codecs/walk、VFS/signals 是 fixture，不是完整 SMP/架構/實際 PMM 耗盡驗收。VM 每個內部 PMM/quota 故障點由另個 VM probe 分開驗，不代表此 caller 版本已全組合覆蓋。USTART/fork_start/frame 的 KOBJ 記帳仍 open；本次無 heap leak 不能代替記帳完成。固定里程碑 QEMU、stack/image/exec 等既有缺口不在本工具結案範圍。


## 重跑命令與證據清單（本次只讀核對補記）

以下命令均以 `/mnt/c/Users/user/swork/WElyOS` 為工作目錄；需要 Python 3、host C compiler `cc` 與 UBSan runtime。此次補記沒有重新跑測試或擴充工具。

| 命令 | 範圍與結果判讀 |
| --- | --- |
| `python3 tests/static/probe_fork_caller_current.py` | 真 sys_fork → split pmem helpers → VM；28 heap/thread 失敗 + 12 lifecycle，漏 abandon 突變須 exit10。程式整體 exit0 表示原碼和指定突變均符合 oracle 且來源未變。 |
| `python3 tests/static/probe_fork_caller_rollback.py` | 直接真 vm_space_fork/create/destroy/walk；兩/三層各320組 VM 配置、quota、reserve/ref 失敗／成功清理，三種 rollback 突變須 exit90。不是 sys_fork caller 串接。 |
| `python3 tests/static/probe_tempobj_fork_late_failure.py` | 舊反例工具；現版移除 pmem_fork 後 ValueError，不能作現版驗收命令。新 caller 工具只重用它的 PRE/MAIN fixture。 |
| `python3 /tmp/welyos-parallel-fork-qxwakjc8/reproduce.py` | 先前臨時版 driver，已納入 current 工具；沒有正式工具的失敗回傳彙整保障，應優先用正式工具。 |
| `cc -std=c11 -O1 -fsanitize=undefined -fno-sanitize-recover=all /tmp/welyos-fork-vfs-contract-upneu_n4/contract.c -o /tmp/welyos-fork-vfs-contract-upneu_n4/contract && /tmp/welyos-fork-vfs-contract-upneu_n4/contract` | 已留存生成 C 的 VFS 普通／non-file／UINT_MAX 邊界觀察；輸出 wrap 是已重現的 fixture 邊界，exit0 不代表飽和安全。此 C 為當時抽取快照，不會重新抽目前產品。 |

已核對上述 Python 工具、四組 results.json、caller 生成 C/突變 C 與 VFS contract.c 實際存在。正式 current 結果中的四個產品 SHA-256 目前仍與工作樹相符。以下 hash 綁定既有證據，未補造新驗收結果；/tmp 可能日後被清除，正式工具保留在專案可重建新證據。

| 檔案 | SHA-256 |
| --- | --- |
| tests/static/probe_fork_caller_current.py | e9d8ded8146da50378b6417ccdfda499f279b703076170eb5e8463127d4bbb8d |
| tests/static/probe_fork_caller_rollback.py | d6331d77b973877da409ced1ca2e48cfab880d1554a324dae3b2974da252d733 |
| tests/static/probe_tempobj_fork_late_failure.py | 63683ac2ef6af81d9b01b0f45c1892f513fb66583333d7885dd7905b2801fd7a |
| /tmp/welyos-fork-current-3l7w6u0u/results.json | b5afe4424c3580013fa036722e7ee0dcc0d6f8d4539742a2a2b201b3dd6ad111 |
| /tmp/welyos-fork-caller-vcfju7f4/results.json | 52a0840e9674d7bbd793fc713a2da505025076ba9811c13e1ce0d23fa56db630 |
| /tmp/welyos-parallel-fork-qxwakjc8/results.json | cfb313c55b07b68f8c55f0f2498d59b9b60469578522093376a334829b411fb5 |
| /tmp/welyos-fork-vfs-contract-upneu_n4/results.json | 7053740a60ae3563b53aee974c64f3148e0da57de43b127ec905271cc536c6a5 |

仍未涵蓋：真 caller 同時串入 VM root／各層子表 PMM/quota 每點故障注入（這些只在 VM-only 工具各自驗）；實際 sched/thread KOBJ／domain limit、mutex/spinlock 競爭、CPU TLB shootdown、真 PMM/refcount、arch MMU、VFS reference 並行變動與真 syscall 整合。既有結果不能合併宣稱上述全通過。
