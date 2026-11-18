#!/usr/bin/env bash
# 更新靜態檢查的參考報告(tests/static/findings-fast.json 與 snapshot-fast.json)。
# 回歸測試只回報「相對於參考報告新增的」提示;新增的提示處理完(修正,或逐筆審查後寫進
# tests/static/baseline.txt)之後執行這個腳本,再把更新後的參考報告一起提交。
# 注意:這不是「接受」任何提示 — 參考報告只是比較的起點,未審查的仍然標為未審查。
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
rc=0
bash scripts/static-check.sh --out "$dir/out" > "$dir/log.txt" 2>&1 || rc=$?
if [ "$rc" -ge 2 ]; then
  cat "$dir/log.txt"; echo "掃描不完整,參考報告沒有更新"; exit 2
fi
cp "$dir/out/findings.json" tests/static/findings-fast.json
cp "$dir/out/snapshot-manifest.json" tests/static/snapshot-fast.json
grep '^fast' "$dir/log.txt"
echo "參考報告已更新"
