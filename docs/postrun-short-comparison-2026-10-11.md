# Short pinned postrun comparison

Final corrective application: 2026-10-12. Three new sequential runs reuse the unchanged fixtures and pinned old-build measurements from 2026-10-11; the old cases were not repeated. Inputs and backend EXE/DLL hashes match between each pair. All six paired outputs have independent extraction, exact name/directory-set checks and per-file SHA-256 verification. This is quality/control evidence, not a statistical speedup result.

| Fixture | Old seconds | Final seconds | Old bytes | Final bytes | Old logical temporary bytes | Final logical temporary bytes |
|---|---:|---:|---:|---:|---:|---:|
| text | 14.88 | 16.49 | 728 | 728 | 3786295 | 10821665 |
| incompressible | 18.82 | 45.04 | 3145858 | 3145858 | 109626626 | 1351932594 |
| distant-repeat | 22.26 | 43.68 | 3146787 | 3146745 | 115861769 | 792174253 |

Text and incompressible output sizes match. Distant repeats improve by 42 bytes. New defaults take longer and produce more logical temporary sample bytes on this corpus. The visible sample limit permits larger samples than the legacy small percentage, and alternate deterministic samples expand coverage. These measurements do not establish faster operation or lower physical SSD writes.

The 25 MiB sample and 60-second tournament defaults are retained as a quality-oriented bounded choice, not a measured speed optimum. Users can reduce the visible budget or use RAM samples when suitable. The final Photoshop control uses an explicit 20-second sample budget, two full attempts per group and a 360-second search budget; it is a separate limited control.

Logical totals sum candidateFileBytes, probeFileBytes and probeInputFileBytes. They exclude extraction, checkpoint copies, final output, metadata, paging and cache. They are not physical SSD/NAND writes. Both sides of these cases use Disk samples.

Old application SHA-256: `9fd1fef32d97670791ae485cfaeb3426852224762798b380682ede3a9e591acb`.
Final application SHA-256: `d53d4560c6e975b77ab5ef83dfc7b26c84428f18d47e70cbd408aa02a8bd57a4`.

Final evidence: `validation/postrun-short-ab-final-20261012/summary.json` and its case folders. Reused old evidence: `validation/postrun-short-ab-20261011`. The earlier intermediate measurements remain available and are not silently replaced.

These are two UltraArch versions. There is no paired successful Photoshop run of Ultra7z, so no Photoshop superiority claim is supported.
