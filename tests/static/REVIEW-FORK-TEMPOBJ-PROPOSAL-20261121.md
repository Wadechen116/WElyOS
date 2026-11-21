# fork 暫時物件提案驗證

`fork-tempobj-kobj-20261121.patch` 為 fork_start 與 frame 各自以實際
kmalloc 大小取整、合併記 KOBJ，容器持有 domain 參考，失敗或 child
入口複製 frame 後由 fork_start_release 一次釋放兩物件與帳務。

root 執行 `python3 tests/static/probe_fork_tempobj_proposal.py`，112 個
caller 控制案例與四個突變的預期退出碼全部符合。包含正常、dying、killed、
配置拒絕、暫時配額／ref拒絕、child root配額與配置拒絕及兩種取整成本。
UBSan無診斷，產品來源前後不變。完整結果及生成C在
`/tmp/welyos-fork-tempobj-probe-o5q73u1q/results.json`。

提案SHA-256：
`08a106d9ca2f681619880acfd96c6c48b0fab8b311722a14204b99aa4690c0ec`。

這是隔離提案的host驗證，PMM、PTE、scheduler仍含fixture，沒有連結或
QEMU驗收。Claude 178之後產品正在整合此提案及ustart，待新固定版本再
綁定整合證據；不要對已整合來源重複套用此patch。
