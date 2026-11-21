# 並行證據盤點（2026-11-21）

本輪只讀核對既有檔案，沒有執行 QEMU、GPU、完整 regress 或修改產品。只新增本報告。歷史固定版本與正在修改的來源分開判讀。

## 固定證據仍完整

目前 `build/milestones/` 只有 `c32-b2` 與 `c32-b2-filepipe` 兩份 manifest；尚無 Claude 174/175 stack rollback / fork reorder 的新固定副本。

兩份 manifest 共八個 boot artifacts（三 ELF 加 AArch64 實際開機用 `.bin`，各一組）重新計算 SHA-256 全符合，八個檔案皆無 write mode bits。六份原 QEMU log 的 hash 前綴全符合 manifest。這是本輪重新核對檔案完整性，不是本輪重新開機。

| 證據 | 本輪 SHA-256 | 判定 |
| --- | --- | --- |
| `build/milestones/c32-b2/manifest.txt` | `f04ee08781da6f2022a333fadc2ed5f3ecfbcc0e8d1c023f709f11d18ce27ada` | 固定 artifacts/logs 符合 |
| `build/milestones/c32-b2-filepipe/manifest.txt` | `6f88c61d760470a66ed8a62cbf55b41353fabdc4a84c46c8c4afcf7b312a5bb8` | 固定 artifacts/logs 符合 |
| `build/c32a/regress-b2.txt` | `f60b3be09c15ea5a5b81c6ac9bef49add0c93baaf1da07dfae3dadd8c8ae6ea4` | 符合既有獨立報告，尾「全部符合預期」 |
| `build/c32a/regress-fp.txt` | `9457e85c8371a8d7c99bafec2bdd4da79e959f8a3e4f1505319cc6ec7b1bd8b9` | 符合既有獨立報告，尾「全部符合預期」 |

既有 `REVIEW-B2-MILESTONE-20261121.md`、`REVIEW-FILEPIPE-MILESTONE-20261121.md` 記錄先前真正獨立三架構 QEMU。file/pipe 的 host 報告 `REVIEW-FILEPIPE-LIFETIME-20261121.md` 記錄 1,536 組與三突變，fixture 侷限仍適用。未將正在修改的來源與歷史 manifest hash 不同解讀為歷史證據失效。

## 最新修改的 regress 尚不能宣告全過

`build/c32a/regress-sr.txt` hash `7bd9f6f37cb7266e486b3bc319db2558aeca0bcf3b2fa5c69a29b0c416ec422e` 尾為「全部符合預期」，但尚無固定 manifest 將它綁到 174/175 的精確版本。

`build/c32a/regress-sr2.txt` hash `326ed3dad84ed1b910fb75a778a91efda9ae1cac1221755e811abccde0504b2b` 尾為「有不符合預期的結果」：i386 selftest 結束碼 3（預期 0），AArch64 selftest 結束碼 1（預期 0），兩者各 25/26 情境；RISC-V 26/26。其餘磁碟、多 CPU、host 網路等多項通過不能抵銷這兩個失敗。

檢查時共享 `build/regress/{i386,aarch64}-selftest.txt` 均見 `kernel/test/sched_test.c:872` 週期執行緒時間斷言失敗。這兩個 log 未列於 sr2 固定 hash；且 i386 log 當前 guest exit1 與 sr2 wrapper rc3 不同，可能為另次執行或覆寫，不能直接將其當 sr2 的確定根因。需 Claude 提供失敗定位與後續完整成功記錄，再交固定 artifacts/manifest。

## 仍需追蹤的工作

| 項目 | 已有證據 | 未完成／限制 |
| --- | --- | --- |
| thread KOBJ | 固定 b2 生命周期、host probe；現行 `mem_test.c:936` 已獨立推導 `4+1+ceil(kmalloc_size/PAGE)` | 新精確斷言尚缺本輪固定版本的獨立 QEMU |
| file/pipe | 歷史固定 QEMU、host failure/lifetime | 現行 `mem_test.c:987` 仍 `KOBJ >= 3`，未精確驗實際四頁成本與差一頁拒絕；host 非真 SMP |
| stack 回退／slot | Claude 174 報告修正；舊反例綁舊 hash | 新版交易、部分配置失敗及並行交錯由另一 agent 驗證；未有新 immutable milestone |
| fork failure 原子性 | 舊 host 真函式反例與 Claude 175 重排 | 新順序需獨立 caller failure probe；全 root/child/frame/thread 配置失敗注入不因成功 QEMU 自動完成 |
| 暫時物件 KOBJ | 設計明列 `ustart/fork_start/frame`；當前 proc.c 仍直接 kmalloc/free | 未見對這三種物件的 charge/release，不能宣告 b2 全部完成 |
| STACK/IMAGE/exec | 設計 (c) 明列 stack 255 頁加頁表、image 與 exec 交易憑證 | I-240 仍部分解決；尚不能保證耗盡時第一次 stack/COW fault 成功 |
| stack 格位回收 | I-330 已記錄 | 執行緒結束不回收格位；需確認無使用者後在 mem.mx 下解除全格、退預留並放格 |
| RAMFS | 設計 (d) FS domain／RAMFS 類別 | `kernel/ramfs.c` 未見 mb/MB hook；I-240 未結案 |

`docs/ISSUES.md` 的 I-240 正文仍保留較早「(b)~(d) 未做」敘述，不能拿此句否定已驗證的 b1/b2 子里程碑；同樣不能用子里程碑通過宣告 I-240 全解。I-133（stack fault 不檢查 SP）、I-134（stack 不縮）與 I-330 仍是獨立 open 項。

## 檢查時來源快照

| 檔案 | SHA-256 |
| --- | --- |
| `kernel/proc.c` | `5867a518f2f578e36fa9af03ba0874c35d884a50bdc4f976bad3126d90e1be8e` |
| `kernel/proc_mem.c` | `f5fa7afabb44b14624d0f5d0a5e8dbfc8002880fe57151d78039dd4c5d0ba84a` |
| `kernel/test/mem_test.c` | `08212732bff3930acd386f73cdc3109a3ea359e324903cf51417b4d633cf4497` |
| `docs/ISSUES.md` | `ffc9ae5ecae9482997bb1d1f0ba44134761ef816bab50142ac08a8bccf63c302` |
| `docs/design/C3.2-memory-accounting.md` | `7856f463749ff22b462326721966b06060eeb519c7ce6ca7333a2696c723d24e` |

來源與共享 regress log 可能由 owner 繼續更新；以上僅綁本輪讀取時點，不是凍結宣告。
