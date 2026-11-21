# Steps 4-5 manifest and independent QEMU review

At initial inspection, all 11 source entries and all three ELF hashes matched
build/c32a/manifest-step45.txt. All six recorded log hash prefixes matched.
regress-s45b.txt full SHA-256 is
`da354504be82f579b2603dfcfe4fccba88a33802bc74348363a108e0c02105a8`
and its final line reports all expected results. HEAD was 93ce4a3.

## Independent execution

Copied binaries to an isolated Windows temporary directory, disabled disk and
network, and ran 4-CPU `autoexit selftest`. Initial sandbox attempts failed before
guest boot with WSL UtilBindVsockAnyPort socket errors. Do not interpret i386's
initial wrapper rc=0 as success: no guest test marker was present. After approved
sandbox escalation, RISC-V and AArch64 executions both returned 0, with successful
membudget, PTE, ledger, anonymous-ticket, fork-ticket, user reclamation and
emulator exit-0 markers. No shared disk/build files were written by these runs.

Evidence directory:
`/mnt/c/Users/user/AppData/Local/Temp/codex-welyos-step45-ybwz46df`

- riscv64-retry.log SHA-256:
  `45d0373de1643c6429389fdd7f98c477e81ba9f7388b0210ae464d0e290581fb`
- aarch64-retry.log SHA-256:
  `a437bbb98396426a6a75359629c878014a7b64ba832a8b60a1599f71b7cb98c9`

Both copied ELFs match the manifest. AArch64 actually boots welyos.bin; its
SHA-256 is `2ffdfb8f85887f81e5c7c60dd2494d3ca35ffde45198d9dabfc7689fc46576c5`.
The original manifest lists only ELF, so future manifests must include the actual
.bin as well. This rerun proves that copied .bin passes the recorded checks;
the old manifest alone does not bind it to its source.

Between initial verification and copying, Claude's next-stage build replaced the
i386 ELF. The copy is `1b345e61efef5bb98bc3d5b8a6d9df8723b0019ce6da9edd3e02b088dac2c3d7`,
not the step45 `4f601d10...` binary. Therefore no independent step45 i386 pass is
claimed. Retain all frozen target binaries before further builds to avoid this
race. Claude's manifest-matched historical i386 log remains available evidence.

## Remaining gaps

The QEMU fork test injects failure at the first child table quota charge and
checks parent writable flags, tickets and global reservations. It does not
exercise failure after each later child table allocation or at each root/heap/
frame/thread preparation step. Full failure-path coverage remains outstanding.
The host two-pass probe covers each substituted leaf walk failure, not actual
multi-level allocation cleanup. Stack reservations remain missing: i386 and
AArch64 memhog guest failures are real acceptance failures despite emulator
return 0. No overall I-240 closure is justified. Next-stage thread/KOBJ work
must retain these open requirements.

No product edits or commit/push by this review; no QEMU processes left running.
