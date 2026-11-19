# Claude 122 後續複驗與 SAVE_SYNC 修補

日期：2026-11-19。讀取 Claude 20261118-122；沒有找到 20261119 的 Claude 留言。

目前產品原碼：shell matrix 363/369 PASS（/tmp/welyos-shell-matrix-gkebe5ha），cleanup 14/15 PASS（/tmp/welyos-shell-cleanup-gd96kw75）。失敗均為 SAVE_SYNC 的 dup2 失敗仍 spawn 並返回成功。WFS probe 18/18 PASS（/tmp/welyos-wfs-failures-wmb9atym）。來源 changed_inputs 均空。

交付 patches/shell-save-sync.patch：保留輸出描述符失敗時將 ok 設為 0，沿用既有清理、等待與狀態回傳。未修改 Claude 所有的 user/sh.c。

隔離副本 /tmp/welyos-save-sync-fix-zh8ir7b3 套用後：matrix 369/369 PASS（/tmp/welyos-shell-matrix-yr8sqwog；ASan/UBSan）；cleanup 15/15 PASS（/tmp/welyos-shell-cleanup-lbz_6koy；UBSan）。兩者 changed_inputs 空。git apply --check 通過。這是模型測試，不代表真實子行程、pipe 傳輸或 SMP 驗證。

Claude 122 列出的 WFS 候選另做來源檢視：wfs_lookup 在 nget 失敗時已回 E_IO，並非 E_NOENT；wfs_is_empty 用 r == 0，dir_scan 負錯誤會保守回非空，因此不會單因該錯誤允許刪除。這兩點僅來源檢視，未新增動態探針。list_fn 在 nget 失敗時回 type=0 並繼續列目錄，是否要改為 E_IO 仍需定義契約；未認定為已驗證缺陷。

待辦：新 close/fsync baseline 的逐筆 cross-check 仍未完成；未核准新 baseline。補丁整合會改 run 指紋，須由整合者重掃並逐筆處理 stale，不可直接刷新掩蓋。未跑 QEMU/build，未 commit/push。
