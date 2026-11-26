#!/usr/bin/env python3
"""彙整 I-349 歷史探索資料；缺少的欄位保持空白，不重播分類器。"""

import argparse
import csv
import hashlib
from pathlib import Path
import re


FIELDS = (
    "historical arch mode run source_path source_sha256 recorded_git_commit "
    "exit_code label lag lag_windows window_count longest_run scope target_over "
    "control_over progress_min_pct target_wakes target_worst_counts clock_hz "
    "control_wakes control_worst_counts host_stalls_ge10 iv_min iv_max "
    "target_p50 target_p95 control_p50 control_p95 baseline_iter timestamp_path "
    "safe_mode_detected"
).split()


def collect(source):
    """依架構、模式與數字輪次固定排序，只讀來源實際記錄的值。"""
    exits = {}
    progress = source / "progress.txt"
    if progress.exists():
        for line in progress.read_text().splitlines():
            match = re.match(
                r"(i386|riscv64|aarch64) (none|steady) (\d+) rc=(\d+)", line
            )
            if match:
                arch, mode, run, code = match.groups()
                exits[arch, mode, int(run)] = code

    rows = []
    for arch in ("i386", "riscv64", "aarch64"):
        for mode in ("none", "steady"):
            logs = []
            for path in source.glob(f"{arch}-{mode}-*.txt"):
                match = re.fullmatch(rf"{arch}-{mode}-(\d+)\.txt", path.name)
                if match:
                    logs.append((int(match[1]), path))
            for run, path in sorted(logs):
                data = path.read_bytes()
                text = data.decode(errors="replace")
                row = dict.fromkeys(FIELDS, "")
                row.update(
                    historical="true", arch=arch, mode=mode, run=run,
                    source_path=path.as_posix(),
                    source_sha256=hashlib.sha256(data).hexdigest(),
                    exit_code=exits.get((arch, mode, run), ""),
                    safe_mode_detected=str("safe mode" in text.lower()).lower(),
                )
                timestamp = Path(str(path) + ".ts")
                if timestamp.exists():
                    row["timestamp_path"] = timestamp.as_posix()
                patterns = (
                    (
                        r"diagnosis: ([A-Z-]+) \(lag ([A-Z-]+), (\d+) of "
                        r"(\d+) windows lagging, longest run (\d+); scope "
                        r"([A-Z-]+), over target (\d+) control (\d+); "
                        r"progress min (\d+)%",
                        "label lag lag_windows window_count longest_run scope "
                        "target_over control_over progress_min_pct",
                    ),
                    (
                        r"sleeper woke (\d+) times, worst (\d+) counts "
                        r"\(clock (\d+) Hz\), \d+ over 14 ms, host stalls "
                        r">=10 ticks (\d+)",
                        "target_wakes target_worst_counts clock_hz host_stalls_ge10",
                    ),
                    (
                        r"control sleeper on cpu 2 woke (\d+) times, worst (\d+) counts",
                        "control_wakes control_worst_counts",
                    ),
                )
                for pattern, keys in patterns:
                    match = re.search(pattern, text)
                    if match:
                        row.update(zip(keys.split(), match.groups()))
                intervals = [int(value) for value in re.findall(
                    r"win \d+ dC=\d+ dT=\d+ iv=(\d+)", text
                )]
                if intervals:
                    row.update(iv_min=min(intervals), iv_max=max(intervals))
                rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("build/i349cal"))
    parser.add_argument("--output", type=Path,
                        default=Path("tests/host/sched/i349_calibration_summary.csv"))
    args = parser.parse_args()
    if not args.source.is_dir():
        parser.error(f"找不到來源目錄：{args.source}")
    rows = collect(args.source)
    if not rows:
        parser.error("來源目錄沒有可辨識的歷史資料")
    with args.output.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"已寫入 {len(rows)} 筆歷史資料：{args.output}")


if __name__ == "__main__":
    main()
