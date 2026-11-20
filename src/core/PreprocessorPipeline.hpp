#pragma once

#include "ResourceGuard.hpp"
#include "Types.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace UltraArch {

struct PreprocessResult {
    std::filesystem::path outputPath;
    std::vector<PreprocessStats> steps;
    std::vector<std::filesystem::path> changedFiles;
};

class PreprocessorPipeline {
  public:
    PreprocessorPipeline() = default;

    struct StepConfig {
        std::string name;           // "Leanify", "ECT", "Precomp", "SREP"
        std::string executableName; // "leanify", "ect", "precomp", "srep"
        std::vector<std::string> defaultArgs;
        std::vector<FileCategory> applicableCategories; // empty = all
        std::vector<std::string> applicableExtensions;  // empty = all
    };

    // Detect which tools are available in toolsBinDir
    void detectAvailableTools(const std::filesystem::path &toolsBinDir);

    // Execute the full pipeline on inputDir, working in tempDir
    // Returns vector of per-step stats
    PreprocessResult execute(const std::filesystem::path &inputDir,
                             const std::filesystem::path &tempDir,
                             const std::filesystem::path &toolsBinDir,
                             const std::atomic<bool> *cancelToken,
                             std::function<void(const std::string &)> logCallback,
                             ResourceGuard *guard = nullptr, uint32_t timeoutSeconds = 0);

    bool hasAvailableTools() const;
    std::vector<std::string> getAvailableToolNames() const;

  private:
    std::vector<StepConfig> m_steps;
    std::vector<StepConfig> m_availableSteps;

    PreprocessStats executeStep(const StepConfig &step, const std::filesystem::path &inputDir,
                                const std::filesystem::path &tempDir,
                                const std::filesystem::path &toolsBinDir,
                                const std::atomic<bool> *cancelToken,
                                std::function<void(const std::string &)> logCallback,
                                ResourceGuard *guard = nullptr, uint32_t timeoutSeconds = 0);

    bool shouldProcessFile(const StepConfig &step, const std::filesystem::path &file);
};

} // namespace UltraArch
