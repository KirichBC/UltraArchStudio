#include "PreprocessorPipeline.hpp"
#include "ContentAnalyzer.hpp"
#include "ProcessManager.hpp"
#include "ResourceValidator.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace UltraArch {

namespace fs = std::filesystem;

static uint64_t calculateDirSize(const fs::path &dir) {
    uint64_t size = 0;
    if (fs::exists(dir) && fs::is_directory(dir)) {
        for (const auto &entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                size += entry.file_size();
            }
        }
    } else if (fs::exists(dir) && fs::is_regular_file(dir)) {
        size = fs::file_size(dir);
    }
    return size;
}

static std::string toLower(const std::string &str) {
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowerStr;
}

void PreprocessorPipeline::detectAvailableTools(const fs::path &toolsBinDir) {
    m_steps = {
        {"Leanify", "leanify", {"-i", "10"}, {}, {".png", ".jpg", ".jpeg", ".ico", ".gif", ".swf"}},
        {"ECT", "ect", {"-9", "--mt-deflate"}, {}, {".png", ".zip", ".gz"}},
    };

    m_availableSteps.clear();
    for (const auto &step : m_steps) {
        fs::path exePath = ProcessManager::resolveExecutable(step.executableName, toolsBinDir);
        if (!exePath.empty() && fs::exists(exePath)) {
            m_availableSteps.push_back(step);
        }
    }
}

bool PreprocessorPipeline::hasAvailableTools() const {
    return !m_availableSteps.empty();
}

std::vector<std::string> PreprocessorPipeline::getAvailableToolNames() const {
    std::vector<std::string> names;
    for (const auto &step : m_availableSteps) {
        names.push_back(step.name);
    }
    return names;
}

PreprocessResult PreprocessorPipeline::execute(const fs::path &inputDir, const fs::path &tempDir,
                                               const fs::path &toolsBinDir,
                                               const std::atomic<bool> *cancelToken,
                                               std::function<void(const std::string &)> logCallback,
                                               ResourceGuard *guard, uint32_t timeoutSeconds) {
    PreprocessResult result;

    if (!fs::exists(tempDir)) {
        fs::create_directories(tempDir);
    }

    // Materialize one overlay for CLI 7-Zip: unchanged files may be linked read-only in practice;
    // every tool receives a separate private file, never a link to the original.
    auto root = fs::is_directory(inputDir) ? inputDir : inputDir.parent_path();
    ContentAnalyzer analyzer;
    auto summary = analyzer.analyze(inputDir);
    for (const auto &dir : summary.emptyDirectories)
        fs::create_directories(tempDir / dir.lexically_relative(root));
    for (const auto &file : summary.files) {
        if (cancelToken && cancelToken->load())
            throw std::runtime_error("Cancelled");
        auto relative = file.path.lexically_relative(root);
        auto destination = tempDir / relative;
        fs::create_directories(destination.parent_path());
        std::error_code ec;
        fs::create_hard_link(file.path, destination, ec);
        if (ec)
            fs::copy_file(file.path, destination);
        for (const auto &step : m_availableSteps) {
            if (!shouldProcessFile(step, destination))
                continue;
            if (cancelToken && cancelToken->load())
                throw std::runtime_error("Cancelled");
            auto privateDir = tempDir / (".preprocess-" + uniqueId());
            fs::create_directory(privateDir);
            auto proposed = privateDir / destination.filename();
            fs::copy_file(destination, proposed);
            PreprocessStats stat;
            stat.stepName = step.name + ": " + pathUtf8(relative);
            stat.inputBytes = fs::file_size(destination);
            auto start = std::chrono::steady_clock::now();
            auto args = step.defaultArgs;
            args.push_back(pathUtf8(proposed));
            ProcessControl control{nullptr, timeoutSeconds};
            auto process = ProcessManager::execute(
                pathUtf8(ProcessManager::resolveExecutable(step.executableName, toolsBinDir)), args,
                privateDir, guard, 0, cancelToken, logCallback, &control);
            stat.outputBytes = fs::exists(proposed) ? fs::file_size(proposed) : 0;
            stat.success = process.success && stat.outputBytes < stat.inputBytes &&
                           ResourceValidator::equivalent(destination, proposed);
            if (stat.success) {
                // Compare final LZMA2 cost, not merely the uncompressed resource size.
                auto tool = ProcessManager::resolveExecutable("7z", toolsBinDir);
                auto compressed = [&](const fs::path &source, const std::string &name) {
                    auto archive = privateDir / name;
                    auto r = ProcessManager::execute(pathUtf8(tool),
                                                     {"a", "-y", "-t7z", "-m0=lzma2:d1m", "-mmt=1",
                                                      pathUtf8(archive), pathUtf8(source)},
                                                     privateDir, guard, 0, cancelToken, nullptr,
                                                     &control);
                    return r.success ? fs::file_size(archive) : UINT64_MAX;
                };
                // Same filename inside both archives keeps header overhead comparable.
                auto before = privateDir / "before";
                fs::create_directory(before);
                fs::copy_file(destination, before / destination.filename());
                auto oldSize = compressed(before / destination.filename(), "old.7z");
                auto newSize = compressed(proposed, "new.7z");
                stat.success = oldSize != UINT64_MAX && newSize < oldSize;
            }
            if (stat.success) {
                if (!atomicReplace(proposed, destination, ec))
                    throw fs::filesystem_error("Cannot accept preprocessed resource", destination,
                                               ec);
                result.changedFiles.push_back(relative);
            } else
                stat.errorMessage = "Change discarded: tool, decoded-pixel validation or "
                                    "compression benefit failed";
            stat.durationSeconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            result.steps.push_back(stat);
            if (logCallback)
                logCallback("[Preprocess] " + stat.stepName + ": " +
                            (stat.success ? "accepted" : "discarded"));
            fs::remove_all(privateDir);
        }
    }
    result.outputPath = tempDir;
    return result;
}

bool PreprocessorPipeline::shouldProcessFile(const StepConfig &step, const fs::path &file) {
    if (step.applicableExtensions.empty()) {
        return true;
    }

    std::string ext = toLower(pathUtf8(file.extension()));
    return std::find(step.applicableExtensions.begin(), step.applicableExtensions.end(), ext) !=
           step.applicableExtensions.end();
}

PreprocessStats
PreprocessorPipeline::executeStep(const StepConfig &step, const fs::path &inputDir,
                                  const fs::path &tempDir, const fs::path &toolsBinDir,
                                  const std::atomic<bool> *cancelToken,
                                  std::function<void(const std::string &)> logCallback,
                                  ResourceGuard *guard, uint32_t timeoutSeconds) {
    PreprocessStats stats;
    stats.stepName = step.name;
    stats.inputBytes = calculateDirSize(inputDir);

    if (logCallback) {
        logCallback("Running preprocessor step: " + step.name);
    }

    fs::path exePath = ProcessManager::resolveExecutable(step.executableName, toolsBinDir);
    if (exePath.empty()) {
        stats.success = false;
        stats.errorMessage = "Executable not found: " + step.executableName;
        return stats;
    }

    auto startTime = std::chrono::steady_clock::now();

    ProcessControl control{nullptr, timeoutSeconds};
    bool stepSuccess = true;

    {
        // Leanify / ECT
        for (const auto &entry : fs::recursive_directory_iterator(inputDir)) {
            if (cancelToken && cancelToken->load())
                break;

            if (entry.is_regular_file() && shouldProcessFile(step, entry.path())) {
                std::vector<std::string> args = step.defaultArgs;
                args.push_back(pathUtf8(entry.path()));

                auto result = ProcessManager::execute(pathUtf8(exePath), args, tempDir, guard, 0,
                                                      cancelToken, logCallback, &control);

                if (!result.success) {
                    stepSuccess = false;
                    stats.errorMessage =
                        "Tool execution failed for " + pathUtf8(entry.path().filename());
                }
            }
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = endTime - startTime;

    stats.durationSeconds = diff.count();
    stats.outputBytes =
        calculateDirSize(inputDir); // The tools operate in-place in the copied temp dir
    stats.success = stepSuccess && !(cancelToken && cancelToken->load()) &&
                    stats.outputBytes <= stats.inputBytes;
    if (stepSuccess && stats.outputBytes > stats.inputBytes)
        stats.errorMessage = "Preprocessing increased input size; changes discarded";

    return stats;
}

} // namespace UltraArch
