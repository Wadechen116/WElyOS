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
