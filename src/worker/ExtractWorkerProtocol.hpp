#pragma once
#include <cstdint>
namespace UltraArch::ExtractWorkerProtocol {
inline constexpr char magic[] = "UAI1";
inline constexpr uint32_t schema = 1;
inline constexpr uint32_t maxRequestBytes = 65536;
inline constexpr uint32_t maxReplyBytes = 65536;
inline constexpr uint64_t maxCapacityBytes = 512ull * 1048576;
inline constexpr uint64_t maxGroupBytes = 256ull * 1048576;
inline constexpr uint64_t maxEntries = 1000000;
inline constexpr uint64_t maxManifestBytes = 64ull * 1048576;
// One request: schema,dll,archive,password,mappingName,capacityBytes,root,
// manifestPath,groupLimitBytes,totalLimitBytes,fileLimitBytes,fileCountLimit.
// RAM mapping belongs to parent. Only ordinary 7z at offset0 initially supported.
// Manifest schema1 files[{relative,size,sha256,storage,offset,attributes,mtime,ctime,atime}],
// directories[relative],ramBytes,diskBytes. Times are uint64 FILETIME ticks;0 means absent.
// Reply: schema,success,fallback,error,manifestSha256,ramBytes,diskBytes.
// Unsupported format fallback occurs before any output writes. Corruption is hard failure.
}
