# Shell wait follow-up, 2026-11-18

Current source independently retested with the existing host models:

- `probe_stdio.py`: 105/105 PASS, changed_inputs empty. The previous cat read/write failures no longer reproduce.
- `probe_shell_cleanup.py`: previous seven cases all PASS, including one interrupted wait, denied resume cleanup, and closed stdout cleanup.
- `test_checks.py`: 24 tests PASS.

## Still open: failed wait is treated as a completed child

`user/sh.c`, `job_wait`, the `w != j->pids[i]` branch after the bounded wait loop:
setting `st = 1` and calling `job_mark_done` discards the child even though no successful wait occurred.

Two new deterministic cases reproduce this on the extracted current function:

| Injected result | Calls | Reaped | Job left | PID retained | Return |
| --- | ---: | ---: | ---: | ---: | ---: |
| `-E_INTR` on every wait | 1000 | 0 | 0 | -1 | 0 |
| `-E_IO` on first wait | 1 | 0 | 0 | -1 | 0 |

Expected: bounded execution, foreground restored, an error reported, and the still-unreaped PID retained for later recovery. An interrupted or otherwise failed wait cannot establish child completion. The `-E_IO` case is defensive error injection, not a claim that the current kernel emits this error. Repeated `-E_INTR` exercises the implemented retry limit directly.

Fix must consider `run` and foreground/background job callers as well as `job_wait`: simply returning nonzero without retaining the job can lose the local `cur` state, and marking a wait failure as a stopped child invents a stop event. Distinguish wait error from stopped/completed state and preserve ownership of unreaped children.

Artifacts: `/tmp/welyos-shell-cleanup-63ij6fqj/results.json`, changed_inputs empty; stdio `/tmp/welyos-stdio-ioxkw8ls/results.json`.
Fixtures compile with `-Wall -Wextra -Werror` and UBSan. This is a deterministic host model, without real processes, QEMU, or scheduler coverage.

Only the shell fixture, its runner, and this report were changed in this follow-up. Product code remains with its existing owner. Full pipeline redirection matrix, WFS fault injection, and a fresh GCC analyzer run remain outstanding.

Whole-tree `git diff --check` reports a trailing blank line in `docs/DEVLOG.md:7062`, outside this follow-up's files. No edits made to that file.
