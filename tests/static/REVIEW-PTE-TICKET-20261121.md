# C3.2(b1) step 1: independent PTE codec verification

The current production codecs passed 3,072 host round trips and rejection of all
four invalid software-state combinations on each architecture/mode. This accepts
the software encoding step only; VM accounting and end-to-end correctness remain
unverified by this probe.

Run `python3 tests/static/probe_pte_ticket.py`. Evidence from this run is in
`/tmp/welyos-pte-ticket-rheux0d4/results.json`, including SHA-256 input hashes and
`changed_inputs=[]`. The probe extracts the production leaf/address/flags function
bodies, PTE constants, and shared software codec functions without modifying them.
It executes no privileged instructions and does not emulate an MMU.

Coverage: riscv64 and aarch64 512 cases each, i386 2,048 cases including all
NX/PGE combinations. Cases combine four physical addresses (zero through the
32-bit boundary), normal and large leaf levels, 16 permission/device combinations,
and all four software states. Expected states and raw PTE software-bit values use
an independent numeric oracle, so a paired encode/decode bug cannot hide behind a
round trip. i386 without NX explicitly expects executable mappings; RISC-V and
i386 do not preserve VM_DEVICE. Large-page inputs are aligned.

All invalid software states are checked both through vm_sw_flags_ok and by actually
calling arch_pte_leaf with a host panic/longjmp fixture. Three isolated mutations
per architecture are caught: decode state 3 as SHARED|COW, drop TICKET during
encoding, and omit leaf validation. All nine negative controls failed as expected.
Host execution uses UBSan with recovery disabled; no diagnostic was emitted.

Limits: this is extracted-function host execution, not whole translation-unit or
target-machine execution. It does not test live TLBs, atomic PTE publication,
free/unmap ownership, PMM reservation consumption, fork rollback, COW interleavings,
or ticket returns. Those remain required in the VM integration stages. High
physical-address bits above 32 are outside this case set. Existing architecture
selftests and target regressions reported by Claude are separate evidence, not
re-run here. No product source was edited, no commit/push performed.
