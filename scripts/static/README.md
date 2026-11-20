# WElyOS 靜態檢查

```sh
bash scripts/static-check.sh
bash scripts/static-check.sh --mode analyze --jobs 2 --timeout 20 --budget 180
python3 tests/static/test_checks.py
```

只依賴 Python 標準函式庫與既有編譯工具，不下載或安裝東西。預設輸出到新的 `/tmp/welyos-static-*`；
可用 `--out /tmp/empty-directory` 指定空目錄。禁止把輸出放進專案，尤其不碰 Claude 使用的 `build/`。

## Gate 與審查

- exit 0：所選模式沒有新的提示，且所要求的工具／快照完整；**不代表程式正確性證明**。
- exit 1：有未審查的新提示，包括風格提示。第一輪有大量既有提示是預期的，不應直接接到必須全綠的回歸 gate。
- exit 2：來源或工具在掃描期間改變、缺工具、逾時、編譯錯誤或分析路徑因複雜度不足而中止。

`tests/static/baseline.txt` 每行是一筆 JSON，必須有 id、reviewer、reason、date。
從報告選取**單一**提示、查看原始碼與測試後，寫明適用範圍／前提才可接受。
不提供批量接受當前結果的開關。提示 ID 綁定規則、檔案、函式 token 與具體位置脈絡；
函式內文字常值改變也會產生新 ID，純粹在函式前插入空行則不變。
baseline 仍需人工確認跨函式前提／巨集變更；它不是永久正確性保證。
`baseline_stale_ids` 列出目前沒有再出現的接受紀錄，不能自動移除或轉套到新提示。

## 與參考報告比較(只看新增的提示)

```sh
bash scripts/static-check.sh --compare tests/static/findings-fast.json --gate delta --changes-only
bash scripts/static-check.sh --compare-ref HEAD:tests/static/findings-fast.json --gate delta
bash scripts/static-refresh.sh        # 處理完新增的提示之後,更新參考報告
```

- `--compare 檔案` 或 `--compare-ref 版本:路徑`(固定到某個 commit 的報告)指定比較的對象;只支援快速模式。
- `--gate delta`:只有「新增的未審查提示」才讓結果為 exit 1;預設的 `--gate all` 不變(任何未審查的提示都是 exit 1)。
- `--changes-only`:主控台與 Markdown 只列新增的;`findings.json` 永遠是完整的。
- 參考報告必須和目前的規則版本、掃描範圍相容,而且本身是完整的;否則是 exit 2,不會被當成通過。
- **比較不是審查。** 參考報告裡已經出現過的提示仍然是「既有未審查」(existing-unreviewed),
  不會變成已接受;delta 通過不代表專案沒有問題。
- 提示的 ID 含有函式內容的雜湊:修改一個本來就有提示的函式,那個提示會以新的 ID 重新出現,算作新增。

`scripts/regress.sh` 的最後一步(`scripts/static-gate.sh`)就是這個比較:新增的遞迴、禁用函式、
未檢查的錯誤回傳會讓回歸失敗;新增的風格提示(missing-check、function-length)只列出數量。
比較的對象固定是 `HEAD` 裡的報告;`scripts/static-refresh.sh` 在還有需要處理的新提示時拒絕更新參考報告。

## 快速模式的明確界線

所有 `kernel/`、`arch/`、`lib/`、`user/` 的 C 檔與支援標頭 `include/` 都進入詞法掃描。

| 規則 | 做法與限制 |
|---|---|
| recursion | 只對 `kernel/`、`arch/` 的函式逐架構建立直接呼叫圖並報強連通分量；使用者程式（含 `user/*.c`、`user/*.h`）不在本規則範圍。使用者標頭仍會掃描其他適用規則，沒有遞迴提示不代表使用者函式沒有遞迴。未解 callback、組語、預處理可行性及巨集呼叫，因此只報 possible cycle。 |
| loop-bound | 限定 `kernel/net/`（包含內部標頭），檢查條件是否有可見比較式；不是固定迭代次數、進度或 WCET 證明。其餘核心的常駐輪詢不在此規則範圍。 |
| allocation-after-init | 有 `/* no-alloc-after-init */` 的檔案，從非 `*_init`／`init_*` 的函式做直接呼叫可達性搜尋，尋找 kmalloc/pmm_alloc 類呼叫。無法證明間接呼叫或初始化階段。 |
| function-length | 實體原始碼行數大於 80，屬 style；這是本專案門檻。 |
| missing-check | 沒有 assertion 或引用參數的 if 條件，屬 style；由呼叫端保證前提的小函式常會被提示。 |
| unchecked-error | 由明確 `return -E_*` 與簡單返回傳播推導錯誤函式，找獨立丟棄的回傳值，包含 `(void)`。不完整的型別／資料流分析，不能涵蓋全部 callback 或 syscall。 |
| pte-direct-store | 限 `kernel/vm.c`，對宣告為 `pte_t *` 的具名指標，攔截直接解參考／索引的賦值及常見複合更新，要求用 `arch_pte_set`；指標宣告及重新指向不受禁止。這是詞法 guard，不能取代型別分析，cast／void alias／巨集與 bulk write 仍需人工審查。 |
| banned-call | 提議禁用 gets、strcpy、strcat、sprintf、vsprintf、scanf、sscanf、vscanf、vsscanf、alloca、__builtin_alloca。找到呼叫不等於已證實越界；例如已知常值的安全測試案例需逐筆審查。 |

不做註解／字串中的假呼叫比對；函式抽取不是完整 C parser，特殊 attributes／巨集語法可能漏掉。
這些規則受 JPL Power of Ten 啟發，**不是符合該規範的認證或完整實作**。

## 完整分析模式

先複製來源快照，再在 `/tmp` 做 CMake **configure-only**，取得三架構真實的 compile database，
包含 `ring` 與 `list` 執行佇列變體；每個相同 C TU／旗標組合只跑一次。
不 build kernel、不執行 QEMU、不連結或啟動目標程式。
GCC 沿用原本的 `-O2`、架構旗標、include、define，只替換物件輸出路徑並新增 `-fanalyzer` 與診斷選項。
因此並未另跑 `-O0` 分析，最佳化可能影響可見警告。

每個 TU 預設 30 秒，GCC 排程總預算預設 240 秒，並行預設 2 個。
超過總預算不再開始新 TU；已開始的 TU 最多再用一個 timeout。
cppcheck 若有安裝，以同一份各架構 compile database、unix32/unix64 平台執行；本機未安裝，
因此該分支未獲實測，明確顯示 `coverage unavailable`，不計為 pass。
GCC 的 `-Wanalyzer-too-complex` 亦明示覆蓋缺口，不隱藏。

每次輸出包含來源／工具 SHA-256 manifest、HEAD、dirty 狀態、完整診斷 JSON、
每個分析指令、CMake logs 與可讀報告；來源變動會標 stale。
完整工具實測約 67 秒，超過快迴歸預算，應按需執行。

參考：[JPL 作者原文](https://spinroot.com/gerard/pdf/P10.pdf)、
[GCC 靜態分析說明](https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html)。
