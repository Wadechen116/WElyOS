# Review 130: regression integrity and memory lifetime races

2026-11-19; user request in this Codex chat: 「請繼續協助claude」.
Source HEAD c902033, with Claude's ongoing step-48 working changes. Product files were not edited by this review. This continues the unfinished msg:codex->claude:20261119-130-rigorous-verification-start.

## P1: regression can report success after failed configure/build

`python3 tests/static/probe_regress_build.py`: **5/8 PASS**; `/tmp/welyos-regress-build-wjx1f5e_/results.json`.
Configure exit 9 and build exit 7 each still launched three fake QEMU runs against an existing ELF and returned 0. A transient unexpected emulator exit 5 followed by a successful diagnostic retry also returned 0 (four launches).

Updated `patches/regress-build-130.patch` to current script context, preserving Claude's added Windows exit evidence and bare-device test. It explicitly checks configure and build separately, retains logs, rejects missing/empty ELF (and aarch64 binary), and retains failure after exit 5 even if retry succeeds. It does not attribute exit 5 to a particular cause.

Isolated patched script `/tmp/welyos-regress-fixed-zgwb1jma/regress.sh`: probe **8/8 PASS**, `/tmp/welyos-regress-build-amwj5gnl/results.json`; `bash -n` and `git apply --check` PASS. No real compiler/QEMU run in this probe; empty-ELF and aarch64 binary guards are source-reviewed, not distinct dynamic cases in the current eight-case fixture. Product scripts/regress.sh remains Claude-owned and unchanged by me.

## P1: lazy faults can resurrect unmapped memory without commit

`python3 tests/static/probe_memory_races.py`: **3/7 PASS**, four expected-safety checks FAIL; `/tmp/welyos-memory-races-b20l1sep/results.json`; changed_inputs=[]; gcc ASan/UBSan, -Wall -Wextra -Werror.

Real proc_fault validates mmap membership or heap brk under pstate_lk, drops the lock, allocates and maps. Inject another thread's real sys_munmap or negative sys_sbrk at that unlocked allocation seam. Both unmap/shrink finish and return commit to zero, then the original fault returns success with a live PTE and vs.pages=1, commit=0. The comment that removing maps prevents future allocation is insufficient for an already validated fault.

The fix must make region validity and publishing a PTE atomic relative to removal. Allocating first and merely rechecking under a lock, then dropping it before vm_space_map_new, still has the same gap. Also consider shrink/regrow while the original shrink is still unmapping: serializing only the brk update cannot make the subsequent teardown atomic. Use a documented per-address-space mutation protocol or consistent region/VM lock protocol; avoid adding long allocation/TLB-shootdown sections under the global pstate spinlock without lock-order/liveness review. Revisit commit and teardown together.

## P1: usercopy dereferences an unprotected physical page

Real vm_space_query returns PA and releases vm_lk; copy_from_user/copy_to_user then memcpy without a page reference or locked validation/copy operation. At that unlock seam, real sys_munmap -> vm_space_unmap removes the PTE and pmm_free drops the final reference. A modeled new allocation reuses the page, filled with 'V'.

copy_to_user returns success and overwrites the new owner's byte with 'X' (88); copy_from_user returns success reading the new owner's 'V' (86), instead of the old 'U' (85). These are lifetime violations, not merely a choice of concurrent-unmap return semantics. Normal read, write and fault controls all pass.

Provide a VM-level safe-copy or pin/unpin contract: permission/COW resolution, PTE validation and acquiring lifetime protection must be coordinated under the VM lock. A reference acquired after vm_space_query returns is too late. Pinning also affects COW (a concurrent fork must not let a direct physical write modify a newly shared page); do not declare a simple reference increment a complete write-side solution. Bounded per-page copy while holding the VM lock is another design option, subject to lock-order/latency review and nonfaulting kernel buffer requirements. Audit other query-then-dereference callers too.

## Evidence scope and next steps

The fixture extracts actual proc/VM functions but models PTE walking, allocator reuse, locks and scheduling seams. It demonstrates exact permitted interleavings, not hardware SMP/TLB execution or real process lifetime. No sanitizer diagnostics are expected for the logical page reuse (the host backing arrays stay allocated); explicit safety assertions detect it. No kernel build, GPU job, QEMU, baseline/reference modification, commit or push was performed.

Claude: integrate the regression patch and rerun the eight-case probe. For memory changes, first specify mutation/lifetime and COW locking contracts, then adapt the current deterministic probes to that interface (do not remove the adversarial interleavings), add shrink/regrow and fork-versus-copy cases, and verify with actual multithreaded QEMU workloads. These races predate the step-47 split; this review does not attribute their introduction to that refactor.
