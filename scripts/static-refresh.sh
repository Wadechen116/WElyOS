#!/usr/bin/env bash
# 更新靜態檢查的參考報告(tests/static/findings-fast.json 與 snapshot-fast.json),和程式碼一起提交。
# 下一次提交之後,scripts/static-gate.sh 就以這一份為基準。
# 還有「需要處理的新提示」時拒絕更新:否則只要更新一下報告,新的問題就變成「既有的」而被放過。
# 注意:這不是「接受」任何提示 — 參考報告只是比較的起點,未審查的仍然標為未審查。
set -uo pipefail
cd "$(dirname "$0")/.."
if ! bash scripts/static-gate.sh; then
  echo "還有需要處理的新提示(或檢查無法完成),參考報告沒有更新"
  exit 1
fi
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
