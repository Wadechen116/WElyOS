# WElyOS 可靠性第 6 項：靜態分析交付

2026-11-18，Codex。工具已完成並在目前工作樹實跑；**專案 gate 尚非全綠**，需要 Claude 逐項處理候選問題及審查例外。
未修改核心或使用者產品程式，未 QEMU，未碰 `build/`，未提交／推送，未安裝工具。

## 使用方法

```sh
bash scripts/static-check.sh
bash scripts/static-check.sh --mode analyze --jobs 2 --timeout 20 --budget 180
python3 tests/static/test_checks.py
```

詳細規則、回傳值與限制見 `scripts/static/README.md`。
exit 1 表示有新提示，exit 2 表示覆蓋不完整／來源變動／工具錯誤；不能把非零當成 pass。

## 實測數據與版本

兩輪都以 HEAD `156972a0cdfae921fa217967cfee5890a8731399` 加當時未提交修改作為來源；
HEAD 本身不足以重現 dirty tree，因此另外保存每個輸入的 SHA-256。

| 項目 | 實測 |
|---|---|
| 詞法範圍 | 132 個 C/H 檔，1,087 個函式定義 |
| 最新快速掃描 | 2.309 秒，928 個提示，其中 3 筆已個別接受，925 筆仍 NEW；exit 1 |
| 風格提示 | missing-check 730、函式超過 80 行 31，共 761 |
| 待判讀候選 | unchecked-error 146、逐架構 possible recursion 15、banned-call 6，共 167（含上述 3 個例外）；不是 167 個已證實 bug |
| 網路迴圈與標記檔案 | `kernel/net/` 沒找到缺乏比較條件的迴圈；標記 `stack.c` 沒找到直接呼叫可達的配置；不是完整有界性／無配置證明 |
| GCC 完整分析 | 66.838 秒；i386 70、riscv64 67、aarch64 67，共 204 個 C TU／旗標組合 |
| GCC 一般診斷 | 69 筆（i386 15、riscv64 27、aarch64 27），含同一問題的跨架構重複與誤報 |
| GCC 覆蓋缺口 | 963 筆 `analyzer-too-complex`（330/333/300），三架構皆 partial，不聲稱分析全路徑 |
| cppcheck | 未安裝：coverage unavailable；未安裝替代工具，該分支未實測 |
| 完整分析 gate | exit 2，因 GCC 覆蓋缺口與 cppcheck 缺失；當時 baseline 尚未加入 3 個例外，新提示共 1,960 |
| 掃描中變動 | 最新快速／完整掃描的來源與工具皆未變動；第一輪曾正確偵測 7 個來源變動並標 stale |
| 工具測試 | 16 個 stdlib unittest 通過，包含植入問題、假呼叫排除、baseline、新程式變動、缺工具與逾時 |

快速結果：`findings-fast.json`、`snapshot-fast.json`；完整分析摘要：`findings-analyze.json`、`snapshot-analyze.json`。
完整診斷路徑與原始來源快照暫存在 `/tmp/welyos-static-analyze-final`，快速輸出在 `/tmp/welyos-static-reviewed-fast`。
暫存目錄不保證重啟後存在；倉庫中的 JSON 摘要保留提示 id／位置／分類／雜湊，不含冗長的逐步 trace。

## 已用原始程式重現的問題（人工 triage，非只憑提示數量）

兩個問題位於 `user/libc.c` 的 `format()`。驗證來源 SHA-256：
`aa6a57ea9b10b3742a99f4fd6cc79fdccfb5416d5ce0b161a57203561ed9506b`。

| 問題 | 觸發 | 實際結果 |
|---|---|---|
| 第 244 行，負寬度取負 | `snprintf(buf, 32, "%*d", INT_MIN, 1)` | UBSan：negation of -2147483648 cannot be represented，exit 1 |
| 第 240 行，十進位寬度累積 | `snprintf(buf, 32, "%2147483648d", 1)` | UBSan：signed integer overflow，2147483640 + 8，exit 1 |

重現工具直接編譯原始 `libc.c`，只在編譯時把函式名稱加前綴隔離，避免覆蓋主機 libc；
不複製 formatter 邏輯、不改產品檔案、不執行 WElyOS syscall。主機 `int` 為 32 位，與三目標一致。

```sh
python3 tests/static/probe_format_width.py
# 鎖定原始快照：
python3 tests/static/probe_format_width.py --source /tmp/welyos-static-analyze-first/source
```

原始重現輸出：`/tmp/welyos-format-probe-g0qwft3k/results.json`。
重現到 UB 或逾時時工具刻意回傳非零；它不是正常 regression pass。
已於 memory `099-format-overflow-confirmed` 交 Claude 修正。
應同時限制寬度的執行成本，不能只避免整數溢位卻仍執行數十億次 padding。
`kernel/kprintf.c:107` 有同類十進位累積，已列為人工候選請 Claude 一併查核；尚未在核心重現。
此處未聲稱已證明可越權或危及核心。

## 其餘候選與風格的區分

- 69 個 GCC 診斷不能直接視作真缺陷：例如 `cp.c`／`df.c` 的 syscall 輸出結構警告，
  涉及 inline assembly 的模型；`user/libc.c` varargs trace 對固定 `%s` 字串卻假設讀取整數路徑，需逐條判讀。
- `kernel/main.c` 的故意 null fault／寫入程式碼與 `overflow_recurse` 是故障注入測試，不能當成正常路徑漏洞。
- `free_table`／`fork_table` 的 level 遞減與 `fs_list` 的 depth 上限是有界遞迴；仍違反「無遞迴」規則，應作個別政策決定，不能偽稱完全符合。
- panic／spinlock／輸出形成詞法呼叫環，但 panic 狀態會改變鎖行為；尚無未受保護無限遞迴的證據。
- `wfs_create`／`wfs_unlink`／`wfs_release` 的丟棄錯誤碼，需與 `fs_failed`、`bsync` 和唯讀降級一起判讀；不因一個 void discard 就確診資料損毀。
- user init 中 6 次 `strcpy` 呼叫目前使用常值與已知測試 buffer，屬禁用函式政策提示，沒有由此證實越界。
- 730 個 missing-check 與 31 個長函式是 style，不建議機械地加無意義斷言或切碎函式來湊數。

## 已審查基準與後續整合

`baseline.txt` 只接受三個網路錯誤處理例外，每筆都有審查者、日期與具體測試／語意理由：
`learn()` 一次性傳送、`arp_rx()` 回覆失敗計數、`net_tick()` 有限重試失敗計數。
這些不是無記錄地吞錯誤；`tx_errors` 與對應 host failure tests 已覆蓋。
其餘項目沒有自動接受，不能直接把當前整份結果變成 baseline。

Claude 持有 `scripts/regress.sh`，應在審查／修復既有提示後再接入強制 gate；
目前若直接接入，exit 1 會正確阻擋回歸。
快速路徑符合一分鐘內的預算；完整 GCC/cppcheck 路徑只按需執行。
詞法掃描未解完整預處理、間接呼叫、所有錯誤型別或可達性，GCC 也不是正確性證明工具。

參考：[JPL 原文](https://spinroot.com/gerard/pdf/P10.pdf)、
[GCC 官方分析器說明](https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html)。

## 後續(2026-11-18,Claude 接手收尾)

Codex 在完成比較功能(`--compare`、`--compare-ref`、`--gate delta`、`--changes-only`,含測試)後用完額度,
使用者請 Claude 接手完成。上面各節是第一輪交付當時的數字,以下是目前的狀態:

| 項目 | 狀態 |
|---|---|
| 工具自身的測試 | `python3 tests/static/test_checks.py`:21 項通過 |
| 寬度溢位的重現工具 | `probe_format_width.py`:star、digits、bounds 三種模式都通過(已修正) |
| 快速掃描 | 886 個未審查(730 missing-check、125 unchecked-error、31 function-length)、32 個已審查接受、沒有過期的基準紀錄 |
| 遞迴、禁用函式 | 全部已逐筆審查(15 + 4),沒有未審查的 |
| 參考報告 | `findings-fast.json`、`snapshot-fast.json` 已更新為新的格式(schema 2) |
| 回歸整合 | `scripts/regress.sh` 最後一步以 delta 比較:新增的遞迴/禁用函式/未檢查回傳視為失敗,風格提示只計數 |
| 分析模式 | `findings-analyze.json`、`snapshot-analyze.json` 仍是第一輪的結果,**沒有**重新產生 |

依這份報告修正的程式缺陷:`printf` 寬度的兩個未定義行為(使用者端與 kernel);
`wfs_unlink`、`wfs_check`、`initrd_unpack` 忽略的錯誤回傳;`file_truncate` 在間接區塊讀不到時遺失資料區塊;
`wfs_release` 與 `dinode_rw` 在讀取失敗時未轉為唯讀;測試中 `malloc` 之後沒有檢查。

尚未做的:125 個 unchecked-error(大多在 `user/init.c`、`user/sh.c`、`user/top.c`)與 GCC 分析器的 69 個診斷仍未逐筆審查。

## 2026-11-18 Codex 獨立複核（回覆 106）

- 核對目前函式 hash 與八筆 baseline 的 reviewed_function_sha256 全部相符，逐筆加入 cross_checked_by。
  mode_heap 四次 strcpy 的來源都是固定字串，目的容量為 10、32、32、80，配置結果先檢查；
  disk_stage_test 四次 unlink 是測試前清理，後續 open/write/assertions 驗證建立成功。
  接受範圍只涵蓋這八個精確 ID，不表示所有忽略回傳值皆合理。
- WFS 指定四條路徑已人工重審：file_truncate 先取得間接區塊才釋放資料；wfs_release 失敗後
  標記唯讀且不增加 free_inodes；dinode_rw 寫入前讀取失敗會標記 fs_failed；wfs_test_damage
  檢查 iupdate。這些修正解決先前提出的具體問題，沒有執行 QEMU 或獨立驗證重開機回收。
  尚需另審 bfree/bitmap_op 錯誤傳遞，以及非對齊截斷讀不到尾端區塊時仍回報成功的策略。
- Gate：函式 token 改動讓 ID 更新並要求重新審查，符合原設計。工作目錄 reference 可作本地比較，
  但 refresh 可在未處理新增 hard finding 時直接覆寫參考，因此不是獨立 CI 防線；CI 應固定到可信
  base commit 的報告並禁止 job 自動刷新。未審查項目永遠不等同接受。
- 實跑 python3 tests/static/test_checks.py：21/21；probe_format_width.py 三模式 returncode 0，
  無 UBSan 訊息。fast delta：142 files、1117 functions，0 newly-unreviewed、898 existing-unreviewed，
  exit 0。完整輸出 /tmp/welyos-codex-107-fast。尚未重跑 GCC analyzer。

### TMR 兩個已重現的阻塞問題（交 Claude 修正產品程式）

1. user/tmr.h:93，副本 sys_fork 失敗後直接 break，父行程保留 out[i][1]；其後讀取 out[i][0]
   永遠等不到 EOF。應在失敗清理路徑關閉所有父行程持有的寫入端，再回收已建立的子行程。
2. user/tmr.h:117，計時器 sys_fork 回傳負值時仍在 line 127 放行三個副本，再阻塞讀取輸出；
   副本若卡住便沒有計時器終止它，line 153 的錯誤判斷永遠到不了。計時器建立失敗必須停止放行，
   關閉 gate、清理及回收子行程後回傳失敗。

重現：python3 tests/static/probe_tmr_failures.py（需 Linux/POSIX 與既有 gcc；不執行 WElyOS syscall）。
複製當前原始 tmr.h，不改函式，僅以 POSIX wrapper 替換 libc.h 及注入 fork 失敗。
正常模式 rc=0；第一次 fork 失敗，以及第四次（timer）fork 失敗搭配停滯 worker，皆超過外部 1 秒期限。
探針超時後終止獨立 process group，避免殘留子行程。結果 /tmp/welyos-tmr-107-5hnz9548/results.json。
這是主機模型重現，尚待 Claude 修正後在真實系統驗證；此探針目前記錄缺陷，不是通過型回歸測試。
