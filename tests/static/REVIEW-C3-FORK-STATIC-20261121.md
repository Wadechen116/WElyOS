# C2+C3 fork_table/U1 與三筆遞迴提示獨立審查（2026-11-21）

已依 `scripts/static/README.md`、`scripts/static-gate.sh` 與rules.py規範逐筆重審三個fork_table提示，只更新 `tests/static/baseline.txt` 對應三行，沒有修改產品、其他baseline記錄、findings-fast參考或啟動QEMU。**接受的是有界遞迴提示，不是整個C2+C3驗收或航太用途可靠性證明。**

## 固定來源與變更歸因

由完整base `512e49661e2f8a955537c28c98ce463555a2c984` 的git archive在 `/tmp/welyos-c3-fork-static-7gu3go_u/source` 重建，再套fixed `build/milestones/c32-c3-exec/source.patch`。patch SHA-256 `14b5b6873b854e3d6698117c22f02d42b0f6e2f780acd3c8d609cd61515481b9`，套用成功。固定來源與本輪掃描工作樹vm.c皆為完整hash `ab09abc24f6d3d61cab4a078142289a296409b630ea3432a84c42580919d8872`。

rules.functions重新計算base fork_table token digest為舊baseline所綁 `30e65b5a75a4478ffefafe743174cdaea6dcc41c3e99b9bfe5dd5d4cfae91a52`；fixed則為 `0f67475bb8372e13ab5998bca314e87426ae24ab6df3a9b252a36777e579845d`。函式逐行diff只增加葉節點U1檢查與kprintf：有fa且可寫私有頁不在任何承諾範圍便回-1。沒有改動遞迴guard、level參數、recursive call、迴圈範圍或兩趟mark_parent傳遞。新增文字與呼叫改變token digest，因而三架構提示以新ID重發；不是新增一個無界遞迴路徑。

## 遞迴的可核對界限

- 三個固定架構mmu.h皆MMU_LEVELS=3、MMU_BITS=9。
- 唯一外部呼叫點是vm_space_fork的兩趟，均從top=MMU_LEVELS-1=2開始。
- 唯一self call只在level>0分支，傳level-1，且維持mark_parent；葉層level0不再遞迴。
- 因此有效呼叫鏈level為2→1→0，最多三個同時存在的fork_table frame。for上限由root user索引或ENTRIES-1傳入，非由新增U1改變。

這是level可達性與直接呼叫圖的人工證據。未量測每層frame bytes、walk/kprintf等被呼叫函式的stack、所有頁表項全掃描的延遲或WCET；也不驗PTE內容是否已遭硬體損壞。不能由「三層」推論整個fork可在特定時間／stack預算內完成。

## U1新增分支的範圍

U1只針對 `!(flags & VM_SHARED) && VM_WRITE`，且僅有fa（有dom與parent dom的正常記帳fork）時拒絕。第一趟父還未改；若遇範圍外私有WRITE，caller不跑第二趟，退父pending、destroy child，保留父未提交狀態。第一趟成功到第二趟期間一直持vm_lk，且來源同一組父PTE/ranges，第二趟理應不會新遇同一U1拒絕；因此新增檢查沒有令正常第二趟引入新的配置失敗。

這是靜態控制流核對。沒有在本輪執行U1反例注入或image/exec測試，不將舊6480-case leaf probe的PASS改名為C3結果。U1不是對所有PTE型態的普遍驗證：既有readonly COW不在此WRITE分支，SHARED頁刻意排除；r範圍仍須正確、不重疊，pending/marked與IMAGE producer記帳需其他驗收。kprintf新增可能增加失敗路徑輸出成本，本次baseline不保證其時序。

## 精確baseline更新

| 架構 | 舊ID | 新ID |
| --- | --- | --- |
| i386 | 7268a0e8cafaefcfdd8ca4b0 | 212e985bf728ed5b2add8443 |
| aarch64 | 14349e3834b1d108e25f3681 | 3b022a00efd4b9b6c2734562 |
| riscv64 | 0899c005f9cecf0f9680245f | 9f0b435e75ddd20f3e97ed84 |

三行逐筆寫入新reviewed_function_sha256、supersedes、reviewer/date、適用前提與本報告cross_check_record；其餘99行原文字保留（git diff顯示3新增/3刪除）。另外三條stale IDs `7c7aa6cda2a7ee2b20944a3f`、`81ccb02baa7772dd72d6b162`、`fd5f1a46a727d2fe3e611f78` 不在這次授權範圍，未自動刪除或接受。

更新前baseline SHA-256 `c46e64aa0bcfabde391f2f314ee2a9317f7571cf05c49ccc9470a3ec12b00bb3`；更新後 `89e959af0675c70e68ab1515ae0b572c3cfc9a78e83e0d1281f162c61e33a3b2`。前份完整副本與映射留在稽核目錄baseline-before.txt及baseline-renewal.json。

## 檢查結果與不可改寫的歷史

1. 更新前跑 `bash scripts/static-check.sh --compare-ref HEAD:tests/static/findings-fast.json --gate delta --changes-only`：三個新hard恰為fork_table各架構遞迴；新style94、existing-unreviewed832，exit1。原始完整findings在 `/tmp/welyos-static-fgv6yn18`。
2. 更新後跑 `bash scripts/static-gate.sh`：exit0，顯示new hard0、style94、existing832。
3. 再保存更新後完整掃描：`bash scripts/static-check.sh --compare-ref HEAD:tests/static/findings-fast.json --gate delta --changes-only --out /tmp/welyos-c3-fork-static-7gu3go_u/after`。三個新ID accepted=true、new hard0；raw checker仍exit1，因它也對94條未審style報失敗。這與regress wrapper只阻擋hard的exit0不同，不能宣稱全部靜態提示清零。

更新後findings.json完整SHA-256 `18131717f72dd3a83c885b2f1f75b13962b6784290673d502a4103afa785680a`；完整console留after-scan.log，函式差異留fork_table.diff。rules.py SHA-256 `367af68b440b0e1ba01d4b098bdc2a02355fcde1ff35584e48917412f42f3e9f`，規範README SHA-256 `560441901d049af2dd036ef058377f79881baec01daf8dc61e92cb251fb9fc71`。

固定包原regress-c3.txt尾「有不符合預期的結果」必須保留；本次新gate通過只消除三個待審遞迴提示，沒有重跑整份regress，也沒有把舊失敗log覆寫成成功。root若要宣布整合驗收通過，仍需自己整合其他agent的exec/IMAGE故障與固定boot驗證證據，或由owner跑新回歸保存新log。
