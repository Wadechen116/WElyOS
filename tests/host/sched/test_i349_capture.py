#!/usr/bin/env python3
"""真artifact檔案與synthetic JSONL；不啟QEMU，不把fixture當實際校準資料。"""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
from validate_i349_capture import validate_capture
from test_i349_replay import valid_record


def fixture(root, run_id="none-1"):
    """capture_version=1最小完整schema例；artifact證據使用實際檔案hash。"""
    def evidence(name, data):
        (root / name).write_bytes(data)
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": str(len(data))}

    source = evidence("source.h", b"/* captured classifier header */\n")
    elf = evidence("kernel.elf", b"synthetic ELF fixture, not executable")
    stdout = evidence("stdout.log", b"synthetic selftest log\n")
    manifest = {
        "schema": "i349-calibration-v1", "type": "manifest", "run_id": run_id,
        "capture_version": 1, "arch": "i386", "mode": "none", "cpu_count": 4, "strict_ms": 14,
        "source": {"head_commit": "1" * 40, "dirty": False, "tracked_patch": None,
                   "source_files": [{**source, "source_path": "include/classifier.h", "tracked": True}]},
        "build": {"elf": elf, "classifier_headers": [source], "compiler_version": "gcc fixture",
                  "argv": ["gcc", "-m32", "-O2"], "negative_controls": []},
        "qemu": {"version": "QEMU fixture", "executable_sha256": "2" * 64,
                 "argv": ["qemu-system-i386", "-smp", "4"]},
        "clock": {"source": "tsc", "hz": "1000000000"},
    }
    obs = valid_record()
    obs["run_id"] = run_id
    completion = {
        "schema": "i349-calibration-v1", "type": "completion", "run_id": run_id,
        "capture_version": 1, "termination": "exited", "qemu_exit_code": 0, "runner_exit_code": 0,
        "safe_mode": False, "selftest_started": True, "selftest_finished": True,
        "degraded": False, "panic": False, "capture_complete": True, "capture_errors": [],
        "artifacts": [{**stdout, "role": "stdout"}],
    }
    return [manifest, obs, completion]


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="i349-capture-test-")
        self.root = Path(self.tmp.name)
        self.rows = fixture(self.root)
        self.path = self.root / "capture.jsonl"

    def tearDown(self):
        self.tmp.cleanup()

    def validate(self, rows=None, raw=None):
        if raw is None:
            raw = "".join(json.dumps(row) + "\n" for row in (self.rows if rows is None else rows))
        self.path.write_text(raw, encoding="utf-8")
        return validate_capture(self.path)

    def invalid(self, fragment, rows=None, raw=None):
        report = self.validate(rows, raw)
        self.assertFalse(report["valid"], report)
        self.assertIn(fragment, "\n".join(report["errors"]))
        self.assertEqual(report["candidate_count"], 0)
        return report

    def test_candidate_is_not_replay_or_ground_truth(self):
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertTrue(report["runs"][0]["candidate_requires_replay"])
        self.assertEqual(report["runs"][0]["classification_replay"], "not_run")
        self.assertIn("未驗證實際執行的來源", report["notice"])

    def test_complete_timeout_without_observation_is_preserved(self):
        self.rows[2].update(termination="timeout", qemu_exit_code=None, runner_exit_code=124,
                            selftest_finished=False)
        report = self.validate([self.rows[0], self.rows[2]])
        self.assertTrue(report["valid"])
        run = report["runs"][0]
        self.assertTrue(run["capture_complete"])
        self.assertEqual(run["outcome"], "failed")
        self.assertFalse(run["observation_present"])
        self.assertFalse(run["candidate_requires_replay"])

    def test_failed_then_successful_retry_both_retained(self):
        failed = copy.deepcopy(self.rows)
        for row in failed: row["run_id"] = "failed-1"
        failed[2].update(panic=True, qemu_exit_code=2, runner_exit_code=2, selftest_finished=False)
        report = self.validate([failed[0], failed[2]] + self.rows)
        self.assertTrue(report["valid"])
        self.assertEqual([r["outcome"] for r in report["runs"]], ["failed", "succeeded"])
        self.assertEqual(report["run_count"], 2)
        self.assertEqual(report["candidate_count"], 1)

    def test_missing_completion_is_not_success(self):
        report = self.invalid("missing_completion", self.rows[:2])
        self.assertEqual(report["runs"][0]["outcome"], "incomplete")

    def test_success_cannot_omit_observation(self):
        self.invalid("successful completion必須有observation", [self.rows[0], self.rows[2]])

    def test_observation_before_manifest(self):
        self.invalid("manifest必須先於", [self.rows[1], self.rows[0], self.rows[2]])

    def test_after_terminal(self):
        self.invalid("terminal completion後", self.rows + [self.rows[1]])

    def test_duplicate_manifest_and_observation(self):
        for index in (0, 1):
            with self.subTest(index=index):
                rows = self.rows[:index + 1] + [self.rows[index]] + self.rows[index + 1:]
                self.invalid("重複", rows)

    def test_wrong_run_id_does_not_bind(self):
        self.rows[2]["run_id"] = "different-run"
        report = self.invalid("manifest必須先於")
        self.assertEqual(report["run_count"], 2)

    def test_duplicate_json_key(self):
        raw = "".join(json.dumps(r) + "\n" for r in self.rows)
        raw = raw.replace('"cpu_count": 4', '"cpu_count": 4, "cpu_count": 8')
        self.invalid("重複 JSON key", raw=raw)

    def test_nan_rejected(self):
        raw = "".join(json.dumps(r) + "\n" for r in self.rows)
        self.invalid("非標準JSON常數", raw=raw.replace('"strict_ms": 14', '"strict_ms": NaN'))

    def test_shape_and_boolean_integer(self):
        self.rows[0]["cpu_count"] = True
        self.invalid("型別或數值超界")
        self.rows[0]["cpu_count"] = 4
        self.rows[0]["extra"] = 1
        self.invalid("欄位必須恰為")

    def test_u64_must_be_canonical_string_and_in_range(self):
        for value in (1000, True, "01", "-1", str(1 << 64)):
            with self.subTest(value=value):
                self.rows[0]["clock"]["hz"] = value
                self.invalid("uint64" if value != str(1 << 64) else "超界")

    def test_observation_uses_shared_validator(self):
        self.rows[1]["obs"]["iv"] = "01"
        self.invalid("標準十進位字串")

    def test_dirty_source_needs_patch_and_untracked_snapshot(self):
        m = self.rows[0]
        m["source"]["dirty"] = True
        self.invalid("必須保存tracked_patch")
        data = b"captured patch bytes\n"
        (self.root / "dirty.patch").write_bytes(data)
        m["source"]["tracked_patch"] = {"path": "dirty.patch",
            "sha256": hashlib.sha256(data).hexdigest(), "bytes": str(len(data))}
        m["source"]["source_files"][0]["tracked"] = False
        self.assertTrue(self.validate()["valid"])
        (self.root / "source.h").write_bytes(b"changed untracked source")
        self.invalid("SHA256不符")

    def test_clean_source_cannot_hide_declared_untracked_input(self):
        self.rows[0]["source"]["source_files"][0]["tracked"] = False
        self.invalid("不可宣稱含未追蹤")

    def test_artifact_hash_and_size(self):
        self.rows[2]["artifacts"][0]["sha256"] = "0" * 64
        self.invalid("SHA256不符")
        self.rows[2]["artifacts"][0]["sha256"] = hashlib.sha256((self.root / "stdout.log").read_bytes()).hexdigest()
        self.rows[2]["artifacts"][0]["bytes"] = "0"
        self.invalid("bytes/SHA256不符")

    def test_traversal_and_absolute_path(self):
        for value in ("../source.h", "/tmp/source.h", "a/../source.h", "C:/source.h", "a\\source.h"):
            with self.subTest(value=value):
                self.rows[0]["build"]["elf"]["path"] = value
                self.invalid("相對POSIX路徑")

    def test_symlink_outside_root(self):
        with tempfile.TemporaryDirectory(prefix="i349-outside-") as outside:
            target = Path(outside) / "secret"
            target.write_bytes(b"outside")
            (self.root / "escape").symlink_to(target)
            self.rows[0]["build"]["elf"]["path"] = "escape"
            self.invalid("逸出根目錄")

    def test_capture_failure_is_retained_without_candidate(self):
        self.rows[2].update(capture_complete=False, capture_errors=["log truncated"])
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertEqual(report["runs"][0]["outcome"], "incomplete")
        self.assertEqual(report["candidate_count"], 0)

    def test_capture_errors_cannot_claim_complete(self):
        self.rows[2]["capture_errors"] = ["lost output"]
        self.invalid("矛盾")

    def test_unknown_flags_are_not_false(self):
        for key in ("safe_mode", "degraded", "panic", "selftest_finished"):
            with self.subTest(key=key):
                original = self.rows[2][key]
                self.rows[2][key] = None
                self.assertEqual(self.validate()["candidate_count"], 0)
                self.rows[2][key] = original

    def test_negative_control_is_behavioral_not_candidate(self):
        self.rows[0]["build"]["negative_controls"] = [{"name": "FORCE_LAG", "enabled": True, "value": "1"}]
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertIn("negative_control_enabled", report["runs"][0]["candidate_exclusions"])

    def test_modes_match_actual_injection_enum(self):
        for mode, inj in (("none", 0), ("spike", 1), ("spike", 2), ("spike", 3), ("steady", 4)):
            with self.subTest(mode=mode, inj=inj):
                self.rows[0]["mode"] = mode
                self.rows[1]["obs"]["inj"] = inj
                report = self.validate()
                self.assertTrue(report["valid"])
                self.assertEqual(report["candidate_count"], 0 if inj in (2, 3) else 1)
        self.rows[0]["mode"] = "none"
        self.rows[1]["obs"]["inj"] = 4
        self.invalid("mode與observation.inj不符")

    def test_explicit_classifier_invalid_is_not_candidate(self):
        baseline = copy.deepcopy(self.rows[1]["diag"])
        for key, value in (("lag", 3), ("scope", 0), ("reason", 1 << 3),
                           ("reason", 1 << 4), ("reason", 1 << 8), ("reason", 1 << 12)):
            with self.subTest(key=key, value=value):
                self.rows[1]["diag"] = {**baseline, key: value}
                report = self.validate()
                self.assertTrue(report["valid"])
                self.assertEqual(report["candidate_count"], 0)
                self.assertEqual(report["runs"][0]["classification_replay"], "not_run")

    def test_truncated_samples_are_not_candidate(self):
        self.rows[1]["obs"]["target"]["truncated"] = 1
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertIn("declared_samples_truncated", report["runs"][0]["candidate_exclusions"])

    def test_unknown_clock_is_not_candidate(self):
        self.rows[0]["clock"] = {"source": "unknown", "hz": "0"}
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertEqual(report["candidate_count"], 0)

    def test_arch_counter_source_qualification(self):
        for arch, sources in (("i386", ("tsc", "hpet", "pmtimer", "unknown")),
                              ("riscv64", ("time", "unknown")),
                              ("aarch64", ("cntpct", "unknown"))):
            for source in sources:
                with self.subTest(arch=arch, source=source):
                    self.rows[0]["arch"] = arch
                    self.rows[0]["clock"]["source"] = source
                    self.assertTrue(self.validate()["valid"])
        self.rows[0]["arch"] = "aarch64"
        self.rows[0]["clock"]["source"] = "tsc"
        self.invalid("clock.source與arch")

    def test_complete_samples_timeout_count_matches_declared_clock(self):
        self.rows[0]["clock"]["hz"] = "1"
        self.rows[0]["strict_ms"] = 1
        report = self.invalid("target.over與Hz/strict_ms/d[]不符")
        self.assertEqual(report["run_count"], 1)
        self.assertIn("control.over與Hz/strict_ms/d[]不符", "\n".join(report["errors"]))
        for name in ("target", "control"):
            self.rows[1]["obs"][name]["over"] = 10
        self.assertTrue(self.validate()["valid"])

    def test_timeout_threshold_is_strict_greater_not_equal(self):
        self.rows[0]["clock"]["hz"] = "1000"
        self.rows[0]["strict_ms"] = 1
        for name in ("target", "control"):
            self.rows[1]["obs"][name].update(total=3, kept=3, d=["0", "1", "2"], over=1)
        self.assertTrue(self.validate()["valid"])
        self.rows[1]["obs"]["target"]["over"] = 2
        self.invalid("target.over與Hz/strict_ms/d[]不符")

    def test_truncated_or_unkept_samples_do_not_infer_missing_over(self):
        self.rows[0]["clock"]["hz"] = "1"
        self.rows[0]["strict_ms"] = 1
        for name in ("target", "control"):
            self.rows[1]["obs"][name].update(total=65, kept=10, truncated=1, over=15)
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertEqual(report["candidate_count"], 0)

    def test_unknown_clock_does_not_infer_timeout_count(self):
        self.rows[0]["clock"] = {"source": "unknown", "hz": "1"}
        self.rows[0]["strict_ms"] = 1
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertEqual(report["candidate_count"], 0)

    def test_timeout_arithmetic_overflow_is_not_candidate(self):
        self.rows[1]["obs"]["target"]["d"][0] = str((1 << 64) - 1)
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertIn("target_timeout_arithmetic_overflow", report["runs"][0]["candidate_exclusions"])

    def test_diag_tampering_is_left_to_replay(self):
        self.rows[1]["diag"]["reason"] = 0
        report = self.validate()
        self.assertTrue(report["valid"])
        self.assertEqual(report["runs"][0]["classification_replay"], "not_run")

    def test_cli_json_report_and_read_only(self):
        self.validate()
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("validate_i349_capture.py")),
                                 str(self.path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["run_count"], 1)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.root.iterdir()})

    def test_cli_missing_completion_fails_but_keeps_report(self):
        self.validate(self.rows[:2])
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("validate_i349_capture.py")),
                                 str(self.path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["run_count"], 1)
        self.assertEqual(report["runs"][0]["outcome"], "incomplete")


if __name__ == "__main__":
    unittest.main()
