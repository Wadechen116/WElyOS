# C3.2 exec-bind 修正固定證據核對（2026-11-21）

對 `build/milestones/c32-c3-bind/`（Claude 187）獨立核對 hash 與來源重建。沒有修改產品、baseline、共享 build，沒有編譯 binary 或執行 QEMU。三份 QEMU 與完整 regress 是 owner 證據；本輪只核對其完整性和可見結果，不能寫成 root 本輪獨立執行。

## 來源重建

base `fbdfbe4` 解析為 `fbdfbe4ef74e80a60aa6ccdb3adcdfefcef3a8b9`。以 `git archive` 匯出至隔離 `/tmp`，對固定 `source.patch` 執行 `git apply --check` 後套用，manifest 所列 **5/5** 來源 SHA-256 全符合：

| 重建檔案 | SHA-256 |
| --- | --- |
| `include/membudget.h` | `76660c3ef7840f00468d61e28cbb9832ea26fa551428a47678b40785e35d0bbf` |
| `kernel/membudget.c` | `c7278428cf2e5718cfb7884ea4d21d35e6a92496238d246f93e8f7ea88736c87` |
| `kernel/test/mem_test.c` | `6f68df1f29c34875bb845b504fcd1fe930990bc26ec9857e1d8e003dd033bb86` |
| `kernel/vm.c` | `6f8f8639f6e87754e9b2f575f658f9c043c89d5f382b07421156f37e80ba0701` |
| `tests/static/baseline.txt` | `89e959af0675c70e68ab1515ae0b572c3cfc9a78e83e0d1281f162c61e33a3b2` |

完整 patch 也包含 DEVLOG，沒有將5筆來源表說成完整建置輸入證明。重建副本含 baseline 是驗證既有包內容，沒有編輯工作樹 baseline 或新增豁免。當前工作樹可能已繼續其他產品工作，本輪不以它取代 base+patch 的固定來源。

證據：`/tmp/welyos-c3-bind-evidence-oibkqwd9/results.json`；重建 tree 在同目錄 `source/`。核對前後 milestone既有檔 hash 未變，`changed_milestone_inputs=[]`。

## 固定 artifacts 與唯讀狀態

manifest 完整 hash：`031c62812ba7b1d5391a22753caeb9b3c3dc5a56348485a64c0f45a70f9449b3`。

| 檔案（milestone目錄） | SHA-256 | mode |
| --- | --- | --- |
| `aarch64-welyos.bin` | `0428be473bffea76a68751ae90a10a588bfa5555c5db158b6d1ef42c77601438` | 0555 |
| `aarch64-welyos.elf` | `f716ef0d546cfcbc35ed197e32e9e4b4f0ef389fc6078b61dabbb2cba19a1895` | 0555 |
| `i386-welyos.elf` | `a98cc702c43fb35b5e5340143c62eb705dfe2acc7b71054aaaef2dea80953a7f` | 0555 |
| `riscv64-welyos.elf` | `7df3c282f0e23beba16a3211b9c711e1d2bc439343e2432004de37f43c3d52cb` | 0555 |
| `source.patch` | `f004804c9cc44bdcdacf5e3375499403c633de45adbd00fad081d550dff2f14e` | 0555 |

以上 **5/5** 完整 hash 符合 manifest，且無write bits。AArch64真正boot輸入應核對.bin。manifest本身、三個log及工作目錄的regress檔本輪所見mode為0777，**不是唯讀**；只能說本次hash符合且核對期間未變，不可宣稱整個驗收包每個檔案皆已鎖唯讀。本輪沒有自行chmod或改寫owner檔案。

## Owner QEMU log 與 marker

| log（milestone目錄） | 完整 SHA-256 |
| --- | --- |
| `qemu-i386-selftest.log` | `c73d194e787170f6d9c7e87439f987de657851af20271f19aa673555e65c3d20` |
| `qemu-riscv64-selftest.log` | `af0ff89d3a136693e27317db524b6a935e90eb77157a37255c879a19fdcf7d23` |
| `qemu-aarch64-selftest.log` | `f2d3d66554afe5acfcbb573a133435944dcc9da331603d146632ad2885574614` |

三個完整 hash 均符合 manifest前綴；三log皆含 exec transaction marker、sched/user memory reclaimed、user quota/mmap exit27、guest `exit: code 0 to the emulator`，無 PANIC。manifest記載由固定副本跑4CPU selftest、rc0；本輪沒有重新執行。

重建的 `exec_txn_test` 內確有第二空間、異domain與已結束txn拒絕斷言。既有exec成功marker位於這些斷言後方；這是對owner測試路徑與log的閱讀支持，不是獨立故障注入、不證明所有bind/unbind失敗點或生命周期均完成驗收。

## 完整 regress

`build/c32a/regress-c3b.txt` 完整 hash：
`dbbb03084eca18c9ad2da2058a554770184577985ef4c045f6984ef0bf4e7b5e`。

符合manifest前綴，全文尾為「全部符合預期」，靜態新增0個需處理提示。summary列三架構26/26、磁碟16/16、安全模式、外來磁碟、無disk/net及1/4/8CPU重複測試通過，host網路與兩項架構檢查、exit5-class亦通過。這是owner新完整成功記錄，**不覆蓋**舊`regress-c3.txt`的三提示FAIL，也不解釋I-331曾有的排程失敗。

## 限制與後續

本輪獨立完成 base+patch來源重建與證據hash核對，尚未重新建置binary以驗證來源到artifact對應；root的獨立QEMU及host bind契約probe需另列自己的證據。baseline內容可重建不等於本agent重新接受所有基準理由。

一張txn只綁一個new_vs的產品修正是否完整，仍要由獨立真函式測試覆蓋重複／異domain／expired、構造中失敗解除綁定、錯owner不能解除、成功後結束與重用等契約。RAMFS與其他C3.2缺口不由此包自動結案。
