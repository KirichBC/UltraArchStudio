#pragma once
#include "ContentAnalyzer.hpp"
#include <algorithm>
#include <cctype>
#include <map>

namespace UltraArch {
class DictionaryPolicy {
  public:
    static uint64_t streamBytes(const AnalysisSummary &summary, const std::string &solid) {
        // Preview/synthetic summaries may lack a complete inventory. Never infer a
        // smaller bound from incomplete metadata.
        if (solid == "on" || summary.files.size() != summary.totalFiles || summary.files.empty())
            return summary.totalBytes;
        uint64_t total = 0, largest = 0;
        std::map<std::string, uint64_t> extensions;
        for (const auto &file : summary.files) {
            if (file.sizeBytes > summary.totalBytes - std::min(total, summary.totalBytes))
                return summary.totalBytes;
            total += file.sizeBytes;
            largest = std::max(largest, file.sizeBytes);
            if (solid == "e") {
                auto extension = pathUtf8(file.path.extension());
                // std::filesystem treats a leading-dot filename as extensionless,
                // whereas 7-Zip groups the suffix after that dot with normal files.
                auto basename = pathUtf8(file.path.filename());
                if (!basename.empty() && basename.front() == '.')
                    return summary.totalBytes;
                // Backend Unicode case folding is not reproduced here.
                if (std::any_of(extension.begin(), extension.end(),
                                [](unsigned char c) { return c >= 128; }))
                    return summary.totalBytes;
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                extensions[extension] += file.sizeBytes;
            }
        }
        if (total != summary.totalBytes)
            return summary.totalBytes;
        if (solid == "off")
            return largest;
        if (solid == "e") {
            uint64_t bound = largest;
            for (const auto &[extension, bytes] : extensions)
                bound = std::max(bound, bytes);
            return bound;
        }
        if (validSolid(solid)) {
            auto amount = std::stoull(solid.substr(0, solid.size() - 1));
            auto multiplier = solid.back() == 'k' ? 1024ULL
                              : solid.back() == 'm' ? 1048576ULL : 1073741824ULL;
            // Pinned 7-Zip keeps a file larger than -ms in a single block.
            return std::min(summary.totalBytes, std::max(largest, amount * multiplier));
        }
        return summary.totalBytes;
    }
    static uint64_t limitBytes(const AnalysisSummary &summary, const OptimizerSettings &settings,
                               const std::string &solid) {
        auto bytes = std::max<uint64_t>(1024, (streamBytes(summary, solid) + 1023) / 1024 * 1024);
        auto ceiling = settings.maxLzmaDictionaryMB ? settings.maxLzmaDictionaryMB : 1536;
        return std::min(bytes, ceiling * 1048576);
    }
    static uint64_t decoderBytes(const CodecSpec &method) {
        if (method.codec == Codec::LZMA1 && method.dictionary.value >= 2ULL * 1048576)
            // Official LzmaEnc_WriteProperties aligns these dictionaries to MiB.
            return ((method.dictionary.value + 1048575) / 1048576) * 1048576;
        if (method.codec != Codec::LZMA2 && method.codec != Codec::LZMA1)
            return method.dictionary.value;
        // Below 2 MiB LZMA1 uses the same 2/3 * power-of-two steps as LZMA2,
        // starting at 4 KiB. Encoder history remains the requested value.
        for (uint32_t property = 0; property < 40; ++property) {
            uint64_t bytes = uint64_t(2 + (property & 1)) << (property / 2 + 11);
            if (bytes >= method.dictionary.value)
                return bytes;
        }
        return UINT32_MAX;
    }
    static void apply(CompressionSpec &spec, const AnalysisSummary &summary,
                      const OptimizerSettings &settings) {
        const auto bound = limitBytes(summary, settings, spec.solid);
        // BCJ2 output streams cannot exceed the complete input stream. Unknown
        // branch lengths retain this conservative bound instead of a guessed ratio.
        for (auto &method : spec.methods)
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2)
                method.dictionary.value = std::min(method.dictionary.value, bound);
    }
};
} // namespace UltraArch
