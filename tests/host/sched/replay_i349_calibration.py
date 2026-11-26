#!/usr/bin/env python3
"""以正式 C 分類器重播 JSONL observation；不驗證 manifest/completion 或收集完整性。"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


U64 = (1 << 64) - 1
U32 = (1 << 32) - 1
DIAG = [
    ("lag", 3), ("scope", 6), ("label", 3), ("lag_windows", U32),
    ("longest_run", U32), ("progress_min_pct", U32), ("progress_known", 255),
    ("target_p50", U64), ("target_p95", U64),
    ("target_percentiles_known", 255), ("control_p50", U64),
    ("control_p95", U64), ("control_percentiles_known", 255), ("reason", U32),
]


def shape(value, fields, where):
    if type(value) is not dict or set(value) != set(fields):
        raise ValueError(f"{where}: 欄位必須恰為 {', '.join(fields)}")


def integer(value, maximum, where):
    if maximum == U64:
        if type(value) is not str or not re.fullmatch(r"0|[1-9][0-9]*", value):
            raise ValueError(f"{where}: uint64 必須是標準十進位字串")
        if len(value) > 20:
            raise ValueError(f"{where}: 超出 uint64 範圍")
        value = int(value)
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"{where}: 型別或數值超界")
    return value


def observation(obs):
    shape(obs, "iv nwin win have_control target control base_iter inj p".split(), "obs")
    tokens = [integer(obs["iv"], U64, "iv"), integer(obs["nwin"], 8, "nwin")]
    if type(obs["win"]) is not list or len(obs["win"]) != tokens[1]:
        raise ValueError("win: 長度必須等於 nwin")
    for win in obs["win"]:
        shape(win, ["dc", "dt", "iter", "valid"], "win")
        tokens.extend(integer(win[k], bound, k) for k, bound in
                      [("dc", U64), ("dt", U64), ("iter", U32), ("valid", 255)])
    tokens.append(integer(obs["have_control"], 255, "have_control"))
    for name in ("target", "control"):
        s = obs[name]
        shape(s, ["total", "kept", "truncated", "over", "d"], name)
        kept = integer(s["kept"], 64, "kept")
        tokens.extend([integer(s["total"], U32, "total"), kept,
                       integer(s["truncated"], 255, "truncated"),
                       integer(s["over"], U32, "over")])
        if type(s["d"]) is not list or len(s["d"]) != kept:
            raise ValueError(f"{name}.d: 長度必須等於 kept")
        tokens.extend(integer(d, U64, name + ".d") for d in s["d"])
    shape(obs["base_iter"], ["known", "v"], "base_iter")
    tokens.extend([integer(obs["base_iter"]["known"], 255, "known"),
                   integer(obs["base_iter"]["v"], U32, "base_iter.v"),
                   integer(obs["inj"], (1 << 31) - 1, "inj")])
    fields = ["gap_counts", "lag_tick", "sustained_run", "min_control_samples", "progress_pct"]
    shape(obs["p"], fields, "p")
    tokens.extend(integer(obs["p"][k], U64 if k == "gap_counts" else U32, k)
                  for k in fields)
    return " ".join(map(str, tokens)) + "\n"


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"重複 JSON key: {key}")
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    records = []
    seen = set()
    for line_no, line in enumerate(args.input.read_text().splitlines(), 1):
        if not line.strip():
            continue
        row = json.loads(line, object_pairs_hook=unique_object)
        if type(row) is not dict or row.get("schema") != "i349-calibration-v1":
            raise ValueError(f"第 {line_no} 行: 未知 schema")
        if row.get("type") in ("manifest", "completion"):
            continue  # 僅驗證 observation，不替其他紀錄背書。
        shape(row, ["schema", "type", "run_id", "obs", "diag"], "record")
        run_id = row["run_id"]
        if row["type"] != "observation" or type(run_id) is not str or not run_id:
            raise ValueError(f"第 {line_no} 行: type/run_id 不合法")
        if run_id in seen:
            raise ValueError(f"重複 observation run_id: {run_id}")
        seen.add(run_id)
        shape(row["diag"], [k for k, _ in DIAG], "diag")
        expected = [integer(row["diag"][k], bound, "diag." + k) for k, bound in DIAG]
        records.append((run_id, observation(row["obs"]), expected))
    if not records:
        raise ValueError("沒有 observation 可重播")
    repo = Path(__file__).resolve().parents[3]
    with tempfile.TemporaryDirectory(prefix="i349-replay-") as temporary:
        binary = Path(temporary) / "driver"
        subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(repo / "include"),
                        str(Path(__file__).with_name("i349_replay_driver.c")),
                        "-o", str(binary)], check=True)
        for run_id, protocol, expected in records:
            result = subprocess.run([str(binary)], input=protocol, text=True,
                                    capture_output=True, check=True)
            actual = [int(x) for x in result.stdout.split()]
            if len(actual) != len(DIAG):
                raise ValueError(f"{run_id}: driver 輸出欄位數錯誤，預期 {len(DIAG)}，實際 {len(actual)}")
            if actual != expected:
                differences = [key for (key, _), a, b in zip(DIAG, actual, expected) if a != b]
                raise ValueError(f"{run_id}: diag 不一致: {', '.join(differences)}")
    print(f"{len(records)} 筆 observation 重播一致；未驗證 manifest/completion 或收集完整性")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"重播失敗：{error}", file=sys.stderr)
        sys.exit(1)
