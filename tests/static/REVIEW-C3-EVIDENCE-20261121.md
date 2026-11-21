# C3.2 (c2)+(c3) 固定證據核對（2026-11-21）

本輪獨立重算證據 hash，並從指定 base 加 patch 重建來源；沒有修改產品、baseline、共享 build，也沒有建置 kernel 或執行 QEMU。既有 QEMU／regress 為 owner 提供的執行證據，不能寫成本輪獨立執行通過。

## 結果

`build/milestones/c32-c3-exec/manifest.txt` 的 SHA-256：
`c27238290b6a8022f1fa0329a68a63010dea15a1bb50e798a79b3b9e6d44d588`。

- 指定 base `512e496` 解析為 `512e49661e2f8a955537c28c98ce463555a2c984`。以 `git archive` 匯出到隔離 `/tmp`，`git apply --check` 後套用完整 `source.patch`，manifest 所列 **14/14** 來源 hash 全符合。
- 四個 boot artifacts 加 `source.patch` **5/5** 完整 SHA-256 全符合，檔案 mode 均0555（沒有write bits）。
- 六個 owner QEMU log 重新計算完整 SHA-256，**6/6** 符合 manifest 的16字元前綴；六份均有真正 guest exit0、無 PANIC。
- 完整 regress hash 符合；其結果仍是 **「有不符合預期的結果」**，不是 PASS。
- 核對前後 milestone 既有檔案 hash 未變：`changed_milestone_inputs=[]`。

本輪可重查證據：`/tmp/welyos-c3-evidence-1py1cm32/results.json`，含14來源逐檔expected/actual、artifacts與logs完整hash；重建來源位於同目錄 `source/`。patch包含17個檔案，其中DEVLOG、ISSUES與design三份文件不在14來源hash表內；它們仍由完整patch hash固定，不宣稱manifest逐列涵蓋所有原始碼或建置輸入。

## 固定 boot 與 patch 完整 hash

| 檔案（皆位於 milestone 目錄） | SHA-256 |
| --- | --- |
| `aarch64-welyos.bin` | `1d669ebdee66e1e206d7cf7199b8b4af31f77626557f1aee808ede652ae09d23` |
| `aarch64-welyos.elf` | `fcfe4d7090dfa763103c2f2c73100bf2700558fd948233ed2ec7be7b53aacd3d` |
| `i386-welyos.elf` | `3d8203237a6d6431bc39d599b596c9fede42cc3306083c0052d13adefa322517` |
| `riscv64-welyos.elf` | `6ab4fba9f1f07654869d43d08cbe89fc01a0fb5ab94ea6ec2e73af56603b067e` |
| `source.patch` | `14b5b6873b854e3d6698117c22f02d42b0f6e2f780acd3c8d609cd61515481b9` |

AArch64實際開機輸入應以.bin為準；本輪只核對檔案，未啟動guest。

## Owner QEMU 證據

| log（milestone目錄） | 完整 SHA-256 |
| --- | --- |
| `qemu-i386-selftest.log` | `51b440a2c013643009d808e7d2d906f945d2291c7dd86bfc97623ef62c9f95b7` |
| `qemu-i386-memhog=fork.log` | `eb61d49a2b6dda8f9f51f0ad77d2003ae7449593e9344af0c75c7465bf13b730` |
| `qemu-riscv64-selftest.log` | `6098c35dad8f74e172fd2fd7b3c7215e86d344d7375cd0d1d05aa09763bd5cae` |
| `qemu-riscv64-memhog=fork.log` | `557a5e95c68b8ef5fbd629ce9c20802afe775ccf594e155b9e652267d6a698f2` |
| `qemu-aarch64-selftest.log` | `2a9fbdadd826f78b751bf3a8bf75f8f5a421f84970e0677a5e869af5573abcbb` |
| `qemu-aarch64-memhog=fork.log` | `1df0935ae9e27bbaefb91119742cbb49b6d267a7debe2604c519606b8a2490f3` |

三架構selftest log都含255頁stack承諾首次觸及、8頁IMAGE與fork寫入、exec以舊空間可歸還量放寬交易、user memory reclaimed等marker；這支持owner實際跑到相應子測試，不代表各錯誤點已完整注入。

三份memhog=fork均exit0、各1次拒絕：i386最低30891頁／floor4034；RISC-V最低30602／floor4006；AArch64最低30708／floor4020。log也明列free前後差由heap/page-table/stack cache保留量增加對應，不能寫成所有物理頁都回到初始值。最低值遠高於floor，故不能用這三條摘要單獨聲稱fork已把實體記憶體壓到物理底線；特定reservation邊界的驗收要另看確定性子測試與獨立probe。

## 完整 regress 保留 FAIL

`build/c32a/regress-c3.txt` 完整SHA-256：
`3b5d53e2aa22aeb67fd21e882b9d7b5e1295deb3b578c1765f43afe742bf45fa`。

三架構各26/26情境、磁碟16/16、安全模式、外來磁碟、無disk/net、1/4/8CPU各重複3次，summary均列符合預期；host網路1073066 checks、模組界線與編譯器防線、exit5-class亦列通過。這些是既有summary的內容，本輪沒有重跑整套。

靜態檢查新增3個需處理提示，均為 `kernel/vm.c:714 fork_table` 的direct recursion（i386／AArch64／RISC-V各1）。因此尾為「有不符合預期的結果」。本輪未改baseline、未判定三提示可直接豁免，也沒有把「只剩提示」改寫成完整regress PASS。owner需處理提示或以具體遞迴邊界／證據交審，再按專案流程登記。

## 驗收範圍

本輪獨立成立的是 **base+patch可重建所列來源、固定檔案hash與owner log完整性**；沒有重新編譯以證明binary對應來源，也沒有本輪獨立QEMU。後者由root另行驗證後附上自己的runner/hash/marker證據，不以owner log代替。

本文件不結案exec配額降低／每步回退矩陣、IMAGE每分配點／權限／fork票生命週期、完整fault injection、暫時物件帳務、RAMFS、I-331或硬體驗證；功能host probe與產品審查由其他agent分工。重建成功只驗包可重現性的一部分，不等於整體C3.2或航太／無人機部署完成。
