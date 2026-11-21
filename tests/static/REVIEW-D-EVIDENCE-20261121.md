# C3.2 (d) RAMFS 固定證據核對（2026-11-21）

核對 Claude 191 的 `build/milestones/c32-d-ramfs/`。本輪獨立重算hash與重建來源，未改產品、baseline或共享build，未編譯binary／執行QEMU。QEMU與完整regress是owner執行證據，不能寫成root或本agent本輪獨立執行。

## 來源與固定檔案

base `fbdfbe4` 完整ID為 `fbdfbe4ef74e80a60aa6ccdb3adcdfefcef3a8b9`。以 `git archive` 匯出隔離tree，對固定patch先 `git apply --check` 再套用，manifest的 **12/12來源hash全符合**（README、fs/membudget/selftest headers、main/membudget/ramfs/mem_test/user_test/vm、baseline、user/init）。完整逐檔hash在結果JSON；重建baseline只是核對固定內容，沒有編輯工作樹baseline或新增豁免。

證據：`/tmp/welyos-d-evidence-gb7wjgpx/results.json`，重建來源位於同目錄 `source/`。核對前後milestone既有檔案hash未變，`changed_milestone_inputs=[]`。

manifest完整hash：`41aae48eef6486e2693f0dee366da3bcf7eb1f11e9832746cad3d386f015ffc7`。

| 固定檔案（milestone目錄） | 完整 SHA-256 | mode |
| --- | --- | --- |
| `aarch64-welyos.bin` | `11657e304d0867aa7a68649541cfc34341fc9389c10e23810192c22daa523e5a` | 0555 |
| `aarch64-welyos.elf` | `71a7d26521f84ccfc1b741abdbf235757f86e6f808af5378d636b0cf32ab169a` | 0555 |
| `i386-welyos.elf` | `92ff58995cdd66e2d6197bddc76b798cb54376fc97147f4728b99429cc454ef0` | 0555 |
| `riscv64-welyos.elf` | `854ed60c52b6e1c1c7291fbe66590469fcad6eca76b1ed1f56824a94c17b634d` | 0555 |
| `source.patch` | `606c7f3941b381d14c002cbdfd460ed158c4e0035112a872bab7bc80600d83c9` | 0555 |
| `regress.txt` | `ce785d631c8081cab8aa53af0331164d38384080c728202b8a7a29fa42cda26e` | 0555 |

**6/6完整hash符合manifest**。四boot、patch與固定regress副本皆無write bits；manifest與三QEMU log所見mode仍0777，不宣稱整個資料夾所有證據都鎖唯讀。工作目錄`build/c32a/regress-d4.txt`也是0777，但已有上列0555固定副本，內容逐byte/hash相同。本輪沒有chmod owner檔案。

## Owner QEMU log

| log（milestone目錄） | 完整 SHA-256 |
| --- | --- |
| `qemu-i386-selftest.log` | `1365fed8289029ad358d35c640f90347b383081b6b2ba414b9b3f7fd34dd76b0` |
| `qemu-riscv64-selftest.log` | `6c0edde576cae10cee97b6f7f8f8fac172e49ae54ef2c37e4cadc83a051ce767` |
| `qemu-aarch64-selftest.log` | `58379590061d3647fbda8de516aa30973e2a8f7676878689f187c0a2fa6057c2` |

三完整hash都符合manifest前綴，且均有下列實際guest marker、無PANIC：

- RAMFS quota頁數宣告（i386 14116、RISC-V 14019、AArch64 14068）。
- exec transaction、sched/user memory reclaimed。
- signal handlers exit24。
- 非管理者RAMFS writer停止於E_NOSPC、admin可超額的測試exit71。
- guest `exit: code 0 to the emulator`。

manifest宣告這是4CPU固定副本selftest、各rc0；本輪只驗檔案與內容，未獨立啟動guest。一般selftest通過也不能代替每個RAMFS配置點故障、最後引用釋放、rename/link/truncate及管理者force-add溢位等完整驗收。

## 四輪 regress 分開記錄

| 記錄 | 完整 SHA-256 | 本輪讀到的結果 |
| --- | --- | --- |
| `build/c32a/regress-d.txt` | `baa6de52dc8c753fb622b40b84874b8beb7f3cf988e6231bab5cb51635dab66f` | FAIL；i386 8CPU第2次重複失敗，該組2/3；其餘摘要不能抵銷此失敗 |
| `build/c32a/regress-d2.txt` | `f976493aab2e08fdb716999b7b5a5dfb42e1beb72eb2596ec35781ef74bffe61` | FAIL；AArch64 4CPU第2次重複失敗，該組2/3 |
| `build/c32a/regress-d3.txt` | `8c4ad625bb05c25aa96732b545c68acbbd7d553bed527da7b492a0e4bb7efe44` | FAIL；新增13個unchecked-error提示，均在mode_sighandler對sys_kill/write/close/signal/exec等回傳的處理 |
| `build/c32a/regress-d4.txt`及固定`regress.txt` | `ce785d631c8081cab8aa53af0331164d38384080c728202b8a7a29fa42cda26e` | PASS；全文尾「全部符合預期」，靜態新增0個需處理提示 |

第四輪summary列三架構26/26、磁碟16/16、安全模式、外來磁碟、無disk/net、1/4/8CPU各重複3次、host網路1073066 checks、兩項架構檢查與exit5-class通過。本輪沒有重跑regress。

前三輪FAIL保留，第四輪PASS是後續成功記錄，不能回寫成「四輪全過」。第一、二輪summary只指出失敗情境與共享log路徑，沒有把當時實際boot／失敗log完整hash固定；本輪不靠之後可能覆寫的共享log替歷史失敗斷定原因，更不將它們自動歸為I-331或RAMFS產品錯誤。若owner有已定位原因與對應修正，須另附可追查的原始證據與回歸。

## 證據邊界

本輪獨立成立的是base+patch可重建所列來源、固定artifacts與owner成功log／regress的完整性；沒有驗證編譯輸入到binary的可重現建置，也沒有本輪獨立QEMU／RAMFS故障注入。root與其他agent的獨立執行應另列各自來源hash、runner與完整log。

12筆來源hash之外，完整patch另包含DEVLOG、ISSUES、design文件，其內容由patch完整hash固定。固定tree已包含先前exec-bind改動，不能拿較早c3-exec包的通過結果當成本包所有新行為通過。I-240整項、SMP thread生命週期、I-331、實機／航太／無人機能力不由證據包完整性自動結案。
