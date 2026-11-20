#pragma once
#include "Hasher.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace UltraArch {
inline bool validSolid(const std::string &value) {
    if (value == "on" || value == "off" || value == "e")
        return true;
    if (value.size() < 2 || value.size() > 8 || value.front() < '1' || value.front() > '9')
        return false;
    if (value.back() != 'k' && value.back() != 'm' && value.back() != 'g')
        return false;
    for (size_t i = 0; i + 1 < value.size(); ++i)
        if (value[i] < '0' || value[i] > '9')
            return false;
    return true;
}

struct Bytes {
    uint64_t value = 0;
};
struct MiB {
    uint64_t value = 0;
    Bytes bytes() const {
        return {value * 1048576};
    }
};
struct Seconds {
    uint32_t value = 0;
};
enum class Codec { Store, LZMA2, PPMd, BCJ, BCJ2, Delta, LZMA1, BZip2, Deflate };
struct CodecSpec {
    Codec codec = Codec::LZMA2;
    Bytes dictionary{1048576};
    uint32_t fastBytes = 273, matchCycles = 0, lc = 3, lp = 0, pb = 2, order = 8, delta = 1;
    std::string matchFinder;
};
struct CompressionSpec {
    static constexpr int schema = 2;
    std::vector<CodecSpec> methods;
    std::vector<std::string> bindings;
    std::string solid = "on";
    bool sortByType = false;
    uint32_t threads = 1;
    std::vector<std::string> compile() const;
    std::string id() const;
    uint64_t memoryMB(bool compress = true) const;
    std::string family() const;
    void validate() const;
};
} // namespace UltraArch
