#pragma once
#include "ContentAnalyzer.hpp"
#include <atomic>
#include <functional>
namespace UltraArch {
struct InputManifest {
    static constexpr int schema =
        3; // SHA-256, canonical relative paths; old FNV sessions incompatible.
    std::filesystem::path root;
    AnalysisSummary summary;
    std::string fingerprint;
    uint64_t bytesRead = 0;
    static InputManifest scan(const std::filesystem::path &input,
                              const std::atomic<bool> *cancel = nullptr,
                              std::function<void(uint64_t, uint64_t)> progress = {}, bool similarity = false);
    static InputManifest fromEntries(const std::filesystem::path &root,
        std::vector<FileTypeInfo> files, std::vector<std::filesystem::path> emptyDirectories);
};
} // namespace UltraArch
