#!/usr/bin/env python3
"""直接編譯 kernel backend，並確認兩個完整性修補的突變負對照必定失敗。

只在暫存副本移除 rejected increment 或 seq-odd pending；正式原碼不變。
負對照必須在指定斷言以 SIGABRT 結束，其他失敗不能算成功抓到突變。
"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
harness = r'''
#include <assert.h>
#include <stdio.h>
#include "backend.c"
int main(void) {
    struct irq_late_snap a, b;
    uint32_t first, count;
    struct irq_late_in in = {.kind=IRQL_KIND_GAP, .src_id=1, .entry=100};
    irq_late_config(0, 1, IRQL_KIND_GAP, 10);
    irq_late_note(&in);
    irq_late_snapshot(0, &a);
    assert(a.ok && !a.pending);
    /* 精確模擬 config 取得互斥、尚未發布新 gen 的暫停點。 */
    uint32_t seq = rq[0].seq;
    ST(&rq[0].seq, seq + 1);
    irq_late_snapshot(0, &b);
    assert(!b.ok || b.pending);
    ST(&rq[0].seq, seq + 2);
    in.src_id=2; in.entry=200;
    irq_late_note(&in);
    in.src_id=1; in.entry=300;
    irq_late_note(&in);
    irq_late_snapshot(0, &b);
    assert(b.ok && !b.pending && a.gen == b.gen);
    assert(b.rejected == a.rejected + 1);
    assert(irq_late_check(&a, &b, 250, &first, &count) & IRQL_INC_REJECTED);
    a=b; in.entry=400; irq_late_note(&in); irq_late_snapshot(0, &b);
    assert(irq_late_check(&a, &b, 350, &first, &count) == 0);
    puts("PASS: writer-before-gen pending, rejected-input evidence, clean subsequent window");
}
'''

def main() -> None:
    # 預期的 assert abort 不得留下 core dump；子程序繼承此限制。
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (root / "kernel/irq_late.c").read_text(encoding="utf-8")
    mutations = (
        ("without_rejected_increment", "ST(&t->rejected, t->rejected + 1);", "",
         "b.rejected == a.rejected + 1"),
        ("without_seq_odd_pending", "(rs & 1u) || ", "",
         "!b.ok || b.pending"),
    )
    variants = [("actual_backend", source, None)]
    for name, old, new, assertion in mutations:
        if source.count(old) != 1:
            raise RuntimeError(f"NOT_APPLICABLE: {name}: expected exactly one reviewed mutation site")
        variants.append((name, source.replace(old, new, 1), assertion))

    with tempfile.TemporaryDirectory(prefix="welyos-irql-backend-") as directory:
        tmp = Path(directory)
        (tmp / "cpu.h").write_text(
            "#define MAX_CPUS 8\nstruct cpu { int id; };\n"
            "static inline struct cpu *this_cpu(void) { static struct cpu c; return &c; }\n"
            "static inline void arch_cpu_relax(void) {}\n", encoding="utf-8")
        (tmp / "test.c").write_text(harness, encoding="utf-8")
        for name, backend, assertion in variants:
            (tmp / "backend.c").write_text(backend, encoding="utf-8")
            subprocess.run(
                ["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-I", str(tmp), "-I", str(root / "include"),
                 str(tmp / "test.c"), "-o", str(tmp / "probe")],
                check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(tmp / "probe")], cwd=tmp,
                                    capture_output=True, text=True, timeout=10)
            if assertion is None:
                if result.returncode:
                    raise RuntimeError(f"FAIL actual_backend: {result.stderr}")
                print(result.stdout, end="")
            elif result.returncode == -signal.SIGABRT and assertion in result.stderr:
                print(f"PASS: {name} rejected by expected assertion: {assertion}")
            else:
                raise RuntimeError(
                    f"FAIL {name}: wanted SIGABRT at {assertion!r}, "
                    f"got exit={result.returncode}: {result.stdout}{result.stderr}")


if __name__ == "__main__":
    main()
