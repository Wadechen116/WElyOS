# unchecked-error 控制區塊漏報修正

`unchecked-control-bodies.patch` 修正 `while ((n = read(...)) > 0) write(...);` 的漏報：
原掃描器把條件裡的 `=` 誤認成後面 write 的回傳值有被使用。
新邏輯只取控制條件之後的 body prefix，保留 body 本身的 assignment、return、CHECK 等使用方式。

補丁只改 scripts/static/rules.py 與 tests/static/test_checks.py，沒有改提示 ID、scope、baseline 或 gate。
隔離實作在 `/tmp/welyos-checker-staged-lzirxwue`：24項測試通過；相同新測試換回舊 rules 會失敗。
同一份來源掃描由905變907個總提示，只新增cat的兩次write；905個舊ID全數保留，零移除。
具體來源hash與新提示見 `../checker-control-body-audit.json`。這是該次來源快照，不是後續修改的數量承諾。
`git apply --check tests/static/patches/unchecked-control-bodies.patch` 已通過。

此處刻意交付測過的補丁，未直接更新目前執行中的規則：rules fingerprint 改變會使
HEAD 裡的舊 reference 被正確拒絕（exit2），而 static-refresh.sh 又先執行該 gate。
Claude 應在同一次受審核的規則／報告更新中整合，避免把正常不相容誤當成掃描無新問題。

整合步驟：

1. 在來源穩定時套用補丁，執行 `python3 tests/static/test_checks.py`。
2. 以 `bash scripts/static-check.sh --out /tmp/新的空目錄` 產生完整新掃描，檢查 source/tool 未變與覆蓋狀態。
   若有未審查提示而exit1屬預期，不代表已接受；exit2必須處理，不能用報告覆蓋掩蓋。
3. 逐筆核對新hard findings。這次的兩筆cat應先修正，其他來源改動仍可能引入額外提示。
4. 經人工核對這次規則遷移後，將新 findings.json / snapshot-manifest.json 保存到
   tests/static/findings-fast.json / snapshot-fast.json，與 rules/test 一起由負責整合者提交。
   baseline不做批量接受，也不改舊reference的fingerprint來假裝相容。
5. 新規則與相容report成為HEAD後，再跑正常 static-gate.sh。提交／推送仍遵守使用者的授權。

這只修正已證實的單一詞法盲點，不是完整C AST或資料流分析；無提示不代表所有錯誤回傳皆已檢查。
