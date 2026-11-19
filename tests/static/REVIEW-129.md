# Step 47：proc 模組拆分的獨立審查

2026-11-19，使用者指示「請協助claude」。讀取 Claude 留言 125/126；審查來源 HEAD e20248a 上的未提交 step 47。未改 Claude 的 kernel、架構檢查腳本、baseline、gate 或 reference。

## 結果

未在這次拆分的 fork 順序／新介面中證實新的 kernel 正確性缺陷；三筆 baseline 精確呼叫可接受。確定找到的是 `scripts/arch-check.sh` 的六類漏檢及兩類誤報，已有可套用、隔離測過的補丁。這不是整個行程子系統或 SMP 正確性認證。

交付：更新 probe_seu_race.py；新增 probe_proc_split.py、tests/host/proc/split_probe.c、probe_arch_boundaries.py；baseline-crosscheck-129.json（來源指紋、逐筆理由與函式比對）；兩個 patches/*129.patch。

## 1. 三筆 baseline

|新 ID|函式|判定|
|---|---|---|
|fd5f1a46a727d2fe3e611f78|proc_mem.c commit_release_all|持有 pstate_lk，delta 非正；既有有界記帳不變量下 release 不回 NOMEM。|
|5be00e3472b51c46eee5e4de|proc_mem.c sys_munmap|持有 pstate_lk，以負 pages 歸還；實際 unmap 仍在解鎖後。|
|81ccb02baa7772dd72d6b162|proc.c sys_fork 的 vfs_exec_hold|c->exe 仍是 vfs_dup 已持有的同一 regular executable；dup 不配置、不失敗，exec_hold 只有非 I_FILE 才拒絕。只接受此呼叫，未接受整個 fork。|

前兩個函式及 commit_change_locked 與 HEAD 的 body tokens，經明列的 p->mem 子結構／鎖改名正規化後相同；sys_fork body 確實有變，已另審。所有新 ID、當前函式 hash 與本次新掃描相符。patches/baseline-crosscheck-129.patch 只補三筆 cross-check metadata，並將舊理由前段的 holds fd_lk 更正為 holds pstate_lk；不新增／刪除接受的 ID。git apply --check 通過，未直接套用。

## 2. fork、exec 與新介面

87 個原 proc.c 函式均在新模組找到；78 個 body 在明列欄位／鎖改名後 token 相同，9 個另看差異：proc_create、proc_became_zombie、proc_wait_child、sys_procinfo、user_return_check、sys_dup2、sys_exec、sys_fork、syscall_dispatch。另檢視 18 個新增介面。此比對不包含全域初始化、宣告／可見性、preprocessor、link 或效能的等價證明。

- pmem_fork 在 pstate_lk 下先記帳，再複製 maps/mmap_next/brk。子結構由 kzalloc 清零，記帳拒絕時沒有增加 child.commit/commit_total；呼叫者走 nomem。VM fork 失敗時呼叫 commit_release_all(c)；thread_create 失敗的既有清理仍歸還記帳、VM、exe 與 fd。失敗時 maps 等仍會被複製，但未發布的 child 隨即釋放，這裡未使用它去 unmap 父行程。
- brk 由 VM fork 後提前到之前，於目前限制下未見改變可觀察快照：sys_fork 要求 parent.nthreads == 1；其他行程不能替它執行 sbrk/mmap，SEU 只排隊、由目標自己的 return checkpoint 套用。VM fork 期間同一父執行緒不執行 user signal handler；remote signal 可改訊號狀態，不能改 brk/maps。這不是將來允許多執行緒 fork 時仍安全的承諾。
- parent.mem.limit 仍在鎖外讀入 child，與舊 fork 相同；目前單執行緒父行程且無跨行程 memlimit setter，未證實新增競態。若新增該能力，要把整份繼承快照納入一致同步契約。
- pmem_init/pmem_exec 的配額繼承、exec 清 heap/maps、psig_fork 只繼承 handler/restorer（不繼承 pending/stop）、psig_exec 保留 IGN 並清 pending，均符合原程式順序。psig_fork 無 sched_lock 的原前提是只有父行程自己透過 sys_signal 改 handler，且 fork 拒絕多執行緒；不宣稱它可任意獨立呼叫。
- psig_stopped/stop_unreported/report_stop 在 proc_wait_child、proc_became_zombie、sys_procinfo 的呼叫點仍處於原 sched_lock 範圍。report_stop 的標記與訊號讀取仍在同一鎖內。
- fd_in_range/replace 與原 dup2 的範圍檢查、鎖內交換、鎖外 drop old ref 相同；files_init_cwd 新增 kernel-created case 的 NUL，parent case 用 pstate_lk 複製。sys_exec 的 helper 次序與舊內嵌程式一致。
- proc_pid/pgid/has_cap/user_addr_ok 與五個 cred syscall 的 wrapper 保留舊判斷、回傳與鎖區間；SEU dispatch 參數轉送與原 switch 一致。syscall.c 的 opaque proc 邊界有編譯器支持；內部子結構的 owner 限制目前仍是詞法規則。

17 項 host checks PASS，/tmp/welyos-proc-split-g911pstn/results.json：配額初始化／繼承、fork 成功與行程／全域 quota 拒絕、commit 歸還、munmap 與重複拒絕、exec reset、signal 繼承與 IGN、stop report、cwd 初始化／繼承、fd 範圍與交換。摘取 14 個真實函式，採有界 host struct 與 lock stub，ASan/UBSan、警告視錯誤、changed_inputs 空。沒有執行真實 VM/scheduler、完整 sys_fork lifecycle 或 SMP。

負對照：/tmp/welyos-split-129-negative-controls.json。刪掉 fork 的 brk 複製、exec pending 清除、fd slot 更新，各自確實被 assertion 抓到；都是 /tmp 副本，不動產品碼。

## 3. SEU 探針

改為摘取 proc_mem.c 的完整 sys_seu / pmem_apply_injection，host proc 模型使用 mem 子結構。保留 busy slot 與確定性交錯，增加檢查第一次只能改 A（[1,0]），第二次才改 B（[1,2]）。也用 token/source guard 確認 user_return_check 仍呼叫 consumer，dispatch SYS_seu 仍按原參數轉送；這兩項是來源接線檢查，不是假裝在 host 執行完整 user-return/dispatch。

最終執行 PASS：/tmp/welyos-seu-race-ntytszi9/results.json，busy=-16、slot_preserved=1、publication_rc=0、after_first=[1,0]、after_second=[1,2]、pending=0，UBSan、changed_inputs 空。負對照把 payload 讀取移到 unlock 後，確實重現錯誤並 exit 1（/tmp/welyos-seu-negative-5wktukl9）。不是完整真實 SMP race proof。

## 4. 架構檢查的可重現問題與補丁

probe_arch_boundaries.py 在隔離目錄執行原腳本，**3/11 符合預期**，/tmp/welyos-arch-boundaries-w6gum8ur/results.json。

漏檢：target->sig.stopped（非常見變數名）、p -> sig . stopped（空白）、&p->mem（取得整個 group 位址）、foreign header 內存取、#include "./proc_internal.h"、#include <proc_internal.h>。誤報：// 註解與字串 literal 中的 p->sig.stopped。這些反例是檢查器輸入，不需要本身是可建置的完整 TU；合法 C 表達式的型別仍依原 proc struct。

patches/arch-boundaries-129.patch：不依賴變數名，容許跨行空白並檢查整個 group 的存取；納入 headers；字串與註解分開處理；包含標頭按 basename／明確 allowlist 判斷。在 /tmp/welyos-arch-fix-r0_0zopw 的來源副本執行架構檢查 PASS，隔離案例 **11/11 PASS**（/tmp/welyos-arch-boundaries-4vexmh0s），git apply --check PASS。未套入 Claude 的腳本。

仍是詞法 guard，不是 type analysis：macro-expanded include、token pasting、經 alias／cast/offset 的存取都不能聲稱完整攔截；同名 group 在無關型別中也可能被保守拒絕。若要編譯器真正隔離 owner，要把 private type/representation 放回其擁有模組；不要以此補丁宣稱已做到型別層級私有化。

## 5. 耦合與鎖契約建議

目前 signal/mem 對核心的 proc_find、exit/lifecycle 呼叫有實際必要，不建議只為讓 call graph 單向而加轉發函式或 function pointer，會藏起鎖與 lifetime。若日後需要縮小依賴，可另拆 registry/lifetime service，但應與持有 sched_lock 時可安全查找／使用 proc 的契約一起設計。

建議現在先補介面文件：proc_find 的 caller 必須持有 sched_lock，回傳的是借用指標；proc_begin_exit 等 locked helpers、psig_* stop query/report 的 caller-lock 前提應在 proc_calls.h 一起標明。pstate_lk 的新註解說不能呼叫會拿其他鎖的函式，但 fd_get/fd_inherit 已明訂 pstate_lk -> inode_lk，commit_cap 初始化也查 pmm；應把註解改為不能睡眠、明列允許的鎖順序，避免與真實程式矛盾。commit_change_locked 現只在 proc_mem.c 使用，可保留 static，減少不需要的 link-visible 介面。

## 驗證範圍

checker 24 tests PASS。正常 static gate PASS：新增 hard 0、新 style 35（已有 769 未審 style）。本次新完整 scan /tmp/welyos-review-129-fast：97 accepted、0 unreviewed hard、0 stale、804 未審 style；source/tool changed 空，all gate 因 style exit 1。原 arch-check 在目前無違規來源 PASS 不等於反例檢查也通過。

本輪來源／baseline hashes 在交付前重新比對未變。未重新 build/QEMU；Claude 的三架構完整回歸是他的結果，本輪沒有將其宣稱為我方實測。未 commit/push，未修改無關的使用者檔案。
