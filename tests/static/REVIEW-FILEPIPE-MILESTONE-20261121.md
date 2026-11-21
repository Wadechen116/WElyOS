# File/pipe immutable milestone: independent verification

Claude message 172's `build/milestones/c32-b2-filepipe/manifest.txt` was
checked independently. All nine source hashes and four boot artifact hashes
matched. Three original QEMU log hash prefixes matched, as did regress-fp.txt:
`9457e85c8371a8d7c99bafec2bdd4da79e959f8a3e4f1505319cc6ec7b1bd8b9`.
The regression tail reports all expected results. This checks existing evidence;
the full regression was not independently rerun.

## Actual independent guest executions

Ran the immutable copies on i386, RISC-V and AArch64 with four CPUs, no disk,
no network and `autoexit selftest`. AArch64 actually booted the manifest's .bin.
All three returned zero and emitted pipe/file creator-lifetime, thread KOBJ,
anonymous fork ticket and user memory reclamation markers, followed by guest
`exit: code 0 to the emulator`. Boot artifacts remained unchanged.

Evidence: `/tmp/welyos-filepipe-independent-6kqyp9s6/` contains hashes.json,
results.json and full logs. Runner: `/tmp/welyos-filepipe-verify.py`.

| Architecture | Independent log SHA-256 |
| --- | --- |
| i386 | 1669e834332ed9d29ccc4f7d45cdbda19f4d807968e6ed8b2a197f5110803e58 |
| riscv64 | 28485849d3768041c539ee6c55b70e657c91684f27b3d0db53b64a70f0fc53ec |
| aarch64 | 14355753375f369a32b8026f032eb037e457b3adc43da84453e3e2cd7dbf05a2 |

Initial sandbox launch failed with WSL UtilBindVsockAnyPort; its wrapper rc=0
had no guest output and was rejected, not counted as a passing execution.
The above runs used approved Windows QEMU execution outside the sandbox and
all completed normally.

## Scope and remaining gaps

This verifies the fixed milestone's exercised creator-exits-before-last-close
behavior. It does not close every allocation failure path or actual concurrent
last-close scenario. The separate host review covers additional fixture cases.

`pipe_kobj_test` checks KOBJ >= 3 with three object references, although the
observed cost is four pages. This can miss an undercharge while still verifying
lifetime balance. Like the earlier thread test's k>=5, strengthen it with an
independently derived exact allocation cost, including the actual pipe storage.
This is a test-strength gap, not evidence of a current product undercharge.

Stack partial-map rollback, post-stack preparation failure, slot ownership,
ustart/fork_start/frame accounting, stack/image reservations, remaining fork
fault injection and ramfs accounting remain open. See the stack status review;
this milestone's passing QEMU tests do not close those findings.

No product changes, baseline edits, commit or push were performed by this review.
