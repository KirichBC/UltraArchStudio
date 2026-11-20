# Search and CPU controls

Run the native Windows executable from PowerShell. Paths containing spaces must be quoted.

```powershell
.\UltraArchStudio.exe --cli --input 'F:\Test.7z' --output 'F:\Test.ultra.7z' --profile extreme --search balanced --cpu-auto --ram 26624 --probe-sample-mb 25 --probe-budget 120 --probe-timeout 30 --verifier dual --report 'F:\Test-report.json'
```

The example illustrates a configuration; it is not a measured optimal preset. A completed run and content verification are required to compare size or speed.

## CPU allocation

`--cpu-auto` enables automatic CPU allocation. The engine resolves the CPUs available to the process and selected affinity policy. The baseline is up to half that number of simultaneous tasks, with two CPU threads per task. Codec capabilities, available RAM, disk admission and safety limits can reduce concurrency. This does not promise constant full CPU utilization.

Without `--cpu-auto`, `--workers` selects the concurrent task target and `--cpu-threads` sets the total compression CPU thread budget. The default worker target is two. When automatic allocation is enabled, manually supplied values are preserved as requested settings but do not override the automatic policy.

`--affinity none`, `--affinity physical` and `--affinity sets` choose the scheduling policy. `--background` lowers child process priority; `--cpu-limit 25` additionally applies a 25% Windows Job CPU cap.

## Search modes and samples

| Option | Meaning |
| --- | --- |
| `--search fast` | A reduced search using preliminary samples. |
| `--search balanced` | Sample-based parameter selection followed by full-group finalists. |
| `--search full` or `--search exhaustive` | Full search without hidden sample pruning or search time budgets. This is the default. |
| `--probe-sample-mb 25` | Parameter-search sample size in MiB; accepted range 1–256. |
| `--probe-budget 120` | Sample tournament budget in seconds; accepted range 1–3600. |
| `--probe-timeout 30` | Timeout for an individual sample process in seconds; accepted range 1–600. |
| `--probe-passes 2` | Number of sample passes; accepted range 1–4. |
| `--full-attempt-limit 8` | Explicit limit on full-data compression attempts, excluding the original-archive candidate; accepted range 0–10000, with zero meaning unlimited. |

The sample size is independent of the archive's solid block size. `--solid on`, `--solid off` or `--solid 512m` controls solid compression. Sampling chooses parameter candidates; finalists still compress the full input or group. Sample timeouts are not a full-data candidate timeout.

`--adaptive` is a compatibility alias for Balanced. An explicit `--search` takes precedence regardless of its position relative to `--adaptive`. Repeated explicit `--search` options use the last value.

Full search honors limits the user explicitly supplies: `--timeout` limits each full process, and `--time-budget` stops further scheduling before assembling the best verified available result. Both default to zero, meaning unlimited. A limited or interrupted search must not be treated as an exhaustive result.

`--full-attempt-limit` is a budget per search: each group search and the whole-input search have their own counter. Original-archive preservation and group orchestration do not consume a compression attempt. The limit also applies to Full search when explicitly nonzero. The GUI exposes the same setting as “Explicit full-attempt limit (0=unlimited)”. The default zero does not add a hidden attempt budget.

## Evidence and validation

Use `--report` to preserve the redacted run report. `--verifier dual` requests worker and disk content verification. The original compatible archive remains a possible winner; an unchanged output does not establish an improvement. Logical temporary file totals are not physical SSD writes.

Invalid numeric syntax, overflow and out-of-range sample options fail with an error instead of silently clamping the requested CLI values.

The GUI's Sessions and recovery view measures saved workspace sizes after Refresh history without scanning directories every frame. Incomplete scans display a lower bound. Deleting a saved session requires selecting that session's deletion action and confirming the exact workspace; its saved group leaders and checkpoint are then discarded. Resume or export any results you need before deleting the session.

The GUI offers a temporary memory allowance only for an identified deferred candidate whose projected combined reservation fits within the original total limit plus 1024 MiB. Its `+N MiB` label is the allowance above that original limit, rather than an increment per click. The offer includes active reservations and persistent RAM input. Clicking rechecks the selected candidate, approved allowance, physical memory, system commit and Windows Job limits; changed conditions can reject the request. The visible offer itself does not reserve memory.

The visible sample MiB limit is authoritative. Samples use up to that many bytes, bounded by input size and whole-PE eligibility. Legacy `quickProbePercent` is retained for reading older settings but does not silently reduce this limit.
