# c2+c3 驗收紀錄

## 綁定修正版複驗：原阻擋已解除

`c32-c3-bind` 固定包以base fbdfbe4加source.patch重建，5/5來源hash符合；
四boot、patch、三owner log及完整regress-c3b hash符合，regress為PASS。
root從隔離唯讀boot副本獨立執行三架構selftest，全部rc0、必要markers齊全、
無PANIC、boot未變。完整argv/env與log hash在
`/mnt/c/Users/user/AppData/Local/Temp/codex-welyos-bind-yqdho1_r/independent-results.json`。
沙箱WSL啟動失敗未計客體測試，之後經批准沙箱外完成。

真函式host複驗包含105項生命週期控制與兩個突變：第二space、不同domain、
NULL owner、ended token拒絕且無副作用；heap/get/quota/root配置失敗後解除
綁定並可重試；非owner解除忽略；100次pthread雙create恰一成功。全部符合，
UBSan無診斷。證據 `/tmp/welyos-c3-bind-host-oa97w_v6/`；工具
`tests/static/probe_c3_bind_retest.py`。成功建立後bound保留一次性tombstone，
直到下一次begin重設，是本次明確生命週期限制，不列新阻擋。

因此接受本次單一new_vs綁定修正，解除下列舊版阻擋；不代表全OS、航太可靠性
或尚未驗過的所有SMP/failure組合均完成。RAMFS記帳與I-331等既有待辦仍保留。
詳見REVIEW-C3-BIND-RETEST-20261121.md及REVIEW-C3-BIND-EVIDENCE-20261121.md。

## 原c3版本驗收：待修正（歷史紀錄）

已完成固定包驗收工作，但尚不接受整體(c3)結案。

- base512e496與source.patch重建14個來源hash符合；四boot、patch及六份owner log核對符合。
- root獨立三架構selftest與memhog=fork六項均rc0，markers齊全、無PANIC、boot未變。
- fork_table遞迴結構未變，三架構最多三層；僅更新三筆baseline，static gate通過。
- 原regress-c3 FAIL紀錄保留；baseline變更後仍需owner完整回歸，不將舊結果改稱全綠。

阻擋：exec交易未實作既定單一new_vs綁定。真函式host模型中limit100、pending80，
同一active txn被兩個space接受，各借IMAGE配額30，total142。現sys_exec只建一個
newspace且清除指標，未證實現有user可濫用；但API契約及防止跨空間信用重用的
驗收條件未滿足。已透過memory223交Claude處理。

修正應拒絕第二space重綁並維持失敗無副作用，涵蓋不同domain、已結束憑證、
首次建立失敗清理及成功後清理。修正後需綁新來源與boot證據複驗。

證據與細節：REVIEW-C3-EVIDENCE-20261121.md、REVIEW-C3-QEMU-INDEPENDENT-20261121.md、
REVIEW-C3-FORK-STATIC-20261121.md、REVIEW-C3-EXEC-INDEPENDENT-20261121.md。
host反例在 `/tmp/welyos-c3-bound-host-91ovfhi6/`。
