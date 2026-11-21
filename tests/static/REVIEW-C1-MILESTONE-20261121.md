# C3.2(c1) 固定里程碑來源／boot／證據核對（2026-11-21）

只讀核對 `build/milestones/c32-c1-stack/`，並在獨立/tmp目錄由git基底與source.patch重建來源。未編譯產品、啟動QEMU、執行host測試或commit；沒有修改共享產品。這是既有證據完整性與來源可重建性核對，不是我方獨立執行三架構驗收。

## 核對結果

- 4個實際boot檔（含AArch64.bin）與source.patch完整SHA-256全部符合manifest。
- 6個既有QEMU log完整hash的前16碼符合manifest，各有客體exit0，未見panic/assertion failed。
- regress-c1完整hash前綴符合manifest，尾行「全部符合預期」。原記錄含三架構26/26情境、磁碟16/16、安全模式5/5、1/4/8CPU各3次、net1073066、架構／靜態／exit5 checks；我方沒有重跑這些情境。
- manifest列15個來源hash，目前工作樹15/15符合；更重要的是獨立從base `872a25bbe525419aaf551619f6fee7eb790fe8d4` 的git archive解出來源，再套固定source.patch（git apply rc0，無stderr），重建後15/15亦符合。此次終於具備可重建的歷史來源，不再依賴持續變動的工作樹。
- source.patch涵蓋18個tracked檔（含docs/design等），不用目前未追蹤review/probe檔即可重建所列產品來源。此處未做可重現binary build，因此未證明各boot恰由該來源／同版toolchain產生；下一步可直接使用獨立重建來源做測試而不碰owner工作樹。

稽核證據 `/tmp/welyos-c1-source-audit-y5qx93ho/results.json`；SHA-256 `0ebb20acfc7b3613db2e024a340ff8ae124999c7c28cf31d3add4945eff54ed1`。同目錄source/保存重建結果。

## Boot／patch完整hash

| 檔案 | SHA-256 |
| --- | --- |
| aarch64-welyos.bin | 38be7f39af829a96a4930eeaf2fc7ad7e39c90a221044fe2422e915aaf546cf3 |
| aarch64-welyos.elf | 51810ce5beb5aef47d678c9af48c193a0283a1b03fa2bec20293af2eea3aa06e |
| i386-welyos.elf | 7de40cc6bd19525f219493fcaa212c70172213e6b363088e8d1a0590cfde83b4 |
| riscv64-welyos.elf | ec08c28166a086784fbb898d384517d4b2b3e13e6d671a2fd730246c468dc74d |
| source.patch | 6e4a028fc00f88b192b9435b9402a05d7c90df29ffc162b6d5bb09009d5e4a69 |

## 既有log／regress完整hash

| 檔案 | SHA-256 |
| --- | --- |
| qemu-i386-selftest.log | 9316f91f949f1a718c98751769565d8c2962ce04564da52040945cfc853769df |
| qemu-i386-memhog=fork.log | 869c963d4e78b45e7b28e33630fa11aba7df1aa02a247a091902c5d8e343bfe7 |
| qemu-riscv64-selftest.log | 2a8e17795a71fd820f8d276c3a0406912ff44df398eac93bdbe8408eb62f82ac |
| qemu-riscv64-memhog=fork.log | 37c77c9e764b0f62b0b12393920b9bd92b57e601e71b5476b6ac31700ffe1b6c |
| qemu-aarch64-selftest.log | f16fdec48a4582545484467c538ed1b230b7895fa9a00fe148eb73194fac0016 |
| qemu-aarch64-memhog=fork.log | 2d735fecc7360a9997e192918b5446c73314611acf4c03b7407988c11cfea042 |
| build/c32a/regress-c1.txt | 32a4e2b2aa79d4d7a4f4a5fa30228b603ca3803a1e1225c510f490b5cca3ec79 |
| manifest.txt | 34dbdb0d866e2dcce1ecd012b224b553cd92ddf48c67be37219549b5a4fe390a |

manifest本身仍只記log/regress短hash；本表補記現存檔的完整值。未將短碼核對描述成manifest原本已有完整hash。

## 行為證據與範圍

三份selftest log均有：一格255頁stack在一般配置到達reservation後全部觸及；40條依序建立／退出、各用512KB stack成功exit61；20次failed creations均reached後段20次、slots0x1→0x1、pages6→6；所有user process退出與memory reclaimed。這支持原owner已跑的順序churn、reservation後整格觸及、後段prepare rollback案例。

三份memhog=fork log均有明確fork error12拒絕與exit0：i386103個child，最低free30797／floor4034；RISC-V102個，30499／4006；AArch64102個，30601／4020。free數下降在log被記為heap/page tables/stack cache保留（i38631910→31712；RISC-V31626→31408；AArch6431728→31510），不是所有physical free完全回原數；不能將此記錄改述成「全部頁數無差額歸還」。這些案例可支持承諾拒絕路徑，但不是所有IMAGE COW都已預留的證據。

## 仍待驗證與下一依賴

1. **並行create/exit未驗**：40輪churn是依序建立／退出，不取代多creator與exit交錯、slot重用、ready更新與同時fault的同步驗收。owner亦明說仍缺，保留open。
2. **非slot0存活執行緒fork未驗**：主執行緒退出後，slot1執行緒成唯一thread再fork；需核對child tag沿用slot1、stack_ready/slots一致、父子各自COW／exit退各自quota與tickets，不能用一般slot0 fork代替。
3. **故障點未全串驗**：新增STACK quota+票後，真sys_fork整條鏈的根表、每張子中間表、bundle、父reserve、thread/tempobj等故障與兩/三層結構還需來源綁定的host／整合驗證；舊ANON-only probe不能直接視為c1結果。
4. **其他C3.2工作仍open**：IMAGE私有頁、image-aware fork COW、exec交易與RAMFS分類domain/配額未因本包stack測試通過而完成。不能把memhog=fork單一成功模式升級成全負載可靠性保證。
5. **來源可重建不等於binary可重建**：尚缺manifest內完整toolchain/flags、建置命令與生成輸入hash；QEMU完整命令、磁碟／固件／網路輸入hash及原始wrapper rc證據亦未完整列出。現log可見disk/net存在；未獨立重跑，不假設已隔離。regress檔仍放可覆寫build/c32a，建議下次同里程碑保存原檔。
6. **新方向的證據尺度**：高效能／高穩定／高彈性與航太工程用途要求可追溯的失敗語意及有界資源；本輪僅核對一般emulator功能與帳務證據，沒有最壞延遲／WCET、負載下deadline、長時間可靠性、硬體故障注入或實機量測。現有毫秒數與一次成功不能用來宣稱這些性質已達成。

下一可執行依賴是使用已重建的固定source/做c1 host故障與非slot0 fork測試，以及由root另以固定boot重跑三架構QEMU，保留新命令、完整log hash與未驗清單。這份核對沒有阻止owner繼續別的模組，但上述open項不能省略為結案。
