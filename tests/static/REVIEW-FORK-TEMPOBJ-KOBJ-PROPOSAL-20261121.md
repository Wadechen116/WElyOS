# fork_start / frame KOBJ 獨立子批提案

供 Claude owner 審閱，未套用共享產品，也未堆疊 ustart patch：

- `tests/static/proposals/fork-tempobj-kobj-20261121.patch`
- `tests/static/probe_fork_tempobj_proposal.py`

隔離實作：`/tmp/welyos-fork-tempobj-proposal-7mf1wl94/kernel/proc.c`。
patch 僅改 proc.c，基於現版未提交來源，HEAD 4c04c3b；基準 proc.c SHA-256
`5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e`。
Patch SHA-256：`08a106d9ca2f681619880acfd96c6c48b0fab8b311722a14204b99aa4690c0ec`。

## 所有權與最小改動

fork_start 是 frame 的容器，兩者一起交給 child consumer 或一起回退。
fork_start 改用 kzalloc，提早記錄 frame，因此從第一個配置失敗開始都可以
統一清理；st為NULL時僅釋放獨立frame。dom NULL 表示尚未成功記帳。

pmem_fork_domain 成功後，兩個物件各自用真 kmalloc_size（含標頭）向上取整，
**相加後一次** mb_charge_object；這保守地記上兩物件的全部配額，容器為整組
持有一份自有domain reference。選擇同一組生命週期，不是每個allocation各
拿一份reference；因此無「st charge成功而frame charge失敗」中間狀態。

charge 在 thread_prepare 與 pmem_fork_space 之前。quota/ref 拒絕、thread
prepare失敗、child VM建立失敗都維持原有父PTE不變契約。nomem先釋放c，
但清理helper只用st自有dom，不再讀c或c->mem.dom，reference仍保障domain。
pmem_release退proc參考與charge，fork_start_release退最後的暫時物件參考。

成功時consumer先把frame複製到自己的kernel stack，再呼叫fork_start_release
一起釋放frame/st與charge/reference；此步在dying/killed分支前，不論是否
真正返回user mode都已完成。沒有在fork_space成功後新增會失敗的配置。

本patch不修改exec pending、STACK/IMAGE承諾，也不借用pending作KOBJ額度。
ustart patch與此patch修改不相同區段，理論上可順序套用，但本工具只驗單獨
fork patch；兩patch整合後仍須owner重新build並檢查預期配額邊界。

## Host 驗證

執行 `python3 tests/static/probe_fork_tempobj_proposal.py`，工具另複製產品來源
至/tmp，以patch --fuzz=0套用，再從複本抽真sys_fork/fork_child_main/
fork_start_release、split pmem_fork helpers、VM fork/create/destroy以及
membudget計數/charge/release函式。沿用現版fork caller探針的PMM/PTE fixture。

112控制通過：2種實際kmalloc成本（每物件1頁／2頁）× 父resident1~4頁 ×
14種路徑（11失敗＋3正常/dying/killed）。失敗包含六個heap配置位置、
thread_prepare拒絕、暫時KOBJ quota不足、domain reference飽和、child根表
quota不足與root實體配置失敗。失敗父PTE/資料票/全域reserved不變，無pid/
thread發布、無child allocation或帳務殘留。正常/dying/killed在consumer
結束後KOBJ僅留proc成本，暫時charge/reference已退，後續完整destroy歸零。

四突變皆被指定出口抓到：漏退charge→10；漏free frame→10；漏free st→10；
漏frame成本→90（fixture明確panic成本/ref不符）。baseline exit0、UBSan
無診斷、changed_inputs=[]。

證據：`/tmp/welyos-fork-tempobj-probe-o4ozkuh6/results.json`，同目錄保留套用
後proc.c、五份C與執行檔，以及以下依賴來源hash：

| File | SHA-256 |
| --- | --- |
| kernel/proc_mem.c | f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a |
| kernel/vm.c | bf6531b8960779f85538a552eed96fef540ced65cfe01c9d9045ae1915643b8d |
| kernel/membudget.c | 4a7fc43c3c4f4275b9def828801df8ed9e07aa471ab2c25343b1d6f251ca7c50 |
| kernel/proc_internal.h | b41a25b8e6ee39ea4e4b41063866289d81b2bf98a2a5d41b525a1f2a46b4ee1b |

限制：fixture的thread本身未接真thread KOBJ；heap/PMM/table walk/PTE codec/
locks/TLB/scheduler均簡化，domain ref飽和由wrapper暫設並在返回後恢復以清理。
root fail是真create函式的table_alloc邊界注入，不涵蓋全部中間表fail點。
未做產品build/QEMU/SMP/GPU，未改共享產品或共用探針，未commit/push。
owner套用後須原生三架構編譯、相同故障點與immutable milestone QEMU驗收，
不能用此host fixture取代最終產品驗收。
