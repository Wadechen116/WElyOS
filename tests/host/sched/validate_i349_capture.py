#!/usr/bin/env python3
"""唯讀驗證 I-349 capture envelope 及檔案證據；不重播分類器或驗證環境歸因。

manifest/completion 使用 capture_version=1，observation 沿用既有 replay 格式。
所有 artifact path 相對 --artifacts-root（預設 JSONL 所在目錄）。失敗 run 不刪除，
完整保存的失敗也可 capture_complete；candidate_requires_replay 只表示宣告與檔案
證據足以交給下一關，不代表執行來源、建置輸入完整性、diag 或環境 ground truth 已驗證。
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys

sys.dont_write_bytecode = True  # CLI唯讀，不因載入共用validator建立__pycache__。
from replay_i349_calibration import DIAG, U32, U64, integer, observation, shape, unique_object

SCHEMA = "i349-calibration-v1"
COMMON = ["schema", "type", "run_id"]
MANIFEST = COMMON + ["capture_version", "arch", "mode", "cpu_count", "strict_ms",
                     "source", "build", "qemu", "clock"]
COMPLETION = COMMON + ["capture_version", "termination", "qemu_exit_code", "runner_exit_code",
                       "safe_mode", "selftest_started", "selftest_finished", "degraded",
                       "panic", "capture_complete", "capture_errors", "artifacts"]
ARTIFACT = ["path", "sha256", "bytes"]
NOTICE = ("僅檢查宣告、檔案雜湊與capture生命週期；未驗證實際執行的來源、建置輸入是否完整、"
          "QEMU版本/argv是否確實執行、分類重播或環境ground truth。")
# capture v1 對應 include/i349_classify.h 的公開enum，不重作分類演算法。
MODE_INJECTIONS = {"none": {0}, "spike": {1, 2, 3}, "steady": {4}}
ARCH_SOURCES = {"i386": {"tsc", "hpet", "pmtimer", "unknown"},
                "riscv64": {"time", "unknown"}, "aarch64": {"cntpct", "unknown"}}
INVALID_REASON_MASK = (1 << 3) | (1 << 4) | (1 << 8) | (1 << 12)


def text(value, where):
    if type(value) is not str or not value or "\x00" in value:
        raise ValueError(f"{where}: 必須是非空字串且不含NUL")
    return value


def boolean(value, where, nullable=False):
    if type(value) is not bool and not (nullable and value is None):
        raise ValueError(f"{where}: 必須是布林值" + ("或null" if nullable else ""))


def choice(value, choices, where):
    if type(value) is not str or value not in choices:
        raise ValueError(f"{where}: 未知值")


def digest(value, where, lengths=(64,)):
    text(value, where)
    if len(value) not in lengths or re.fullmatch(r"[0-9a-f]+", value) is None:
        raise ValueError(f"{where}: 必須是小寫十六進位雜湊")


def string_list(value, where, nonempty=False):
    if type(value) is not list or (nonempty and not value):
        raise ValueError(f"{where}: 必須是" + ("非空" if nonempty else "") + "字串陣列")
    for item in value:
        text(item, where)


def relative_path(value, where):
    text(value, where)
    p = PurePosixPath(value)
    if (p.is_absolute() or "\\" in value or ":" in value or
            any(part in ("", ".", "..") for part in value.split("/"))):
        raise ValueError(f"{where}: 必須是無traversal的相對POSIX路徑")
    return p


def artifact(value, root, where, extra=()):
    shape(value, ARTIFACT + list(extra), where)
    rel = relative_path(value["path"], where + ".path")
    digest(value["sha256"], where + ".sha256")
    expected_bytes = integer(value["bytes"], U64, where + ".bytes")
    try:
        path = (root / str(rel)).resolve(strict=True)
        path.relative_to(root)
    except (OSError, ValueError, RuntimeError) as error:
        raise ValueError(f"{where}: artifact不存在或逸出根目錄") from error
    if not path.is_file():
        raise ValueError(f"{where}: artifact不是一般檔案")
    sha = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
            size += len(chunk)
    if size != expected_bytes or sha.hexdigest() != value["sha256"]:
        raise ValueError(f"{where}: artifact bytes/SHA256不符")


def manifest(row, root):
    shape(row, MANIFEST, "manifest")
    if integer(row["capture_version"], U32, "capture_version") != 1:
        raise ValueError("capture_version: 不支援")
    choice(row["arch"], ("i386", "riscv64", "aarch64"), "arch")
    choice(row["mode"], ("none", "spike", "steady"), "mode")
    for key in ("cpu_count", "strict_ms"):
        if integer(row[key], U32, key) == 0:
            raise ValueError(f"{key}: 必須大於零")
    source = row["source"]
    shape(source, ["head_commit", "dirty", "tracked_patch", "source_files"], "source")
    digest(source["head_commit"], "head_commit", (40, 64))
    boolean(source["dirty"], "dirty")
    if source["dirty"]:
        if source["tracked_patch"] is None:
            raise ValueError("dirty source: 必須保存tracked_patch（未追蹤檔另存source_files）")
        artifact(source["tracked_patch"], root, "tracked_patch")
    elif source["tracked_patch"] is not None:
        raise ValueError("clean source: tracked_patch必須為null")
    if type(source["source_files"]) is not list or not source["source_files"]:
        raise ValueError("source_files: 必須是非空陣列")
    seen = set()
    for item in source["source_files"]:
        artifact(item, root, "source_file", ("source_path", "tracked"))
        relative_path(item["source_path"], "source_path")
        boolean(item["tracked"], "tracked")
        if item["source_path"] in seen:
            raise ValueError("source_files: source_path重複")
        seen.add(item["source_path"])
        if not source["dirty"] and not item["tracked"]:
            raise ValueError("clean source: 不可宣稱含未追蹤編譯輸入")
    build = row["build"]
    shape(build, ["elf", "classifier_headers", "compiler_version", "argv", "negative_controls"], "build")
    artifact(build["elf"], root, "elf")
    if type(build["classifier_headers"]) is not list or not build["classifier_headers"]:
        raise ValueError("classifier_headers: 必須是非空陣列")
    for item in build["classifier_headers"]:
        artifact(item, root, "classifier_header")
    text(build["compiler_version"], "compiler_version")
    string_list(build["argv"], "build.argv", True)
    controls = build["negative_controls"]
    if type(controls) is not list:
        raise ValueError("negative_controls: 必須是陣列")
    names = set()
    for item in controls:
        shape(item, ["name", "enabled", "value"], "negative_control")
        text(item["name"], "negative_control.name")
        boolean(item["enabled"], "negative_control.enabled")
        text(item["value"], "negative_control.value")
        if item["name"] in names:
            raise ValueError("negative_controls: name重複")
        names.add(item["name"])
    shape(row["qemu"], ["version", "executable_sha256", "argv"], "qemu")
    text(row["qemu"]["version"], "qemu.version")
    digest(row["qemu"]["executable_sha256"], "qemu.executable_sha256")
    string_list(row["qemu"]["argv"], "qemu.argv", True)
    shape(row["clock"], ["source", "hz"], "clock")
    choice(row["clock"]["source"], ARCH_SOURCES[row["arch"]], "clock.source與arch")
    integer(row["clock"]["hz"], U64, "clock.hz")


def completion(row, root):
    shape(row, COMPLETION, "completion")
    if integer(row["capture_version"], U32, "capture_version") != 1:
        raise ValueError("capture_version: 不支援")
    choice(row["termination"], ("exited", "timeout", "launch_error", "capture_error", "interrupted"),
           "termination")
    for key in ("qemu_exit_code", "runner_exit_code"):
        code = row[key]
        if code is not None and (type(code) is not int or not -(1 << 31) <= code < (1 << 31)):
            raise ValueError(f"{key}: 必須是int32或null")
    if row["termination"] == "exited" and row["qemu_exit_code"] is None:
        raise ValueError("exited: 缺qemu_exit_code")
    if row["termination"] == "launch_error" and row["qemu_exit_code"] is not None:
        raise ValueError("launch_error: 不可宣稱QEMU exit code")
    for key in ("safe_mode", "selftest_started", "selftest_finished", "degraded", "panic"):
        boolean(row[key], key, True)
    boolean(row["capture_complete"], "capture_complete")
    string_list(row["capture_errors"], "capture_errors")
    if row["capture_complete"] and row["capture_errors"]:
        raise ValueError("capture_complete與capture_errors矛盾")
    if row["selftest_finished"] is True and row["selftest_started"] is False:
        raise ValueError("selftest_finished但未selftest_started")
    if type(row["artifacts"]) is not list or not row["artifacts"]:
        raise ValueError("completion.artifacts: 必須是非空陣列")
    paths = set()
    roles = set()
    for item in row["artifacts"]:
        artifact(item, root, "completion.artifact", ("role",))
        choice(item["role"], ("stdout", "stderr", "timestamps", "other"), "artifact.role")
        if item["path"] in paths:
            raise ValueError("completion.artifacts: path重複")
        paths.add(item["path"])
        roles.add(item["role"])
    if "stdout" not in roles:
        raise ValueError("completion.artifacts: 必須保存stdout（可為空檔）")


def checked_observation(row):
    shape(row, COMMON + ["obs", "diag"], "observation")
    observation(row["obs"])
    shape(row["diag"], [key for key, _ in DIAG], "diag")
    for key, bound in DIAG:
        integer(row["diag"][key], bound, "diag." + key)


def reject_constant(value):
    raise ValueError(f"非標準JSON常數: {value}")


def validate_capture(path, artifacts_root=None):
    path = Path(path)
    root = Path(artifacts_root).resolve(strict=True) if artifacts_root is not None else path.parent.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("artifacts-root不是目錄")
    runs = {}
    errors = []
    global_bad = False
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        state = None
        try:
            row = json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant)
            if type(row) is not dict or row.get("schema") != SCHEMA:
                raise ValueError("未知schema")
            run_id = row.get("run_id")
            if type(run_id) is not str or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", run_id) is None:
                raise ValueError("run_id: 必須是1..128字元的可攜識別碼")
            state = runs.setdefault(run_id, {"manifest": None, "observation": None,
                                            "completion": None, "errors": []})
            kind = row.get("type")
            if type(kind) is not str or kind not in ("manifest", "observation", "completion"):
                raise ValueError("未知record type")
            if state["completion"] is not None:
                raise ValueError("terminal completion後不可再有紀錄")
            if state[kind] is not None:
                raise ValueError(f"重複{kind}")
            if kind != "manifest" and state["manifest"] is None:
                raise ValueError("manifest必須先於observation/completion")
            if kind == "manifest": manifest(row, root)
            elif kind == "completion": completion(row, root)
            else: checked_observation(row)
            state[kind] = row
        except (ValueError, OSError, RuntimeError) as error:
            message = f"line {line_no}: {error}"
            errors.append(message)
            if state is None:
                global_bad = True
            else:
                state["errors"].append(message)
    if not runs:
        errors.append("沒有run紀錄")
    reports = []
    for run_id, state in runs.items():
        m, o, c = (state[key] for key in ("manifest", "observation", "completion"))
        exclusions = []
        if m is None:
            exclusions.append("missing_manifest")
            errors.append(f"run {run_id}: missing_manifest")
        if c is None:
            exclusions.append("missing_completion")
            errors.append(f"run {run_id}: missing_completion")
        if o is None: exclusions.append("missing_observation")
        if global_bad or state["errors"]: exclusions.append("invalid_envelope_or_artifact")
        if m is not None and o is not None:
            if o["obs"]["inj"] not in MODE_INJECTIONS[m["mode"]]:
                message = f"run {run_id}: manifest.mode與observation.inj不符"
                errors.append(message)
                state["errors"].append(message)
                exclusions.append("injection_mode_mismatch")
            if o["obs"]["inj"] in (2, 3):
                exclusions.append("spike_invalid_or_missed")
            diag = o["diag"]
            if diag["lag"] == 3 or diag["scope"] == 0 or diag["reason"] & INVALID_REASON_MASK:
                exclusions.append("declared_classifier_invalid")
            if diag["reason"] & 1 or o["obs"]["target"]["truncated"] or o["obs"]["control"]["truncated"]:
                exclusions.append("declared_samples_truncated")
            hz = int(m["clock"]["hz"])
            if hz and m["clock"]["source"] != "unknown":
                threshold = hz * m["strict_ms"]
                for name in ("target", "control"):
                    samples = o["obs"][name]
                    # 未保留的樣本可能超時；截斷時不能從d[]反推全部over。
                    if samples["truncated"] or samples["total"] != samples["kept"]:
                        continue
                    durations = [int(d) * 1000 for d in samples["d"]]
                    # kernel以uint64相乘；若會溢位，不能用Python大整數當實測語義。
                    if threshold > U64 or any(d > U64 for d in durations):
                        exclusions.append(name + "_timeout_arithmetic_overflow")
                        continue
                    expected = sum(d > threshold for d in durations)
                    if samples["over"] != expected:
                        message = f"run {run_id}: {name}.over與Hz/strict_ms/d[]不符"
                        errors.append(message)
                        state["errors"].append(message)
                        exclusions.append(name + "_timeout_count_mismatch")
        terminal_success = False
        if c is not None:
            terminal_success = (c["termination"] == "exited" and c["qemu_exit_code"] == 0 and
                                c["runner_exit_code"] == 0 and c["selftest_started"] is True and
                                c["selftest_finished"] is True and c["safe_mode"] is False and
                                c["degraded"] is False and c["panic"] is False)
            if c["termination"] != "exited": exclusions.append(c["termination"])
            for key in ("qemu_exit_code", "runner_exit_code"):
                if c[key] != 0: exclusions.append(key + "_nonzero_or_unknown")
            for key, expected in (("safe_mode", False), ("selftest_started", True),
                                  ("selftest_finished", True), ("degraded", False), ("panic", False)):
                if c[key] is not expected: exclusions.append(key + "_unexpected_or_unknown")
            if not c["capture_complete"]: exclusions.append("capture_incomplete")
            if terminal_success and o is None:
                message = f"run {run_id}: successful completion必須有observation"
                errors.append(message)
                state["errors"].append(message)
        if m is not None:
            if m["clock"]["source"] == "unknown" or int(m["clock"]["hz"]) == 0:
                exclusions.append("unknown_clock")
            if any(item["enabled"] for item in m["build"]["negative_controls"]):
                exclusions.append("negative_control_enabled")
        capture_valid = not (global_bad or state["errors"] or m is None or c is None)
        complete = bool(capture_valid and c["capture_complete"])
        if not capture_valid: outcome = "invalid" if state["errors"] or global_bad else "incomplete"
        elif not complete: outcome = "incomplete"
        elif terminal_success: outcome = "succeeded"
        else: outcome = "failed"
        reports.append({"run_id": run_id, "capture_valid": capture_valid,
                        "declared_capture_complete": c["capture_complete"] if c else None,
                        "capture_complete": complete, "outcome": outcome,
                        "observation_present": o is not None,
                        "candidate_requires_replay": not exclusions,
                        "candidate_exclusions": exclusions, "classification_replay": "not_run",
                        "errors": state["errors"]})
    return {"schema": "i349-capture-report-v1", "valid": not errors,
            "notice": NOTICE, "run_count": len(reports),
            "candidate_count": sum(r["candidate_requires_replay"] for r in reports),
            "runs": reports, "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--artifacts-root", type=Path)
    args = parser.parse_args()
    try:
        report = validate_capture(args.input, args.artifacts_root)
    except (ValueError, OSError, RuntimeError) as error:
        print(json.dumps({"valid": False, "errors": [str(error)], "notice": NOTICE}, ensure_ascii=False))
        return 1
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
