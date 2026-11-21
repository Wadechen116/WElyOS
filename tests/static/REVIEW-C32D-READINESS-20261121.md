# C3.2(d) RAMFS 落地前盤點（2026-11-21）

這是只讀設計／所有權盤點，沒有新增或執行測試、修改產品、QEMU/GPU、commit。現行 ramfs.c 尚未引用 membudget、建立 FS domain 或呼叫 MB_RAMFS 記帳；已有的 data_pages 只統計資料頁，不含 metadata，因此不能拿它代表完整 RAMFS 邏輯配額。

設計依據為 docs/design/C3.2-memory-accounting.md 第4節表格（資料頁、區塊表、inode、目錄項全部 FS domain；一般配置、不預留）、第5節（開機建立永久 FS domain）、第7節（實體唯一真相為 PMM，檔案共用頁不另取 process reservation）。當前 include/membudget.h 的 mb_charge_object 固定記 KOBJ，不能直接拿來當 RAMFS metadata 記帳；可用單類 mb_quota_add/sub(MB_RAMFS) 或明確的新 RAMFS helper，介面尚待 owner 決定。

## 配置／釋放所有權清單

| 物件 | 實際配置入口 | 持有者／公開時點 | 釋放／失敗路徑 | (d) 所需帳務 |
| --- | --- | --- | --- | --- |
| rnode，內含 inode | node_new:50 kzalloc；root/create 均經此處 | 成功前暫時物件；create 接到父 ents 後由 VFS nlink/refs 控制；root 永久 | create:94–95 兩配置任一失敗直接 kfree；最後 ramfs_release:303 kfree | 實際 kmalloc_size 含標頭向上取頁，MB_RAMFS；charge 失敗不得公開，且此後失敗要退票。若 node_new 先記帳，create 的裸 kfree(n) 必須同步改為配對釋放。 |
| rdirent | ramfs_create:90、ramfs_link:115 kzalloc | 父目錄 ents 串列；每個名稱獨有一個，硬連結不新建 inode | create失敗:94；ramfs_unlink:142 | 每個配置獨立 metadata charge；link失敗不加名稱、不動 nlink。unlink只退該名稱的 metadata。 |
| blocks 指標表 | grow_table:218 kzalloc(cap*sizeof(void*))，capacity倍增 | rnode獨有，指標表只複製既有資料頁指標；223替換舊表 | grow成功退舊表；ramfs_release:302退最後表 | 按 kmalloc_size 算成本，不能用有效資料頁數。需明定 grow 同時持有新舊表時的配額峰值／delta交易規則；失敗保留舊表與原成本。 |
| 資料頁 | ramfs_write:249 pmm_alloc；頁清零後放入 blocks | inode獨有，可能透過 VM_SHARED 借給多個行程 | ramfs_truncate:283逐頁 pmm_free；release先truncate(0) | 每成功配置一頁記 MB_RAMFS 1；quota失敗需歸還實際頁，不能留下blocks指標／data_pages；不得取其他人的reservation或雙記 IMAGE。 |
| FS domain 本身 | 現版沒有；設計規定開機建立 | 一個永久 root owner reference，與 writer/process無關 | 正常運行不釋放；初始化失敗政策待定 | FS初始limit、建立失敗行為、初始化先後尚需明訂；其結構是kernel控制物件，不可遞迴把自己的domain成本再當RAMFS資料頁。 |

沒有在 lookup/read/readdir/get_page/truncate 找到新 heap/page 配置。parent 指標與 rdirent.node 是借用關係，不是每次另配物件。ramfs_release 假定目錄已空，沒有遞迴釋放 ents；這依賴 VFS 非空目錄拒刪契約。

## 永久 FS domain 與開機基線

目前 main.c:320 先 vfs_init，321 initrd_unpack；vfs_init:896 建 root，接著 /dev 與裝置節點。FS domain 必須在第一個 node_new 之前可用，包含 root、目錄、裝置 rnode 及 initrd 檔案，不能只對後來的 user write 啟用。初始化新domain失敗不得以 NULL 當作免記帳 fallback。設計未指定 FS limit 如何選定／可否修改；必須先決定，否則「有記帳」不等於「有可驗配額拒絕」。

永久 FS domain 不會因 writer退出、fork或exec重置，也不從當前 thread_domain 選帳戶。它保有永久引用即可讓所有RAMFS物件安全使用同一帳戶；若另採每物件get/put策略，必須列出且測完整失敗平衡，不能把兩種策略混用。關閉一個 struct file 只釋放建立者 KOBJ，RAMFS inode/data仍由FS帳戶持有。根目錄、開機檔案永久存在，刪測試檔後應回到「測試前baseline」，不能要求整個永久FS used=0。

## Metadata、共享檔案與刪除生命週期

- VFS iput:69 與 unlink:555–562 只有 refs==0 且 nlink==0 才呼叫 ramfs_release。開著的檔案被unlink，rdirent帳務立刻退；rnode、blocks表、data帳務必須保留到最後close。hardlink刪掉其中一個名稱不能退共享 inode/data；最後名稱但仍有open/executable reference也同理。
- rename先link新名稱，再unlink舊名稱（vfs.c:698–706）；會短暫同時有兩個dirent。RAMFS quota拒絕新dirent時舊名稱不能消失；成功後總metadata應回原有量。若第二步未移除舊名稱，現有VFS容許兩名稱存在，帳務必須忠實保留兩份dirent，不可強求rename絕對原子。
- ramfs_get_page僅回整頁範圍、已配置頁；vfs_file_page要求exec_count，image_build以 VM_SHARED 對映。頁的物理owner仍是RAMFS，不因fork/exec多個映射重複計MB_RAMFS，也不對該SHARED頁取process COW票。借出不增加PMM refs，其安全依賴exe file引用與exec_count阻寫/截斷。image_discard先destroy空間，再exec_release/close；(d)不能把退FS帳務提前到普通file close或VM unmap。
- truncate釋放資料頁並清空保留尾頁；目前不縮blocks指標表。truncate(0)後仍有metadata表成本是既有設計，不應誤報洩漏；只有release退表和rnode。最後一頁清零不改頁數或配額。

## 仍 open 的具體可驗條件（本輪未測）

1. 分別拒絕rnode、dirent、blocks新表、每個資料頁的heap/PMM或quota步驟：未公開物件、quota不漏、錯誤前舊資料及鏈結不受損。成功配置後quota拒絕的逆序釋放也必須覆蓋。
2. 指標表grow應覆蓋0→4、4→8及最大4096格、32/64位指標尺寸與kmalloc含標頭成本；grow成功但第一個data頁失敗時，現版保留新表，需明定允許此持久metadata變更或要求回復，不可假稱失敗全不變。
3. partial write應只計實際裝入blocks的新頁：跨多頁在第k頁失敗回done，size與file offset跟隨實際寫入；已有頁覆寫不應再charge。零長度／E_FBIG不應取metadata或quota，sparse hole不配置data頁。
4. 靜態候選需特別驗：ramfs_write:260在done==0時仍以off+done更新size。當off>原size、表grow成功、第一頁配置失敗，可回-E_NOMEM但檔案size變成off。這是可由路徑閱讀推出的待重現現象，不是本輪已跑出的反例；(d)應明訂零進度失敗是否允許改size，而不能把它混進「只有partial成功會改size」的驗收假設。
5. delete/open/hardlink/rename/executable跨fork：逐步檢查nlink/refs、FS used、writer domain與file KOBJ分開平衡。unlink後繼續讀寫、最後close才退inode/data；執行中unlink不釋放已借出的頁，最後exe釋放才完成。
6. 多inode並行growth、quota臨界與拒絕、PMM floor+reserved飽和時RAMFS一般配置不能動用承諾票；應區分quota拒絕與physical allocation拒絕。永久FS domain used的變動必須由MB鎖保護，不可只靠各inode鎖或data_pages原子計數。
7. 鎖／API：VFS通常已持inode mutex(MX_INODE=50)，RAMFS呼叫mb(LK_MB=75)、heap(80)、PMM(90)應分段完成，不能在heap/PMM鎖內反取MB。inode release由VFS鎖外呼叫，仍需遵守單一owner解除的生命週期；不要假設每個RAMFS callback都一定在相同鎖狀態。
8. physical與logical分開：data_pages應精確回baseline；heap kfree不向PMM退頁，故metadata配額歸還不代表pmm_free_count等量回升。必須分別驗FS quota balance、PMM票不變、資料頁釋放，不拿整體free數替代三者。

## 來源綁定

| 檔案 | SHA-256 |
| --- | --- |
| kernel/ramfs.c | 335fa9a5078b19c75eba6b619a90a3a559f646a2c3e7c98c427c981c9817517a |
| docs/design/C3.2-memory-accounting.md | 7856f463749ff22b462326721966b06060eeb519c7ce6ca7333a2696c723d24e |
| include/membudget.h | 11af72d1ef7e7fa5604e8bcbc219d154d1088ae225850d22562ba182da708099 |
| kernel/vfs.c | f34897f8af4065ad09395aa441dc142fa096fb9dbe1332c143093914497a4c44 |
| kernel/proc.c | 5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e |
| kernel/main.c | a4eab41d0841de08bb409286fea5646c94f03ffae153537e53b64e741997f252 |
| kernel/initrd.c | 5c5bc59994c5fffb6ade5fdfd2ca0266fad5fd6c7c59b46e10cba4db498df385 |
