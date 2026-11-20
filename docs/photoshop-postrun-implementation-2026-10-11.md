# Photoshop postrun implementation tracking

This document tracks implementation of `photoshop-postrun-plan-2026-10-11.md`. It is not a claim that all acceptance checks have passed.

## Implemented

- Extensionless archive grouping; strict unsafe-path preflight remains enabled.
- Scoped candidate/group skip, early complete assembly with baseline fallbacks, retained durable group leaders and resumable incomplete searches.
- Separate publication/search/stop outcomes; elapsed time, lifecycle events, bounded redacted process stdout/stderr, compression/verification/wait metrics.
- Shared leaf admission broker, checked reservations, bounded fairness, safe existing-Job memory updates, bounded isolated allocation retry.
- Structured sample results, deterministic alternate samples and identities, bounded inconclusive aliases, competitive expensive families, unchanged protected graph/context contracts and explicit Exhaustive.
- Lazy whole-input Store, separate unverified size-discard status, full content verification for potential leaders, fallback protection.
- CPU Auto and manual next dispatch, explicit one-GiB maximum allowance, actual commit/effective limit display.
- Visible sample/full-attempt budgets, hierarchy-aware progress, source-versus-uncompressed sizes, presets and session workspace diagnostics/explicit cleanup.
- Safe group assembly continues to re-encode subsequent groups. Elapsed assembly cost is reported. Compressed-block transplant is not claimed.

## Native verification and remaining acceptance

Final-source native MSVC checks:

- Debug: 11/11 suites passed on the final corrective source, 233.78 seconds (`validation/postrun-debug-final8-20261012.log` and XML).
- Release: 11/11 CTest suites passed before the final duplicate-CRC removal, 214.41 seconds (`validation/postrun-release-final7-20261012.log` and XML). The rebuilt final package then passed all eleven installed developer suites plus CLI/Dual; see the installed-package smoke below.
- Fault-injection publication recovery plus group checkpoint contracts: 2/2 passed, 0.75 seconds on final source (`validation/postrun-recovery-final8-20261012.log`). This is simulated interruption, not a physical power-loss test.
- CLI: four successful policy/budget cases and nine numeric rejection cases passed (`validation/postrun-cli-final-20261012/summary.json`). Explicit Full wins over legacy adaptive in either argument order.
- The suites include real native Job updates with a live allocated child, preserved CPU/kill-on-close flags, refusal to shrink below commit, nested Job/worker OOM, scoped skips, manual dispatch/override retirement, resume/early assembly, Unicode/long paths, password, SFX and volumes.
- Installed-package smoke passed all eleven developer executables plus CLI/Dual from a Unicode installation path and alternate CWD (`validation/postrun-package-smoke-final-20261012.log`). Retained evidence: `C:/Users/Kirich/AppData/Local/Temp/UltraArch-проверка-日本語-71817f15af3149aebd1ded37bd26e7a0`.
- Short pinned comparison: 6/6 independent SHA checks passed. Two output sizes match; distant repeats improve by 42 bytes. New default runs are slower and write more logical sample bytes on this corpus. See `docs/postrun-short-comparison-2026-10-11.md`; no speedup or physical SSD-write reduction is claimed.

Final Release package: `release/msvc-2026-10-12/photoshop-postrun-final`. Application SHA-256: `d53d4560c6e975b77ab5ef83dfc7b26c84428f18d47e70cbd408aa02a8bd57a4`. Tool identities are recorded in the package SHA256SUMS.txt. Earlier failed test logs are retained; final8 Debug/recovery and the 2026-10-12 installed-package log cover the final corrective source.

Long Unicode extended paths now use native separators in extended Windows paths. Native RAM/Disk roundtrip checks pass for extensionless files, dotfiles, Unicode, empty entries, attributes and paths longer than 260 characters. Resume and early-assembly regression tests exceed the grouping threshold.

Group archives use immutable generations. An atomic winner.json replacement is the commit point; interruption before that point retains the predecessor. CRC-only replacements retain the last content-verified fallback. Resume copies and checks both referenced archive identities before committing its destination marker. Tests cover interruption, replacement, fallback retention, unsafe names and damaged-byte rejection. A separate child publication path prevents resume from overwriting a committed generation.

The visible sample MiB limit is authoritative, rather than silently reduced by the legacy percentage. An explicit full-attempt budget already consumed by baseline attempts prevents an unnecessary sample tournament. A sample-pruned test does not require a losing full attempt to exist; the dedicated tiny-input case retains the zero-write loser contract.

The final bounded Photoshop control was stopped by its explicit external deadline after 1201.48 seconds. It did not publish a complete final archive. Full-input acceptance therefore remains unconfirmed. Resume data is retained; the former 62-run series was not restarted. Search quality and speed claims require measured evidence; historical results on another archive are not Photoshop controls.

Manual GUI/DPI 100/150/200 percent, multiple monitors, RU/EN layout, tray/drag-drop and user interactions remain separate acceptance checks. Windows 11, physical power loss/disk exhaustion on a dedicated volume, signing and external decoder interoperability are not confirmed by automated local suites.

## Corrective work after the first bounded control (2026-10-12)

The first full-input control used a 128 MiB explicit dictionary cap and a 120-second process timeout. The explicit cap disables compatible-original preservation by current policy; this was not a RAM-input normalization failure. Whole-input Store timed out. A large group's final disk extraction also timed out, the grouped attempt failed, and no successful complete candidate remained. The run ended Failed / NotPublished / Interrupted in 752.91 seconds. It did not create a successful final archive. Its report and retained workspace are under `F:/UltraArch-postrun-control-20261011`. Source archive identity is pinned in run-environment.json; it was never a deletion target.

That diagnostic control exposed an additional orchestration bottleneck. Groups reserved static executor ranges even while some local executors were idle. The correction removes those group reservations: bounded coordinators interleave expensive groups and cheap backfills; only actual leaf leases consume global RAM, CPU and disk resources. The actual leaf lease assigns a reusable global worker index for process affinity. Automatic disk concurrency now matches the advertised worker count rather than silently using 64.

The new deterministic native test blocks a large-group Store attempt until a small group's Store starts, while checking global reservations and full restored manifest/SHA. It also covers an exhausted full-attempt budget: queued group orchestration must still be admitted, because only leaf compression consumes that budget. Original preservation remains outside the compression-attempt budget.

A successful original-preservation execution already ran `7z t`. Its duplicate identical CRC pass was removed. The candidate still receives full streaming manifest/SHA verification, and final publication still receives mandatory verification.

The final corrective package is `release/msvc-2026-10-12/photoshop-postrun-final`; Debug, installed Release, recovery and CLI checks passed. The short comparison now contains three runs of this exact final executable and three reused, unchanged old-build cases, with matching input and backend identities.

## Final bounded Photoshop control (2026-10-12)

The final executable used the unchanged 7,084,572,804-byte Photoshop input, dictionary Auto, CPU Auto, a 26,624 MiB memory limit, RAM groups up to 40 MiB with a 1024 MiB temporary RAM budget, 25 MiB samples, a 20-second sample budget, two full compression attempts per group, a 360-second search budget and 300-second process timeouts. The external monitor had a separate 1200-second deadline. Search budgets do not bound analysis, checkpoint copies and final verification to the same wall-clock duration.

RAM extraction succeeded, the original archive participated as a verified fallback, whole-input Store was deferred, and small groups completed alongside heavy groups. Logs record local leaders of 429,406,856 bytes for `.dcp` and 4,893,810,299 bytes for `.data`; these are group results, not a complete output size. The run spent substantial time preserving and verifying large group artifacts. The monitor terminated its own process tree at 1201.48 seconds during group finalization. There is no final application report or `result.7z`, so no successful final publication, independent final archive check, compression gain or full search completion is claimed.

Peak tree committed memory was 6,732,869,632 bytes (about 6.27 GiB). Job accounting observed 153,954,808,348 logical read bytes and 52,052,088,308 logical write bytes across 1047 process lifetimes. These counters include the application tree and are not physical SSD writes. The source SHA-256 was checked again after termination and remained `7a67555014472888ea54d3e53faedd3124bf41f191662a5ca79006cbe0144b8b`.

Small evidence files and the explicit unsuccessful outcome are in `validation/postrun-photoshop-final-20261012/summary.json`. Large workspaces and group markers remain in `F:/UltraArch-postrun-control-final-20261012/scratch`. Marker metadata was collected without claiming a new post-termination hash verification of every group artifact. A future completion check should reuse valid checkpoints and allow a separately stated finalization window; another full compression series has not been started. Implementation is complete at the code level; full Photoshop completion and manual acceptance remain open.
