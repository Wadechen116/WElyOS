# RAMFS (d) 固定來源獨立審查

**指定的RAMFS記帳／失敗回退範圍，未發現新的產品阻擋。** 真函式host測試與
三個負對照通過；不將這個範圍延伸為整個OS工作全部驗收完成。

## 固定來源

以git show fbdfbe4重建基準，在/tmp套用
`build/milestones/c32-d-ramfs/source.patch`（--fuzz=0）；manifest12來源
**12/12完整hash符合**。
證據 `/tmp/welyos-d-ramfs-independent-197hhni8/verified.json`。
Patch SHA-256：`606c7f3941b381d14c002cbdfd460ed158c4e0035112a872bab7bc80600d83c9`。

| File | SHA-256 |
| --- | --- |
| kernel/ramfs.c | fd75bc92bb4772abc2f1109135d9a121caf4eb39cd14bf98d95d20dee235a7db |
| kernel/membudget.c | 978491954d07478d3ed20e0e8c095a0fee5c8490ae1be0c930c8ac56d92a03b3 |
| include/fs.h | e1a3f10eacf4ddda15a86d02bf1388bdc870ee5991e2c39f126dc4331a7c89a7 |

未以正在修改的工作樹替代固定來源；baseline僅讀取核hash、不修改。

## 真函式 host 結果

新增 `python3 tests/static/probe_d_ramfs_independent.py`：每次獨立重建固定
來源並核hash，編譯**完整真RAMFS module及全部membudget函式**，沒有改寫
grow/write/truncate等產品函式。兩種heap實際大小取整成本均通過：fixture
回報原配置大小及多一頁大小，確保charge/release不是碰巧固定算一頁。

| 範圍 | 已動態檢查 |
| --- | --- |
| create metadata | dirent heap拒絕→ENOMEM；dirent quota拒絕→ENOSPC；node heap拒絕、node quota拒絕→ENOSPC，directory size/鏈/charge/heap皆回原值 |
| link/unlink | quota拒絕link不改原目錄；成功link新增dirent成本，unlink只退dirent；仍持inode時資料保留，最後顯式release才退node/table/data |
| grow_table | 新表heap或quota拒絕時pointer/cap/舊data/總量不變；成功保留原page pointers並退舊table成本，總量只留新表成本 |
| 零進度物理失敗 | 空檔off8192寫17bytes、第一資料頁PMM拒絕→ENOMEM、size0、data_pages0，已成功建立的table保留且有正確metadata charge |
| partial quota write | 額度只容一頁，寫3頁回4096、size4096、data page1；下一頁回ENOSPC，不把EOF增到要求大小 |
| partial PMM write | 第二資料頁配置拒絕，退失敗頁暫charge，回4096、size4096，成功頁保留 |
| overwrite | 已有資料頁在limit被調到0後仍能覆寫，無正增量也無新增charge |
| admin / kernel bypass | CAP_ADMIN和無cred kernel兩條路徑都可越quota寫頁，但MB_RAMFS仍逐頁增加；普通statfs room在超額時clamp0 |
| truncate / sparse | 縮小釋放完整資料頁並退charge、保留頁尾清零；全truncate保留table成本；稀疏洞讀回0 |
| release | unlink最後名稱但caller尚持inode時資料不提前釋放；顯式release後資料/table/node退清，root最後release後domain total0、heap/data_pages0 |

三個故意破壞的負對照：漏PMM失敗的資料charge退回→exit10；換表漏退舊表
charge→exit19；unlink漏dirent退帳→exit9。baseline編譯0/exit0，UBSan無
診斷；每輪最後heap/physical/data_pages/accounting_errors全零。

證據 `/tmp/welyos-d-ramfs-host-ccx8txe6/results.json`，同目錄保留固定來源、
四份C與四個執行檔。工具可重跑並產生另一個獨立/tmp目錄。

## 路徑審查與易混淆契約

- 固定ramfs_create的node_new NULL分支**已回ENOSPC**，不是ENOMEM。node_new
  本身只有pointer/NULL介面；create把node配置或quota拒絕統一映射ENOSPC。
  最早dirent heap配置失敗仍是ENOMEM，與quota拒絕區分。root_create也只有
  pointer/NULL，沒有可要求對外errno一致的介面。
- kzalloc後才以真kmalloc_size算metadata成本，是先建立尚未發布的私有物件，
  成功記帳才接入inode/目錄/區塊表；charge拒絕即kfree。這不是漏記帳，也
  不能因文案「先記帳再配置」就把為實際大小取整需要的順序誤報為超額漏洞。
- 換表是**新表全部先charge，再退舊表**，所以需要短暫雙表quota；即使最終
  新表成本可放下，也可能在換表peak被拒。這是保守的既有實作契約，成功
  後沒有把兩張表都留在帳上。grow_table拒絕沒有替換/破壞舊表。
- write只對資料部分提供partial progress，不是整筆失敗零配置：grow_table
  成功後資料頁拒絕會留下已記帳的table，直到release才退；size/data_pages
  不因此假增加。測試對這一點採明確oracle，不把table保留誤判為漏charge。
- MB_RAMFS由永久FS domain持有，不記寫入者domain、不需每個inode另拿reference。
  kernel/main在vfs_init/root/archive建立前設定一次fs_dom，保留初始reference；
  沒有觀察到正常執行期切換domain。不能把setter在已有物件後改domain當成
  現有產品呼叫路徑。
- mb_quota_force_add只跳過limit，不跳過total溢位檢查；RAMFS資料仍使用普通
  pmm_alloc，admin不能由這條路徑吃掉別人的預留或底線。host PMM是fixture，
  這裡的實際N1/N2保證依真PMM既有驗收，不冒稱本工具另證明。
- VFS基準fbdfbe4的vfs_rename先link新名稱、成功後才unlink舊名稱，所以新增
  quota拒絕link不會先刪來源。iput在refs/nlink都0才呼叫release。這是靜態
  複核，host直接控制inode生命週期，沒有抽整個VFS的refs/rename鎖當動態測試。

## 限制

fixture替代heap/PMM/cred與鎖，inode保留RAMFS用到的欄位，並使用真inode_ops
宣告；物件布局不是三架構原生VFS struct inode布局，兩種取整成本不等於
實際各架構ABI驗收。測試未實作VFS refs/links鎖層、rename並行、open/dup/exec
hold或真正物理飽和；inode最後reference的判定仍由VFS既有契約負責。

本輪未執行QEMU/GPU，未改產品/baseline/共用probe，未commit/push。root與
其他agent的固定boot、script與regress驗收另計；此處不將它們轉述為我方重跑。
