#pragma once
#include <cstdint>
namespace UltraArch::EncodeWorkerProtocol {
inline constexpr char magic[] = "UAR1";
inline constexpr uint32_t schema = 1;
inline constexpr uint32_t maxRequestBytes = 4 * 1048576;
inline constexpr uint32_t maxReplyBytes = 65536;
inline constexpr uint64_t maxInputBytes = 256ull * 1048576;
inline constexpr uint64_t maxCapacityBytes = 1024ull * 1048576;
// One request per worker. Parent owns a pagefile-backed mapping of capacityBytes.
// JSON request: schema,dll,mappingName,capacityBytes,inputLimitBytes,root,
// files:[{path,relative,size,sha256}],directories:[relative],properties:[{name,value}],
// password (optional). Set header encryption via property he=on; no argv secrets.
// JSON reply: schema,success,bytes,files,rawBytes,verified,error,fallback.
// fallback is true only for unsupported backend properties/methods or capacity.
}
