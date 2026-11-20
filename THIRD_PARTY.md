# Third-party components

- nlohmann/json 3.12.0, MIT: https://github.com/nlohmann/json/releases/tag/v3.12.0
  Vendored header SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.
  License: `vendor/nlohmann/LICENSE.MIT`.
- Dear ImGui: https://github.com/ocornut/imgui (MIT; see `vendor/imgui/LICENSE.txt`).
- 7-Zip tools and SFX modules: https://www.7-zip.org/license.txt
- Leanify: https://github.com/JayXon/Leanify
- Efficient Compression Tool: https://github.com/fhanau/Efficient-Compression-Tool

The shipped binaries are pinned in `bin/tool-manifest.json`: 7-Zip 26.04, Leanify 0.4.3, ECT 0.9.5. The five 7-Zip files were updated on 2026-10-09 and compared byte-for-byte with the official 26.04 release assets. The unchanged ECT/Leanify files retain their previous provenance. URLs and asset SHA-256 are in the manifest. This verifies release provenance, not Authenticode. Windows signature inspection is recorded separately by the installed-package smoke script; see the Windows acceptance report for the observed signature statuses.

Upstream root notices are included in `licenses/`. ECT's upstream `licenses` bundle is copied to `licenses/ECT-dependencies`; Leanify root and embedded third-party copyright/license blocks are retained in `licenses/Leanify-dependencies`. These cover notices supplied in the checked release source trees; they are not a claim that the external binaries were rebuilt here. Retain all notices when replacing or repackaging tools.

The isolated verification worker uses a minimal subset of the official 7-Zip 26.04 SDK headers. Per-header hashes, upstream source archive identity and origin are recorded in `vendor/7zip/PROVENANCE.json`. The worker accepts only the pinned 7z.dll SHA-256 and loads it using Windows DLL-directory/system-directory search restrictions. No automatic downloads or updates are performed.

Legacy optional DLLs in the source-tree `bin/Codecs` directory are outside the pinned seven-file tool manifest and the supported compression matrix. They are excluded from the MSVC Runtime package.
