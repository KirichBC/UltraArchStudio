#pragma once

#include "ContentAnalyzer.hpp"
#include "ResourceGuard.hpp"
#include "WorkspaceManager.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace UltraArch {

class ArchiveExtractor {
  public:
    ArchiveExtractor() = default;

    // Detect if path is an archive by magic bytes
    static bool isArchive(const std::filesystem::path &path);

    // Get archive format name
    static std::string detectFormat(const std::filesystem::path &path);

    static AnalysisSummary preview(const std::filesystem::path &archive,
                                   const std::filesystem::path &tools,
                                   const std::atomic<bool> *cancel = nullptr,
                                   const std::string &password = "");

    // Extract archive to outputDir using 7z
    static bool extract(const std::filesystem::path &archivePath,
                        const std::filesystem::path &outputDir,
                        const std::filesystem::path &toolsBinDir,
                        const std::atomic<bool> *cancelToken,
                        std::function<void(const std::string &)> logCallback,
                        const std::string &password = "", ResourceGuard *guard = nullptr,
                        uint32_t timeoutSeconds = 0, ExtractionQuota quota = {});
};

} // namespace UltraArch
