# b2 fixed milestone independent acceptance evidence

At review all nine source hashes and four boot artifact hashes matched
build/milestones/c32-b2/manifest.txt, including the actual AArch64 .bin.
All three existing QEMU log hash prefixes matched. regress-b2.txt full SHA-256:
`f60b3be09c15ea5a5b81c6ac9bef49add0c93baaf1da07dfae3dadd8c8ae6ea4`;
its final line reports all expected results.

## Independent QEMU execution

Ran each fixed artifact with four CPUs, no disk or network, `autoexit selftest`.
Windows QEMU required approved execution outside the WSL sandbox. All three
returned 0, with thread KOBJ, anonymous ticket, fork ticket, user reclamation and
emulator exit-0 markers. All boot hashes remained unchanged. Evidence directory:
`/tmp/welyos-b2-independent-8r2fvwxr` (results.json and full logs).

| Architecture | Actual boot SHA-256 | Independent log SHA-256 |
|---|---|---|
| i386 | 937b5a2653f2fd61d77209765652b55d39c9c920e6433af865793e20e4a33f58 | 658ebb5c2c591dd392f931fcae77b8a6acdcff86f868d3e4259fd3d70e60bff8 |
| riscv64 | b8d7ba5d752a13bd684c15ac8d78f8ebbecdeb37c00cf688562a7454e7d77f31 | c973fac0581019bec679035248a0a4c30063c1eb3049efed6a0b5ce8c9bb7d16 |
| aarch64 .bin | fb4b7c0e656f73d24bc346d7d3c89b21a8532bdbd86b6a912651a86ab69f8aeb | 335c1bab03bd2e054304473cb7eee5c6c8c744d67cc1100698daef8f4cbc4820 |

Repeated host probe_thread_budget.py: 3,840 cases and three negative controls
passed, unchanged source; evidence `/tmp/welyos-thread-budget-cin3zpq4`.

## Test strengthening and unresolved scope

thread_kobj_test exercises prepare/abandon, insufficient quota, actual start,
join and sched_trim, and verifies quota/ref restoration. Its first assertion is
only k>=5. A guard-page undercharge from 6 to 5 still satisfies that assertion;
the later refusal uses the measured k-1, so it cannot independently detect the
same undercharge. The host probe catches this mutation, but the QEMU test should
also independently derive the exact expected cost from five stack pages plus
ceil(kmalloc_size(t)/PAGE_SIZE), tied explicitly to the stack configuration.
This is a test-strength gap, not a demonstrated product undercharge.

Only this milestone's exercised behavior is accepted. ustart/fork_start/frame,
file/pipe KOBJ, stack/image reservations, later fork allocation failure points,
guard restoration under exhaustion and full SMP lifetime stress remain open.
An i386 pass of this newer b2 binary does not retroactively supply the missing
independent step45 i386 run. No product edits or commit/push by this review;
all QEMU runs completed with no process left running.
