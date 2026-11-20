#include "CompressionSpec.hpp"
#include "DictionaryPolicy.hpp"
#include <algorithm>
#include <stdexcept>
namespace UltraArch {
void CompressionSpec::validate() const {
    if (methods.empty() || methods.size() > 8 || threads < 1 || threads > 256)
        throw std::invalid_argument("Invalid codec graph/threads");
    if (!validSolid(solid))
        throw std::invalid_argument("Invalid solid size");
    for (const auto &m : methods) {
        if (static_cast<int>(m.codec) < 0 || static_cast<int>(m.codec) > 8)
            throw std::invalid_argument("Unknown codec");
        if ((m.codec == Codec::LZMA1 || m.codec == Codec::LZMA2 || m.codec == Codec::PPMd) &&
            (m.dictionary.value < 1024 || m.dictionary.value % 1024 != 0 ||
             m.dictionary.value > uint64_t(1536) * 1048576))
            throw std::invalid_argument("Dictionary outside supported range");
        if ((m.codec == Codec::LZMA1 || m.codec == Codec::LZMA2) &&
            (m.fastBytes < 5 || m.fastBytes > 273 || m.lc > (m.codec == Codec::LZMA1 ? 8u : 4u) || m.lp > 4 ||
             (m.codec == Codec::LZMA2 && m.lc + m.lp > 4) ||
             m.pb > 4 ||
             (m.matchFinder != "" && m.matchFinder != "bt2" && m.matchFinder != "bt3" &&
              m.matchFinder != "bt4" && m.matchFinder != "bt5" && m.matchFinder != "hc4")))
            throw std::invalid_argument("Invalid LZMA parameters");
        if (m.codec == Codec::PPMd && (m.order < 2 || m.order > 32))
            throw std::invalid_argument("Invalid PPMd order");
        if (m.codec == Codec::Delta && (m.delta < 1 || m.delta > 256))
            throw std::invalid_argument("Invalid Delta stride");
    }
    bool bcj2 = methods.front().codec == Codec::BCJ2;
    if (bcj2 && !((methods.size() == 5 &&
                  bindings == std::vector<std::string>{"0:1", "0s1:2", "0s2:3", "0s3:4"}) ||
                 (methods.size() == 4 &&
                  bindings == std::vector<std::string>{"0:1", "0s1:2", "0s2:3"}) ||
                 (methods.size() == 6 && methods[3].codec == Codec::Delta &&
                  bindings == std::vector<std::string>{"0s0:1", "0s1:2", "0s2:3", "0s3:5", "3s0:4"})))
        throw std::invalid_argument("BCJ2 requires four explicit output streams");
    if (!bcj2 && !bindings.empty())
        throw std::invalid_argument("Unexpected graph bindings");
}
std::vector<std::string> CompressionSpec::compile() const {
    validate();
    std::vector<std::string> args;
    if (methods.front().codec == Codec::Store)
        return {"-mx0", "-mmt=1", "-ms=" + solid};
    for (size_t i = 0; i < methods.size(); ++i) {
        const auto &m = methods[i];
        std::string value;
        switch (m.codec) {
        case Codec::Store:
            value = "Copy";
            break;
        case Codec::BCJ:
            value = "bcj";
            break;
        case Codec::BCJ2:
            value = "bcj2";
            break;
        case Codec::Delta:
            value = "delta:" + std::to_string(m.delta);
            break;
        case Codec::PPMd:
            value = "ppmd:mem" + std::to_string(m.dictionary.value / 1024) + "k:o" +
                    std::to_string(m.order);
            break;
        case Codec::LZMA2:
        case Codec::LZMA1:
            value = std::string(m.codec == Codec::LZMA1 ? "lzma:d" : "lzma2:d") + std::to_string(m.dictionary.value / 1024) + "k:fb" +
                    std::to_string(m.fastBytes);
            value += ":lc" + std::to_string(m.lc) + ":lp" + std::to_string(m.lp) + ":pb" +
                     std::to_string(m.pb);
            if (!m.matchFinder.empty())
                value += ":mf=" + m.matchFinder;
            if (m.matchCycles)
                value += ":mc" + std::to_string(m.matchCycles);
            break;
        case Codec::BZip2:
            value = "bzip2";
            break;
        case Codec::Deflate:
            value = "deflate";
            break;
        }
        args.push_back("-m" + std::to_string(i) + "=" + value);
    }
    for (const auto &b : bindings)
        args.push_back("-mb" + b);
    args.push_back("-ms=" + solid);
    args.push_back("-mx=9");
    args.push_back("-mmt=" + std::to_string(threads));
    if (sortByType)
        args.push_back("-mqs=on");
    return args;
}
std::string CompressionSpec::id() const {
    std::string canonical = "compression-spec-" + std::to_string(schema);
    for (const auto &flag : compile())
        canonical += '\n' + flag;
    return Hasher::text(canonical);
}
uint64_t CompressionSpec::memoryMB(bool compress) const {
    validate();
    if (methods.front().codec == Codec::Store)
        return 32;
    uint64_t result = 64;
    for (const auto &m : methods) {
        uint64_t mb = ((compress ? m.dictionary.value : DictionaryPolicy::decoderBytes(m)) + 1048575) / 1048576;
        if (m.codec == Codec::PPMd)
            result += mb;
        if (m.codec == Codec::LZMA2 || m.codec == Codec::LZMA1)
            result += compress ? mb * 12 * std::max(1u, (threads + 1) / 2) : mb + 16;
        if (m.codec == Codec::BZip2 || m.codec == Codec::Deflate)
            result += 64 * threads;
    }
    return result;
}
std::string CompressionSpec::family() const {
    switch (methods.front().codec) {
    case Codec::Store:
        return "Store";
    case Codec::LZMA2:
        return "LZMA2";
    case Codec::LZMA1:
        return "LZMA1";
    case Codec::BZip2:
        return "BZip2";
    case Codec::Deflate:
        return "Deflate";
    case Codec::PPMd:
        return "PPMd";
    case Codec::BCJ:
        return "BCJ";
    case Codec::BCJ2:
        return "BCJ2";
    case Codec::Delta:
        return "Delta";
    }
    throw std::logic_error("Unknown codec");
}
} // namespace UltraArch
