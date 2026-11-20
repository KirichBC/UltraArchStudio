# Short Disk/RAM comparison on Windows

Run `tools/ram-benchmark.ps1` from PowerShell 7 after the Release build finishes. It uses the native GUI executable's CLI mode and verified tools from the same package. No parallel builds or other compressors should run during measurement.

```powershell
pwsh -NoProfile -File tools/ram-benchmark.ps1 `
  -Exe 'C:\path\portable\UltraArchStudio.exe' `
  -ToolsBin 'C:\path\portable\bin' `
  -Destination 'F:\UltraArch-RAM-comparison-new'
```

The destination must not exist. The script creates repeatable text and mixed text/noise fixtures plus native x64/x86 `cmd.exe` copies, which are archived as data and never executed. Source hashes and file/directory manifests are frozen before measurement. Each case gets three sequential Disk/RAM pairs, alternating mode order. `-Repeats 1` is available for a smoke run. It never deletes existing files or cleans its output.

Both modes use Fast, DeepSearch, no grouping, a 4 MiB maximum dictionary, two workers, four CPU threads, an 8192 MiB overall budget, an 8 MiB RAM group threshold and a 512 MiB temporary RAM sub-budget. RAM mode additionally enables `--ram-candidates`. Input archives and all parameters except the storage switch remain the same. Each run has a fresh session/model/cache directory. OS cache is retained: this is a warm-cache-oriented experiment, and the first pair may be colder. Alternating order reduces but does not eliminate bias.

Every successful result is independently extracted with ordinary 7-Zip. Complete file path sets, sizes, per-file SHA-256 and directory sets must match the original. This verification time is recorded separately from optimizer wall time. Output size differences appear in `pairs.json`; equal sizes alone do not prove equal content, so independent verification is mandatory.

`measurements.json` and CSV include wall time, output bytes, logical candidate/probe artifact bytes, probe input materialization, RAM candidate/probe counts, hybrid input bytes, CPU counters and available memory statistics. Full application reports, arguments, stdout/stderr, archives and independent restored trees remain beside each measurement. There is no many-hour full-input benchmark in this script.

Memory figures require careful interpretation: the parent Working Set can be unavailable after exit and excludes workers; the maximum candidate-reported peak is the existing application/process accounting scope and is not a simultaneous parent-plus-child-plus-shared-mapping measurement. Do not add those numbers and call the result physical RAM usage.

Logical temporary writes exclude final output, metadata, checkpoints, capability IPC, unrelated system activity, filesystem cache and paging. They cannot establish physical SSD writes or drive wear. `-CollectSystemCounters` optionally records whole-system PhysicalDisk write and memory page-output **rates** before/after each run. Those point samples are not cumulative bytes or process-attributed measurements; localized Windows counter names may make them unavailable. A separate sustained physical-disk trace is required to confirm SSD traffic reduction. RAM-backed mappings may still be paged.

These fixtures and three repeats can validate local behavior and detect regressions. They cannot establish universal speed/compression superiority or predict NAND wear. Preserve the result JSON and EXE/DLL hashes when comparing later builds.
