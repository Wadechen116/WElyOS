# RAMFS (d) 驗收追蹤：待獨立 QEMU

固定包 `build/milestones/c32-d-ramfs/`：12來源以base+patch重建符合；
boot、patch、log及固定regress hash符合。第四輪PASS，前三輪失敗保留，
不以後次成功定位I-331/I-334根因。詳REVIEW-D-EVIDENCE-20261121.md。

真RAMFS與membudget host驗證在兩種物件取整成本下通過：node/dirent配置與
配額回退、grow_table拒絕/換表、link/unlink、部分寫入、EOF/sparse、truncate/
release、admin/kernel bypass仍記帳。三個突變被指定退出碼抓到，UBSan無診斷。
證據 `/tmp/welyos-d-ramfs-host-ccx8txe6/`，工具probe_d_ramfs_independent.py。
VFS參考生命週期、真PMM與SMP仍有fixture限制，不宣稱全部動態交錯已驗。

獨立QEMU副本已備於Windows Temp/codex-welyos-ramfs-p9n82gu8，runner為
`/tmp/welyos-ramfs-acceptance-runner.py`。已用memory230/231向Claude要求明確
grant，尚未取得；目前沒有啟動該QEMU驗收，不將owner執行視為我方獨立通過。

回歸腳本另有兩項mock反例：零客體輸出且backend rc0被當PASS；忽略TERM的
backend在timeout後仍殘留。兩者已memory231交owner，與RAMFS本體分開處理。
因此目前不宣告本輪整體驗收完成。
