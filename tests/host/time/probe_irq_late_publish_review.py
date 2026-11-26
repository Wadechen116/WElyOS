#!/usr/bin/env python3
"""對實際 irq_late.c 的解鎖後發布位置插樁，重現兩個 config writer 的競賽。

退出碼：0 = 原版未重現或候選修補通過；1 = 缺陷重現或候選修補失敗；2 = 不適用或執行失敗。
不適用不能當成已修；本探針只識別目前審查過的精確程式片段。
"""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess
import tempfile


HARNESS = r'''
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int paused, resume_writer;
static _Thread_local int writer_one;
static void review_after_unlock(void)
{
    if (!writer_one) return;
    pthread_mutex_lock(&gate);
    paused = 1;
    pthread_cond_broadcast(&changed);
    while (!resume_writer) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
#include "instrumented.c"
static void *first_writer(void *unused)
{
    (void)unused;
    writer_one = 1;
    irq_late_config(0, 1, IRQL_KIND_GAP, 10);
    return NULL;
}
int main(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, first_writer, NULL)) return 2;
    pthread_mutex_lock(&gate);
    while (!paused) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    /* W1 已釋放 writer seq；原版尚未發布 gen，候選版已發布。W2 完成下一個設定。 */
    irq_late_config(0, 2, IRQL_KIND_GAP, 20);
    uint32_t newer = __atomic_load_n(&rq[0].gen, __ATOMIC_ACQUIRE);
    uint32_t payload_gen = __atomic_load_n(&rq[0].gen_in, __ATOMIC_RELAXED);
    printf("after W2: published_gen=%u payload_gen=%u\n", newer, payload_gen);
    pthread_mutex_lock(&gate);
    resume_writer = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    if (pthread_join(thread, NULL)) return 2;
    uint32_t final = __atomic_load_n(&rq[0].gen, __ATOMIC_ACQUIRE);
    payload_gen = __atomic_load_n(&rq[0].gen_in, __ATOMIC_RELAXED);
    printf("after W1 resumes: published_gen=%u payload_gen=%u\n", final, payload_gen);
    if (REVIEW_EXPECT_FIXED) {
        if (newer == 2 && final == 2 && payload_gen == 2) {
            puts("PASS: reviewed fix keeps published and payload generation at 2 after both writers");
            return 0;
        }
        puts("FAIL: candidate did not preserve the latest request generation");
        return 1;
    }
    if (newer == 2 && final == 1 && payload_gen == 2) {
        puts("DEFECT: published generation regressed; request generation and payload disagree");
        return 1;
    }
    puts("NO_REPRODUCTION: this controlled schedule did not reproduce the reviewed defect");
    return 0;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-fix", action="store_true",
                        help="只在暫存副本測試 acquire CAS 及互斥內發布 gen 的候選修補")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source_path = root / "kernel/irq_late.c"
    source = source_path.read_text(encoding="utf-8")
    # 只接受已審查的舊版次序或正式修正版，未知變體不能當作通過。
    begin = source.find("void irq_late_config(")
    end = source.find("static int req_read(", begin)
    if begin < 0 or end < 0:
        print("NOT_APPLICABLE: reviewed config/req_read function boundaries changed")
        return 2
    config = source[begin:end]
    pattern = re.compile(
        r"ST\(&r->seq, s \+ 2\);(?P<comment>\s*/\*[^*]*\*/\s*)"
        r"__atomic_store_n\(&r->gen, g, __ATOMIC_RELEASE\);"
    )
    fixed_pattern = re.compile(
        r"__atomic_store_n\(&r->gen, g, __ATOMIC_RELEASE\);\s*"
        r"__atomic_thread_fence\(__ATOMIC_RELEASE\);\s*"
        r"__atomic_store_n\(&r->seq, s \+ 2, __ATOMIC_RELEASE\);"
    )
    fixed_cas = re.compile(
        r"__atomic_compare_exchange_n\(&r->seq, &s, s \+ 1, 0,\s*"
        r"__ATOMIC_ACQUIRE, __ATOMIC_RELAXED\)"
    )
    old_matches = len(list(pattern.finditer(config)))
    fixed_matches = len(list(fixed_pattern.finditer(config)))
    already_fixed = fixed_matches == 1 and old_matches == 0 and len(list(fixed_cas.finditer(config))) == 1
    if not already_fixed and not (old_matches == 1 and fixed_matches == 0):
        print("NOT_APPLICABLE: neither reviewed original nor reviewed fixed publication sequence matches")
        return 2
    if already_fixed:
        config = fixed_pattern.sub(lambda m: m.group(0) + "\nreview_after_unlock();", config)
    elif args.candidate_fix:
        cas = re.compile(r"(__atomic_compare_exchange_n\(&r->seq, &s, s \+ 1, 0,\s*)"
                         r"__ATOMIC_RELAXED, __ATOMIC_RELAXED\)")
        if len(list(cas.finditer(config))) != 1:
            print("NOT_APPLICABLE: reviewed relaxed writer CAS changed")
            return 2
        config = cas.sub(lambda m: m.group(1) + "__ATOMIC_ACQUIRE, __ATOMIC_RELAXED)", config)
        # 候選修補：gen 發布仍在 writer 互斥內；hook 留在釋放之後，W2 可正常取得。
        config = pattern.sub(
            lambda m: m.group("comment")
            + "__atomic_store_n(&r->gen, g, __ATOMIC_RELEASE);\n"
            + "ST(&r->seq, s + 2);\nreview_after_unlock();", config)
    else:
        config = pattern.sub(
            lambda m: "ST(&r->seq, s + 2);\nreview_after_unlock();"
            + m.group("comment")
            + "__atomic_store_n(&r->gen, g, __ATOMIC_RELEASE);", config)
    instrumented = source[:begin] + config + source[end:]
    print("actual source:", source_path, flush=True)
    print("source sha256:", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    mode = "actual-fixed-source" if already_fixed else (
        "candidate-fix (/tmp copy only)" if args.candidate_fix else "original-source")
    print("mode:", mode, flush=True)
    if already_fixed and args.candidate_fix:
        print("candidate-fix: actual source already contains the reviewed fix; no candidate rewrite applied", flush=True)
    print("instrumentation: W1 pauses immediately after request seq unlock; W2 completes", flush=True)
    with tempfile.TemporaryDirectory(prefix="welyos-irql-publish-") as directory:
        tmp = Path(directory)
        (tmp / "instrumented.c").write_text(instrumented, encoding="utf-8")
        (tmp / "cpu.h").write_text(
            "#define MAX_CPUS 8\n"
            "struct cpu { int id; };\n"
            "static inline struct cpu *this_cpu(void) { static struct cpu c; return &c; }\n"
            "static inline void arch_cpu_relax(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }\n",
            encoding="utf-8",
        )
        (tmp / "harness.c").write_text(HARNESS, encoding="utf-8")
        binary = tmp / "probe"
        build = subprocess.run(
            ["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-DREVIEW_EXPECT_FIXED=" + ("1" if already_fixed or args.candidate_fix else "0"),
             "-I", str(tmp), "-I", str(root / "include"), str(tmp / "harness.c"), "-o", str(binary)],
            capture_output=True, text=True, timeout=30,
        )
        if build.returncode:
            print("ERROR: actual-source harness failed to compile\n" + build.stderr)
            return 2
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="")
        return run.returncode if run.returncode in (0, 1) else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.TimeoutExpired) as error:
        print("ERROR:", error)
        raise SystemExit(2)
