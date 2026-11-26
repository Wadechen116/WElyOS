#!/usr/bin/env python3
"""透過子行程編譯並呼叫正式分類器，驗證重播工具的資料邊界。"""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def valid_record():
    sleeper = {"total": 10, "kept": 10, "truncated": 0, "over": 0,
               "d": ["120000"] * 10}
    return {
        "schema": "i349-calibration-v1", "type": "observation", "run_id": "none-1",
        "obs": {
            "iv": "10000", "nwin": 1,
            "win": [{"dc": "500000", "dt": "50", "iter": 200000, "valid": 1}],
            "have_control": 1, "target": sleeper, "control": copy.deepcopy(sleeper),
            "base_iter": {"known": 1, "v": 200000}, "inj": 0,
            "p": {"gap_counts": "40000", "lag_tick": 2, "sustained_run": 3,
                  "min_control_samples": 10, "progress_pct": 80},
        },
        "diag": {
            "lag": 0, "scope": 2, "label": 0, "lag_windows": 0, "longest_run": 0,
            "progress_min_pct": 100, "progress_known": 1,
            "target_p50": "120000", "target_p95": "120000", "target_percentiles_known": 1,
            "control_p50": "120000", "control_p95": "120000", "control_percentiles_known": 1,
            "reason": 2048,
        },
    }


def invalid_diag(reason):
    return {
        "lag": 3, "scope": 0, "label": 3, "lag_windows": 0, "longest_run": 0,
        "progress_min_pct": 0, "progress_known": 0,
        "target_p50": "0", "target_p95": "0", "target_percentiles_known": 0,
        "control_p50": "0", "control_p95": "0", "control_percentiles_known": 0,
        "reason": reason,
    }


class ReplayTests(unittest.TestCase):
    def invoke(self, text, success, message):
        with tempfile.TemporaryDirectory(prefix="i349-replay-test-") as directory:
            source = Path(directory) / "input.jsonl"
            source.write_text(text)
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("replay_i349_calibration.py")),
                 str(source)], capture_output=True, text=True, timeout=30,
            )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        self.assertIn(message, result.stdout + result.stderr)

    def record(self, row, success, message):
        self.invoke(json.dumps(row) + "\n", success, message)

    def test_none(self):
        self.record(valid_record(), True, "1 筆 observation 重播一致")

    def test_missing_target_is_classifier_invalid(self):
        row = valid_record()
        row["obs"]["target"] = {"total": 0, "kept": 0, "truncated": 0, "over": 0, "d": []}
        row["diag"] = invalid_diag(4096)
        self.record(row, True, "重播一致")

    def test_unknown_injection_is_classifier_invalid(self):
        row = valid_record()
        row["obs"]["inj"] = 99
        row["diag"] = invalid_diag(256)
        self.record(row, True, "重播一致")

    def test_tampered_diag(self):
        row = valid_record()
        row["diag"]["reason"] = 0
        self.record(row, False, "diag 不一致: reason")

    def test_duplicate_key(self):
        text = json.dumps(valid_record()).replace('"run_id": "none-1"',
                                                 '"run_id": "none-1", "run_id": "none-2"')
        self.invoke(text, False, "重複 JSON key: run_id")

    def test_duplicate_run_id(self):
        self.invoke((json.dumps(valid_record()) + "\n") * 2, False,
                    "重複 observation run_id")

    def test_bool_is_not_integer(self):
        row = valid_record()
        row["obs"]["nwin"] = True
        self.record(row, False, "型別或數值超界")

    def test_u64_overflow(self):
        row = valid_record()
        row["obs"]["iv"] = str(1 << 64)
        self.record(row, False, "型別或數值超界")

    def test_missing_field(self):
        row = valid_record()
        del row["obs"]["iv"]
        self.record(row, False, "obs: 欄位必須恰為")

    def test_memory_bounds(self):
        row = valid_record()
        row["obs"]["target"]["kept"] = 65
        self.record(row, False, "型別或數值超界")

    def test_system_wide_sustained_low_progress(self):
        row = valid_record()
        # 每窗比 (50+2)*10000 多一計數，連續三窗；100000/200000 = 50%。
        row["obs"]["nwin"] = 3
        row["obs"]["win"] = [
            {"dc": "520001", "dt": "50", "iter": 100000, "valid": 1}
            for _ in range(3)
        ]
        for name in ("target", "control"):
            row["obs"][name].update(over=10, d=["150000"] * 10)
        row["diag"].update(
            lag=2, scope=3, label=1, lag_windows=3, longest_run=3,
            progress_min_pct=50, target_p50="150000", target_p95="150000",
            control_p50="150000", control_p95="150000", reason=4,
        )
        self.record(row, True, "重播一致")

    def test_system_wide_normal_progress_is_inconclusive(self):
        row = valid_record()
        # 同樣連續落後，但進度維持基準值，不足以標 ENVIRONMENT_SUSPECT。
        row["obs"]["nwin"] = 3
        row["obs"]["win"] = [
            {"dc": "520001", "dt": "50", "iter": 200000, "valid": 1}
            for _ in range(3)
        ]
        for name in ("target", "control"):
            row["obs"][name].update(over=10, d=["150000"] * 10)
        row["diag"].update(
            lag=2, scope=3, label=3, lag_windows=3, longest_run=3,
            target_p50="150000", target_p95="150000",
            control_p50="150000", control_p95="150000", reason=4,
        )
        self.record(row, True, "重播一致")

    def test_target_local_gap(self):
        row = valid_record()
        # target 的 p95=200000，嚴格大於 control 120000 + gap 40000。
        row["obs"]["target"].update(over=10, d=["200000"] * 10)
        row["diag"].update(scope=5, label=2, target_p50="200000",
                             target_p95="200000", reason=2)
        self.record(row, True, "重播一致")

    def test_truncated_target_cannot_be_candidate(self):
        row = valid_record()
        # retained p95 差距相同，但第65筆未保留，不能判 TARGET_LOCAL。
        row["obs"]["target"].update(total=65, kept=64, truncated=1,
                                      over=65, d=["200000"] * 64)
        row["diag"].update(scope=6, label=3, target_p50="200000",
                             target_p95="200000", reason=1)
        self.record(row, True, "重播一致")

    def test_window_arithmetic_overflow_is_invalid(self):
        row = valid_record()
        # dt+lag_tick 超出 uint64；輸入值本身仍是合法 uint64。
        row["obs"]["win"][0]["dt"] = str((1 << 64) - 1)
        row["diag"] = invalid_diag(8)
        self.record(row, True, "重播一致")


if __name__ == "__main__":
    unittest.main()
