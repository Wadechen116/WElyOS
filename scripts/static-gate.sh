#!/usr/bin/env bash
# 靜態檢查的把關:和「上一次提交裡的」參考報告(HEAD:tests/static/findings-fast.json)比較,
# 只看新增的提示。
#   - 新增的遞迴、禁用函式、未檢查的錯誤回傳:失敗(修掉,或逐筆審查後寫進 tests/static/baseline.txt)
#   - 新增的風格提示(沒有參數檢查、函式過長):只列出數量
#   - 檢查本身不完整(工具錯誤、參考報告與規則不相容):失敗
# 比較的對象固定是已提交的報告,不是工作目錄裡那一份:工作目錄的報告可以和程式碼一起被改掉,
# 拿它當基準就等於沒有把關(Codex 在共用記憶 107 號留言指出)。
# 既有尚未審查的提示不會因為「以前出現過」而被當成已接受。
# 結束碼:0 = 通過;1 = 有需要處理的新提示;2 = 無法完成。
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
bash scripts/static-check.sh --compare-ref HEAD:tests/static/findings-fast.json --gate delta \
     --out "$dir/out" > "$dir/log.txt" 2>&1
if [ "$?" -ge 2 ]; then
  # 規則本身改過了(scripts/static/ 和上一次提交不同):上一次提交的報告是用舊規則產生的,
  # 兩者不能比較 — 這是正確的拒絕,不是可以繞過的障礙。這時改用更嚴格的標準:
  # 以新規則做完整掃描,「遞迴、禁用函式、未檢查的錯誤回傳」不可以有任何一筆未審查的
  # (不只是「沒有新增」)。新規則與新的參考報告一起提交之後,就回到一般的比較。
  # 只在「可以確定是規則版本不相容」時才走這條路:檢查程式明確回報了這個原因,而且 scripts/static/
  # 確實和上一次提交不同。其他的比較失敗(參考報告不存在、損壞、不完整…)一律照樣算無法完成 —
  # 那些不是規則變更,不可以被這條後備路徑蓋過去(Codex 的審查意見)。
  if grep -q "comparison schema/mode/scope/rules differ" "$dir/log.txt" &&
     ! git diff --quiet HEAD -- scripts/static/ 2>/dev/null; then
    rm -rf "$dir/out"
    bash scripts/static-check.sh --out "$dir/out" > "$dir/log.txt" 2>&1
    if [ "$?" -ge 2 ]; then
      echo "  靜態檢查無法完成:"; tail -3 "$dir/log.txt" | sed 's/^/    /'
      exit 2
    fi
    python3 - "$dir/out/findings.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
hard = [f for f in d['findings'] if not f['accepted'] and f['rule'] in ('recursion', 'banned-call', 'unchecked-error')]
style = sum(1 for f in d['findings'] if not f['accepted']) - len(hard)
print(f"靜態檢查:規則已變更,無法和上一次提交比較;改用完整掃描:{len(hard)} 個未審查的需要處理的提示"
      f"(必須為 0)、{style} 個風格提示、{len(d['baseline_stale_ids'])} 筆過期的基準紀錄")
for f in hard:
    print(f"  {f['path']}:{f['line']} [{f['rule']}] {f['symbol']}: {f['message'][:100]}")
sys.exit(1 if hard or d['baseline_stale_ids'] else 0)
PYEOF
    exit $?
  fi
  echo "  靜態檢查無法完成:"; tail -3 "$dir/log.txt" | sed 's/^/    /'
  exit 2
fi
python3 - "$dir/out/findings.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
new = set(d['comparison']['newly_unreviewed_ids'])
hard = [f for f in d['findings'] if f['id'] in new and f['rule'] in ('recursion', 'banned-call', 'unchecked-error')]
print(f"靜態檢查:相對於上一次提交,新增 {len(hard)} 個需要處理的提示、{len(new) - len(hard)} 個風格提示"
      f"(既有尚未審查 {len(d['comparison']['existing_unreviewed_ids'])} 個)")
for f in hard:
    print(f"  {f['path']}:{f['line']} [{f['rule']}] {f['symbol']}: {f['message'][:100]}")
sys.exit(1 if hard else 0)
PYEOF
