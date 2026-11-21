# RAMFS 零進度寫入錯誤改變 EOF：已重現（2026-11-21）

`python3 tests/static/probe_ramfs_write_current.py` 已抽取未修改的真 `ramfs_read`、`grow_table`、`ramfs_write`，確認先前候選存在：第一個資料頁配置失敗、寫入位置超過舊 EOF 時，回 `-E_NOMEM` 且零個位元組寫入，仍把檔案大小增到寫入位置。這不是 partial success。

## 實測範圍與控制

| 情境 | 舊size | off / requested | 配置故障 | 回傳 | 新size | data pages |
| --- | --- | --- | --- | --- | --- | --- |
| 正常寫入與讀回 | 0 | 17 / 5000 | 無 | 5000 | 5017 | 2 |
| off < size，零進度錯誤 | 8192 | 4096 / 20 | 首data頁 | -12 | 8192 | 0 |
| off == size，零進度錯誤 | 8192 | 8192 / 20 | 首data頁 | -12 | 8192 | 0 |
| off > size，零進度錯誤（反例） | 8192 | 12288 / 20 | 首data頁 | -12 | **12288** | 0 |
| 跨頁部分成功 | 0 | 4090 / 20 | 第2個data頁 | **6** | 4096 | 1 |

正常控制檢查5000 bytes讀回一致，前17 bytes清零；partial控制確認只有第一頁的6 bytes成功，第二頁仍空。每個案例cleanup後fixture heap/data pages均歸零。三個首頁失敗控制都允許grow_table成功後保留4格metadata表，不把此既有行為誤當data leak。

在生成C的比較版，僅把EOF更新條件加 `done &&`（不改產品），五個情境重跑：off>size的零進度錯誤維持8192；正常與partial結果完全相同。這個比較支持問題出在 `ramfs.c:259–260` 沒有排除done==0，不代表已提交修正。

## 既有契約是否容許

現行 `ramfs.c:251` 註解寫「記憶體用完：回報已經寫入的部分」，261行以done>0回正數，done==0回錯誤。`vfs.c:447–450` 也只在正回傳增加file offset，負錯誤不前進。`vfs_seek` 允許移到EOF之後，因此反例offset可由既有操作表達；不是只能破壞內部結構才會出現的狀態。fixture的size8192且尚無資料頁是合法sparse表達，但本輪未經syscall建立該狀態。

在本次查閱的 `include/fs.h`、RAMFS實作與C3.2設計中，未找到明文允許「零bytes寫入失敗可擴大可見EOF」的例外，也未找到覆蓋RAMFS所有錯誤的強交易承諾。因此精確結論是：**真碼已重現不合理的可見EOF擴大副作用，應請owner明確修正／定義語意，不能以合法partial成功解釋它**；不是宣稱已有明文「所有失敗任何狀態全不變」契約。VFS本來可在寫入前改mtime，grow_table也可保留metadata，這兩者不在本次EOF缺陷主張裡。

## 證據與限制

- 證據 `/tmp/welyos-ramfs-write-current-2vz2o_h9/results.json`、兩個生成C與執行檔。
- 原碼與guarded比較均exit0、UBSan無診斷、編譯stderr空，changed_input=false。**工具exit0代表反例與控制如期重現，不是產品驗收通過。**
- 來源 `kernel/ramfs.c` SHA-256 `335fa9a5078b19c75eba6b619a90a3a559f646a2c3e7c98c427c981c9817517a`。
- 工具 SHA-256 `5bc84b4bc672086ec3cdc4ee64d90763ec707c7cdc64befe2817b051cec44760`。
- results.json SHA-256 `dce0d30dbb6b1d9c64d4049e19975571ca163de259ec7f5e2a2f7ee6abcc0481`。
- heap/PMM、inode與rnode的必要欄位為host fixture；沒有測真PMM耗盡、quota、VFS wrapper/syscall、並行、QEMU或arch。未擴充到其他RAMFS情境，未改產品／既有probe／commit。

## 未套用的修正提案

最小正式 unified diff：`tests/static/proposals/ramfs-zero-write-eof-20261121.patch`，只把size推進條件改成 `done > 0 && off + done > in->size`。正數partial仍按實際完成bytes推EOF，零進度負錯誤不改EOF。`git apply --check`對目前產品成功；沒有套用，共享 `kernel/ramfs.c` 仍含已重現反例。host guarded比較使用等價的 `done &&`，不是對產品已修好的聲明。
