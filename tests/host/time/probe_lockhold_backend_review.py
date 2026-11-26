#!/usr/bin/env python3
"""以主機 stub 編譯實際 lockhold backend；只模擬 CPU、安全點與時鐘。

不複製模型/控制演算法。--repo 可指向尚未合併的開發 worktree；所有編譯
輸出均在暫存目錄。退出 0=各 gate 通過，1=缺陷重現，2=建置/輸入失敗。
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile


CPU = r'''
#ifndef WELYOS_CPU_H
#define WELYOS_CPU_H
#include <stdint.h>
#define MAX_CPUS 8
struct cpu { int id, online; };
extern struct cpu cpus[MAX_CPUS];
static inline struct cpu *this_cpu(void) { return &cpus[0]; }
void arch_cpu_relax(void);
#endif
'''

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "backend.c"
#if REVIEW_MODEL_NESTED
#define REVIEW_MODEL (lc[0].u.r.m)
#else
#define REVIEW_MODEL (lc[0].u.m)
#endif
struct cpu cpus[MAX_CPUS] = {{.id=0, .online=1}};
static uint64_t host_ticks, counter;
static int deliver_safepoint=1;
static int force_ack_on_relax;
static unsigned relax_calls;
static uint32_t source=1, generation=1, stamp_valid=1;
static unsigned failures, checks;
#define CHECK(condition, name) do { int passed_ = !!(condition); checks++; \
    if (!passed_) failures++; \
    printf("%s %s\n", passed_ ? "PASS" : "FAIL", name); \
} while (0)

/* 控制端等 ACK 時，模擬下一個本地計時器安全點。hook 中不呼叫安全點。 */
static void pump(void) {
    if (deliver_safepoint && cpus[0].online) lockhold_safepoint();
}
uint64_t timer_ticks(void) { pump(); return host_ticks++; }
void arch_cpu_relax(void) {
    relax_calls++;
    if (force_ack_on_relax && cpus[0].online) lockhold_safepoint();
    else pump();
}
/* 新 backend 以 bounded counter 等待；每 tick 100 counts，單次讀取 +10。
   保留至少一次 relax/pump 機會；不得在 stamp 內 pump，否則安全點會遞迴。 */
uint64_t arch_tick_interval(void) { return 100; }
/* I-375 之後安全點改查 spinlock 的持有清單:由探針模擬 */
static int review_held;
int spin_held_probed(void) { return review_held; }
void arch_lh_stamp(struct lh_stamp *out)
{
    counter += 10;
    *out = (struct lh_stamp){.count=counter, .source=source,
                            .generation=generation, .valid=stamp_valid};
}
static void reentry(void)
{
    uint8_t ack[MAX_CPUS];
    struct lockhold_snap snap;
    int result=lockhold_start(1, 1u, 100, ack);
    CHECK(result && ack[0] == LH_ACK_OK, "START1 accepted");
    result=lockhold_start(2, 1u, 100, ack);
    CHECK(!result && ack[0] != LH_ACK_OK, "START2 before STOP rejected");
    lockhold_snapshot(0, &snap);
    CHECK(snap.ok && snap.m.session == 1, "rejected START preserves session1");
    result=lockhold_stop(1u, 100, ack);
    printf("STOP after rejected START: result=%d ack=%s\n", result, lockhold_ack_name(ack[0]));
    CHECK(result && ack[0] == LH_ACK_OK, "STOP1 still acknowledged");
    lockhold_snapshot(0, &snap);
    CHECK(snap.ok && snap.m.session == 1 && snap.m.phase == LH_FROZEN,
          "session1 reaches FROZEN");
    result=lockhold_start(2, 1u, 100, ack);
    printf("START2 after STOP: result=%d ack=%s\n", result, lockhold_ack_name(ack[0]));
    CHECK(result && ack[0] == LH_ACK_OK, "START2 after STOP accepted");
}
static void hooks(int change, int invalid)
{
    uint8_t ack[MAX_CPUS];
    struct lockhold_snap snap;
    int result=lockhold_start(1, 1u, 100, ack);
    CHECK(result && ack[0] == LH_ACK_OK, "hook session starts");
    lockhold_acquired(LH_SCHED);
    if (change) generation++;
    if (invalid) stamp_valid=0;
    lockhold_releasing(LH_SCHED);
    result=lockhold_stop(1u, 100, ack);
    CHECK(result && ack[0] == LH_ACK_OK, "hook session stops");
    lockhold_snapshot(0, &snap);
    CHECK(snap.ok && snap.pub_no && snap.m.phase == LH_FROZEN, "published FROZEN snapshot");
    const struct lh_stat *s=&snap.m.stat[LH_SCHED-1];
    if (change || invalid) {
        CHECK(s->count == 0 && s->invalid == 1, "invalid interval excluded from aggregate count");
        CHECK(snap.m.flags & LH_SOURCE_CHANGED, "source failure retained in snapshot");
        if (invalid) CHECK(snap.m.flags & LH_UNSUPPORTED_SOURCE, "unsupported stamp retained");
    } else {
        CHECK(s->count == 1 && s->invalid == 0, "normal interval counted once");
        CHECK(s->sum == 10 && s->max == 10, "normal interval sum/max");
        CHECK(snap.m.flags == 0, "normal snapshot has no error flags");
        CHECK(s->ring[0].start.generation == 1 && s->ring[0].end.generation == 1,
              "raw stamps retain source generation");
    }
}
static void masks(int out_of_range)
{
    uint8_t ack[MAX_CPUS];
    unsigned invalid_mask=out_of_range ? 1u << MAX_CPUS : 0u;
    CHECK(!lockhold_start(1, invalid_mask, 1, ack),
          out_of_range ? "out-of-range START rejected" : "empty START rejected");
    CHECK(lockhold_start(1, 1, 1, ack), "invalid mask did not consume session1");
    CHECK(!lockhold_stop(0, 1, ack), "empty STOP rejected");
    CHECK(!lockhold_stop(2, 1, ack), "wrong-mask STOP rejected");
    CHECK(lockhold_stop(1, 1, ack), "original mask still closes session1");
    CHECK(lockhold_start(2, 1, 1, ack), "session2 starts after full STOP");
}
static void timeout_start(void)
{
    uint8_t ack[MAX_CPUS];
    deliver_safepoint=0;
    CHECK(!lockhold_start(1, 1, 1, ack) && ack[0] == LH_ACK_TIMEOUT, "START timeout retained");
    CHECK(!lockhold_start(2, 1, 1, ack), "new START blocked while timed-out request unresolved");
    deliver_safepoint=1;
    pump(); /* 原 START 晚到，仍必須自然 STOP，不假裝先前 timeout 是成功。 */
    CHECK(lockhold_stop(1, 1, ack), "late START1 can be STOPped");
    CHECK(lockhold_start(2, 1, 1, ack), "session2 accepted after late START1 drain");
}
static void timeout_stop(void)
{
    uint8_t ack[MAX_CPUS];
    CHECK(lockhold_start(1, 1, 1, ack), "START1 accepted");
    deliver_safepoint=0;
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_TIMEOUT, "STOP timeout retained");
    CHECK(!lockhold_start(2, 1, 1, ack), "new START blocked after STOP timeout");
    deliver_safepoint=1;
    pump();
    CHECK(lockhold_stop(1, 1, ack), "STOP retry observes full acknowledgement");
    CHECK(lockhold_start(2, 1, 1, ack), "session2 accepted after STOP retry");
}
static void timeout_zero(void)
{
    uint8_t ack[MAX_CPUS];
    /* 正確 zero 是 poll-only，不可進 relax；錯誤版本在第一次 relax 收到
       真安全點 ACK 後就返回，因此可快速抓到錯誤，不必跑完整 spin cap。 */
    deliver_safepoint=0;
    force_ack_on_relax=1;
    int result=lockhold_start(1, 1, 0, ack);
    printf("timeout0 observed: result=%d ack=%s relax_calls=%u\n",
           result, lockhold_ack_name(ack[0]), relax_calls);
    CHECK(!result && ack[0] == LH_ACK_TIMEOUT, "timeout0 polls missing ACK then returns TIMEOUT");
    CHECK(relax_calls == 0, "timeout0 never waits or calls relax");
    force_ack_on_relax=0;
    deliver_safepoint=1;
    int stopped=lockhold_stop(1, 100, ack);
#if REVIEW_HAS_QUIESCENT
    CHECK((stopped && ack[0] == LH_ACK_OK) || (!stopped && ack[0] == LH_ACK_QUIESCENT),
          "timed-out START can be explicitly STOPped or cleaned without fabricating data success");
#else
    CHECK(stopped && ack[0] == LH_ACK_OK, "late START can be explicitly STOPped");
#endif
    CHECK(lockhold_start(2, 1, 100, ack), "new START remains available after timeout0 cleanup");
}
#if REVIEW_HAS_QUIESCENT
static void unsupported_cleanup(void)
{
    uint8_t ack[MAX_CPUS];
    struct lockhold_snap snap;
    stamp_valid=0;
    CHECK(!lockhold_start(1, 1, 1, ack), "unsupported initial START fails");
    lockhold_snapshot(0, &snap);
    CHECK(snap.ok && snap.m.phase == LH_OFF && (snap.m.flags & LH_UNSUPPORTED_SOURCE),
          "unsupported START retains OFF and error evidence");
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_QUIESCENT,
          "STOP cleanup returns incomplete with quiescent-no-data ACK");
    lockhold_snapshot(0, &snap);
    CHECK(snap.ok && snap.m.phase == LH_OFF && (snap.m.flags & LH_UNSUPPORTED_SOURCE),
          "cleanup preserves failed model instead of fabricated session data");
    stamp_valid=1;
    CHECK(lockhold_start(2, 1, 1, ack), "valid START2 accepted after explicit cleanup");
    CHECK(lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_OK, "START2 has ordinary data-valid STOP");
}
static void offline_cleanup(void)
{
    uint8_t ack[MAX_CPUS];
    deliver_safepoint=0;
    cpus[0].online=0;
    CHECK(!lockhold_start(1, 1, 1, ack) && ack[0] == LH_ACK_OFFLINE, "offline START does not succeed");
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_OFFLINE,
          "offline STOP does not fabricate quiescent ACK");
    cpus[0].online=1;
    CHECK(!lockhold_start(2, 1, 1, ack), "offline STOP did not clear session_open");
    deliver_safepoint=1;
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_QUIESCENT,
          "actual online safepoint can later acknowledge no-data cleanup");
    CHECK(lockhold_start(2, 1, 1, ack), "new START waits for actual cleanup ACK");
}
static void no_fake_quiescence(void)
{
    uint8_t ack[MAX_CPUS];
    CHECK(lockhold_start(1, 1, 1, ack), "START1 accepted");
    /* 故意破壞本地狀態，驗證防禦分支；不複製 backend 的判定。 */
    REVIEW_MODEL.session=0;
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_INCONSISTENT,
          "ON mismatch is not quiescent");
    CHECK(!lockhold_start(2, 1, 1, ack), "ON mismatch keeps controller open");
    REVIEW_MODEL.phase=LH_OFF;
    REVIEW_MODEL.token[0].active=1;
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_INCONSISTENT,
          "OFF with active token is not quiescent");
    CHECK(REVIEW_MODEL.token[0].active, "failed cleanup does not erase token");
    REVIEW_MODEL.token[0].active=0;
    review_held=1;
    CHECK(!lockhold_stop(1, 1, ack) && ack[0] == LH_ACK_INCONSISTENT,
          "OFF while physically held is not quiescent");
}
#endif
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "reentry")) reentry();
    else if (!strcmp(argv[1], "normal")) hooks(0, 0);
    else if (!strcmp(argv[1], "source_changed")) hooks(1, 0);
    else if (!strcmp(argv[1], "invalid_stamp")) hooks(0, 1);
    else if (!strcmp(argv[1], "masks")) masks(0);
    else if (!strcmp(argv[1], "mask_out_of_range")) masks(1);
    else if (!strcmp(argv[1], "timeout_start")) timeout_start();
    else if (!strcmp(argv[1], "timeout_stop")) timeout_stop();
    else if (!strcmp(argv[1], "timeout_zero")) timeout_zero();
#if REVIEW_HAS_QUIESCENT
    else if (!strcmp(argv[1], "unsupported_cleanup")) unsupported_cleanup();
    else if (!strcmp(argv[1], "offline_cleanup")) offline_cleanup();
    else if (!strcmp(argv[1], "no_fake_quiescence")) no_fake_quiescence();
#else
    else if (!strcmp(argv[1], "unsupported_cleanup") || !strcmp(argv[1], "offline_cleanup") ||
             !strcmp(argv[1], "no_fake_quiescence")) {
        printf("SKIP %s: header has no LH_ACK_QUIESCENT; cleanup contract NOT VERIFIED\n", argv[1]);
        return 77;
    }
#endif
    else return 2;
    printf("%s: %u checks, %u failures\n", argv[1], checks, failures);
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    repo = args.repo.resolve(strict=True)
    inputs = ("kernel/lockhold.c", "include/lockhold.h", "include/lockhold_model.h")
    data = {}
    for name in inputs:
        data[name] = (repo / name).read_bytes()
        print(f"source {name}: sha256={hashlib.sha256(data[name]).hexdigest()}", flush=True)
    print(f"repo: {repo}", flush=True)
    has_quiescent = bool(re.search(rb"\bLH_ACK_QUIESCENT\b", data["include/lockhold.h"]))
    print(f"REVIEW_HAS_QUIESCENT={int(has_quiescent)}", flush=True)
    # 唯一需要碰 backend 私有狀態的是 no_fake_quiescence 的故障注入。
    # 只接受兩個已審查布局，未知布局不可默默跳過安全性 gate。
    union = re.search(rb"union\s+lh_words\s*\{([^}]+)\}", data["kernel/lockhold.c"], re.S)
    if union is None:
        raise ValueError("NOT_APPLICABLE: unknown backend lh_words layout")
    body = union.group(1)
    if re.search(rb"\bstruct\s+lh_rec\s+r\s*;", body):
        nested = True
    elif re.search(rb"\bstruct\s+lh_model\s+m\s*;", body):
        nested = False
    else:
        raise ValueError("NOT_APPLICABLE: unknown backend model layout")
    print("REVIEW_MODEL=" + ("lc[0].u.r.m" if nested else "lc[0].u.m"), flush=True)
    with tempfile.TemporaryDirectory(prefix="welyos-lockhold-backend-") as directory:
        tmp = Path(directory)
        (tmp / "backend.c").write_bytes(data["kernel/lockhold.c"])
        # 全部標頭也取同一 repo 快照，不混入另一 worktree 版本。
        for name in inputs[1:]:
            (tmp / Path(name).name).write_bytes(data[name])
        (tmp / "cpu.h").write_text(CPU, encoding="utf-8")
        (tmp / "timer.h").write_text("#include <stdint.h>\nuint64_t timer_ticks(void);\n", encoding="utf-8")
        # I-375:backend 只需要 spinlock.h 的 spin_held_probed 宣告(實作由探針模擬)
        (tmp / "spinlock.h").write_text("int spin_held_probed(void);\n", encoding="utf-8")
        (tmp / "test.c").write_text(HARNESS, encoding="utf-8")
        build = subprocess.run(
            ["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-DREVIEW_HAS_QUIESCENT=" + str(int(has_quiescent)),
             "-DREVIEW_MODEL_NESTED=" + str(int(nested)),
             "-I", str(tmp), str(tmp / "test.c"), "-o", str(tmp / "probe")],
            capture_output=True, text=True, timeout=30)
        if build.returncode:
            print("ERROR: actual backend failed to compile\n" + build.stderr)
            return 2
        failed = skipped = 0
        cases = ("reentry", "normal", "source_changed", "invalid_stamp", "masks", "mask_out_of_range",
                 "timeout_start", "timeout_stop", "unsupported_cleanup", "offline_cleanup",
                 "no_fake_quiescence", "timeout_zero")
        for case in cases:
            run = subprocess.run([str(tmp / "probe"), case], cwd=tmp,
                                 capture_output=True, text=True, timeout=10)
            print(run.stdout, end="")
            if run.stderr:
                print(run.stderr, end="", file=sys.stderr)
            if run.returncode == 77:
                skipped += 1
                continue
            if run.returncode not in (0, 1):
                print(f"ERROR: {case} terminated with {run.returncode}")
                return 2
            failed += run.returncode != 0
        tested = len(cases) - skipped
        print(f"backend review: {tested-failed}/{tested} tested scenarios passed; "
              f"{skipped} skipped (NOT VERIFIED); {len(cases)} total")
        return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(2)
