#pragma once

#include "ContentAnalyzer.hpp"
#include "StageMetrics.hpp"
#include "ProcessManager.hpp"
#include "ResourceGuard.hpp"
#include "Types.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace UltraArch {

struct AssemblyPlan {
    std::filesystem::path targetOutputPath;
    std::filesystem::path inputSourcePath;
    TaskConfig winningConfig;
    bool smartCategorized = true;
    bool createSfx = false;

    std::filesystem::path bestCandidateArchive; // Path to already-compressed best candidate
    bool reuseCandidate = false;                // If true, just rename instead of re-compressing
    std::string password;                       // Encryption
    bool encryptHeaders = false;
    uint64_t volumeSizeMB = 0;  // Multi-volume
    std::string solidBlockSize; // Solid block override
};

class ArchiveAssembler {
  public:
    ArchiveAssembler() = default;

    // Verifies an existing .7z archive using '7z t <archive>'
    static bool verifyArchive(const std::filesystem::path &archivePath,
                              const std::filesystem::path &toolsBinDir,
                              std::string &outErrorMessage, const std::string &password = "",
                              const std::atomic<bool> *cancelToken = nullptr,
                              ResourceGuard *guard = nullptr, uint32_t timeoutSeconds = 0,
                              const ProcessControl *control = nullptr);

    // Assembles the final unified .7z archive based on the winning test candidate
    static bool assembleFinalArchive(const AssemblyPlan &plan, const AnalysisSummary &summary,
                                     const OptimizerSettings &settings,
                                     ResourceGuard *resourceGuard,
                                     const std::atomic<bool> *cancelToken,
                                     std::function<void(const std::string &)> logCallback,
                                     std::function<void(float)> progressCallback,
                                     std::vector<std::filesystem::path> *outputFiles = nullptr,
                                     StageMetrics *metrics = nullptr);
};

} // namespace UltraArch
