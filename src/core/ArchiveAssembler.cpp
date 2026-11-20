#include "ArchiveAssembler.hpp"
#include "ArchiveExtractor.hpp"
#include "FileUtils.hpp"
#include "PublicationTransaction.hpp"
#include "StreamingVerifier.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <iomanip>
#include <sstream>
namespace UltraArch {
namespace fs = std::filesystem;
bool ArchiveAssembler::verifyArchive(const fs::path &archive, const fs::path &tools,
                                     std::string &error, const std::string &password,
                                     const std::atomic<bool> *cancel, ResourceGuard *guard,
                                     uint32_t timeoutSeconds, const ProcessControl *control) {
    auto exe = ProcessManager::resolveExecutable("7z", tools);
    std::vector<std::string> args = {"t", "-y", "-mmt1", "-sccUTF-8"};
    if (!password.empty())
        args.push_back("-p" + password);
    args.push_back(pathUtf8(fs::absolute(archive)));
    ProcessControl fallback{nullptr, timeoutSeconds};
    auto r = ProcessManager::execute(pathUtf8(exe), args, archive.parent_path(), guard, 0, cancel,
                                     nullptr, control ? control : &fallback);
    if (!r.success) {
        error = r.errOutput.empty() ? "Archive test failed: " + std::to_string(r.exitCode)
                                    : r.errOutput;
        return false;
    }
    return true; // The tool's exit status is independent of language/version of its messages.
}
namespace {
bool volumeName(const std::string &name, const std::string &base) {
    if (name.rfind(base + ".", 0) != 0)
        return false;
    auto suffix = name.substr(base.size() + 1);
    return suffix.size() >= 3 && suffix.find_first_not_of("0123456789") == std::string::npos;
}
bool verifyManifest(const fs::path &archive, const AssemblyPlan &plan,
                    const AnalysisSummary &summary, const OptimizerSettings &settings,
                    ResourceGuard *guard, const std::atomic<bool> *cancel, std::string &error) {
    auto root = fs::is_directory(plan.inputSourcePath) ? plan.inputSourcePath
                                                       : plan.inputSourcePath.parent_path();
    std::map<std::string, uint64_t> expected, actual;
    for (const auto &f : summary.files)
        expected[pathUtf8(f.path.lexically_relative(root))] = f.sizeBytes;
    bool invalidManifest = false;
    std::set<std::string> entryKeys;
    std::string current;
    uint64_t size = 0;
    bool directory = false, hasSize = false;
    auto commit = [&] {
        if (!current.empty()) {
            try {
                WorkspaceManager::validateArchivePath(current);
                if (!entryKeys.insert(archivePathKey(current)).second)
                    invalidManifest = true;
                if (!directory && (!hasSize || !actual.emplace(current, size).second))
                    invalidManifest = true;
            } catch (...) {
                invalidManifest = true;
            }
        }
        current.clear();
        size = 0;
        directory = false;
        hasSize = false;
    };
    std::vector<std::string> args = {"l", "-slt", "-ba", "-sccUTF-8"};
    if (!plan.password.empty())
        args.push_back("-p" + plan.password);
    args.push_back(pathUtf8(archive));
    ProcessControl control{nullptr, settings.processTimeoutSeconds};
    auto r = ProcessManager::execute(
        pathUtf8(ProcessManager::resolveExecutable("7z", settings.toolsBinDir)), args, root, guard,
        0, cancel,
        [&](const std::string &line) {
            if (line.rfind("Path = ", 0) == 0) {
                commit();
                current = line.substr(7);
                std::replace(current.begin(), current.end(), '\\', '/');
            } else if (line.rfind("Size = ", 0) == 0) {
                try {
                    size = std::stoull(line.substr(7));
                } catch (...) {
                    invalidManifest = true;
                }
                hasSize = true;
            } else if (line.rfind("Attributes = ", 0) == 0 &&
                       line.substr(13).find('D') != std::string::npos)
                directory = true;
            else if (line == "Folder = +")
                directory = true;
        },
        &control);
    commit();
    if (!r.success) {
        error = "Cannot inspect archive manifest: " + r.errOutput;
        return false;
    }
    std::map<std::string, uint64_t> normalized;
    for (const auto &[path, bytes] : expected) {
        auto key = path;
        std::replace(key.begin(), key.end(), '\\', '/');
        normalized[key] = bytes;
    }
    if (invalidManifest || actual != normalized) {
        error = "Archive file list/sizes differ from the input manifest";
        return false;
    }
    auto verificationRoot =
        (settings.customTempDir.empty() ? archive.parent_path() : settings.customTempDir) /
        ("verify_" + uniqueId());
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{verificationRoot};
    if (!ArchiveExtractor::extract(archive, verificationRoot, settings.toolsBinDir, cancel, nullptr,
                                   plan.password, guard, settings.processTimeoutSeconds,
                                   {settings.extractionLimitMB * 1048576,
                                    settings.extractionFileLimitMB * 1048576,
                                    settings.extractionFileCount})) {
        error = "Cannot extract final archive for content verification";
        return false;
    }
    for (const auto &file : summary.files) {
        auto restored = verificationRoot / file.path.lexically_relative(root);
        auto expectedHash =
            file.contentChecksum.empty() ? fileChecksum(file.path, cancel) : file.contentChecksum;
        if (fileChecksum(restored, cancel) != expectedHash) {
            error = "Restored contents differ: " + pathUtf8(file.path);
            return false;
        }
    }
    for (const auto &dir : summary.emptyDirectories)
        if (!fs::is_directory(verificationRoot / dir.lexically_relative(root))) {
            error = "Archive omitted an empty directory";
            return false;
        }
    return true;
}
} // namespace
bool ArchiveAssembler::assembleFinalArchive(const AssemblyPlan &plan,
                                            const AnalysisSummary &summary,
                                            const OptimizerSettings &settings, ResourceGuard *guard,
                                            const std::atomic<bool> *cancel,
                                            std::function<void(const std::string &)> log,
                                            std::function<void(float)> progress,
                                            std::vector<fs::path> *outputFiles, StageMetrics *metrics) {
    // UI callbacks must never turn a committed publication into a rollback.
    log = [callback = std::move(log)](const std::string &message) {
        if (callback)
            try {
                callback(message);
            } catch (...) {
            }
    };
    progress = [callback = std::move(progress)](float value) {
        if (callback)
            try {
                callback(value);
            } catch (...) {
            }
    };
    fs::path stage;
    std::unique_ptr<PublicationTransaction> transaction;
    try {
        StageMetrics::Scope staging(metrics, "FinalStaging");
        if (cancel && cancel->load())
            return false;
        auto target = fs::absolute(plan.targetOutputPath).lexically_normal();
        if (isWithin(target, plan.inputSourcePath))
            throw std::runtime_error("Output must be outside the input tree");
        fs::create_directories(target.parent_path());
        if (fs::exists(target) && !fs::is_regular_file(target))
            throw std::runtime_error("Output is not a regular file");
        if (!PublicationTransaction::pending(target.parent_path()).empty())
            throw std::runtime_error(
                "Pending publication recovery in output folder; recover before starting");
        transaction = std::make_unique<PublicationTransaction>(target);
        stage = transaction->stage();
        auto staged = stage / "new" / target.filename();
        if (progress)
            progress(0.05f);
        bool canReuse = plan.reuseCandidate &&
                        fs::is_regular_file(plan.bestCandidateArchive);
        if (canReuse) {
            if (log)
                log("[Assembler] Staging the winning archive without recompression.");
            auto copyBytes = [&](const fs::path &source, std::ostream &destination, uint64_t limit = UINT64_MAX) {
                std::ifstream input(source, std::ios::binary);
                if (!input)
                    throw std::runtime_error("Cannot read selected archive/module");
                std::array<char, 65536> buffer;
                while (limit && input) {
                    if (cancel && cancel->load())
                        throw std::runtime_error("Assembly cancelled");
                    auto count = std::min<uint64_t>(limit, buffer.size());
                    input.read(buffer.data(), static_cast<std::streamsize>(count));
                    auto bytes = input.gcount();
                    if (bytes) {
                        destination.write(buffer.data(), bytes);
                        limit -= static_cast<uint64_t>(bytes);
                    }
                }
                if (input.bad() || !destination)
                    throw std::runtime_error("Cannot stage selected archive/module");
            };
            if (plan.volumeSizeMB) {
                std::ifstream input(plan.bestCandidateArchive, std::ios::binary);
                if (!input)
                    throw std::runtime_error("Cannot read selected archive for splitting");
                const uint64_t volumeBytes = plan.volumeSizeMB * 1048576;
                uint64_t remaining = fs::file_size(plan.bestCandidateArchive);
                unsigned number = 0;
                std::array<char, 65536> buffer;
                while (remaining) {
                    std::ostringstream suffix;
                    suffix << '.' << std::setw(3) << std::setfill('0') << ++number;
                    std::ofstream output(utf8Path(pathUtf8(staged) + suffix.str()), std::ios::binary);
                    auto bytes = std::min(remaining, volumeBytes);
                    remaining -= bytes;
                    while (bytes) {
                        if (cancel && cancel->load())
                            throw std::runtime_error("Volume assembly cancelled");
                        auto count = std::min<uint64_t>(bytes, buffer.size());
                        if (!input.read(buffer.data(), static_cast<std::streamsize>(count)))
                            throw std::runtime_error("Selected archive changed during splitting");
                        output.write(buffer.data(), static_cast<std::streamsize>(count));
                        if (!output)
                            throw std::runtime_error("Cannot write volume");
                        bytes -= count;
                    }
                    output.close();
                    if (!output)
                        throw std::runtime_error("Cannot finalize volume");
                }
            } else {
                std::ofstream output(staged, std::ios::binary);
                if (plan.createSfx)
                    copyBytes(settings.toolsBinDir / "7z.sfx", output);
                copyBytes(plan.bestCandidateArchive, output);
                output.close();
                if (!output)
                    throw std::runtime_error("Cannot finalize selected archive");
            }
        } else {
            auto root = fs::is_directory(plan.inputSourcePath) ? plan.inputSourcePath
                                                               : plan.inputSourcePath.parent_path();
            auto list = stage / "input.txt";
            std::ofstream out(list, std::ios::binary);
            for (const auto &f : summary.files)
                out << pathUtf8(f.path.lexically_relative(root)) << '\n';
            for (const auto &dir : summary.emptyDirectories)
                out << pathUtf8(dir.lexically_relative(root)) << '\n';
            out.close();
            if (!out)
                throw std::runtime_error("Cannot write input manifest");
            std::vector<std::string> args = {"a", "-y", "-t7z", "-scsUTF-8", "-sccUTF-8", "-spd"};
            for (const auto &f : plan.winningConfig.flags)
                args.push_back(f);
            args.push_back(
                "-w" + pathUtf8(settings.customTempDir.empty() ? stage : settings.customTempDir));
            if (plan.createSfx) {
                auto module = settings.toolsBinDir / "7z.sfx";
                if (!fs::is_regular_file(module))
                    throw std::runtime_error("SFX module 7z.sfx was not found");
                args.push_back("-sfx" + pathUtf8(fs::absolute(module)));
            }
            if (plan.volumeSizeMB)
                args.push_back("-v" + std::to_string(plan.volumeSizeMB) + "m");
            args.push_back(pathUtf8(staged));
            args.push_back("@" + pathUtf8(list));
            ProcessControl control{nullptr, settings.processTimeoutSeconds};
            if (log)
                log("[Assembler] Creating final archive.");
            auto r = ProcessManager::execute(
                pathUtf8(ProcessManager::resolveExecutable("7z", settings.toolsBinDir)), args, root,
                guard, 0, cancel,
                log ? [&](const std::string &line) { log("[7z] " + line); }
                    : std::function<void(const std::string &)>{},
                &control);
            if (!r.success)
                throw std::runtime_error("Final compression failed: " + r.errOutput);
        }
        staging.finish();
        StageMetrics::Scope verification(metrics, "FinalVerification");
        auto first = plan.volumeSizeMB ? utf8Path(pathUtf8(staged) + ".001") : staged;
        std::string error;
        if (progress)
            progress(0.8f);
        // Final output is always checked, even when candidate verification is disabled.
        bool diskRequired = true;
        if (settings.verificationMode != VerificationMode::Disk) {
            auto result =
                StreamingVerifier::verify(first, plan, summary, settings, guard, cancel, error);
            if (result == StreamingVerifier::Result::Failed)
                throw std::runtime_error(error);
            if (result == StreamingVerifier::Result::Unavailable) {
                if (settings.verificationMode == VerificationMode::Dual)
                    throw std::runtime_error("Dual verifier unavailable: " + error);
                log("[Verifier] Disk fallback: " + error);
            } else {
                log("[Verifier] Streaming SHA-256/CRC verified");
                diskRequired = settings.verificationMode == VerificationMode::Dual;
            }
        }
        if (diskRequired && (!verifyArchive(first, settings.toolsBinDir, error, plan.password,
                                            cancel, guard, settings.processTimeoutSeconds) ||
                             !verifyManifest(first, plan, summary, settings, guard, cancel, error)))
            throw std::runtime_error(error);
        if (cancel && cancel->load())
            throw std::runtime_error("Assembly cancelled");
        verification.finish();
        StageMetrics::Scope publication(metrics, "Publication");
        std::vector<fs::path> created;
        for (const auto &e : fs::directory_iterator(stage / "new")) {
            auto name = pathUtf8(e.path().filename());
            auto base = pathUtf8(target.filename());
            if (e.is_regular_file() && (name == base || volumeName(name, base)))
                created.push_back(e.path());
        }
        if (created.empty())
            throw std::runtime_error("Compression produced no output files");
        std::sort(created.begin(), created.end());
        transaction->verified(created);
        transaction->publish(cancel);
        if (outputFiles)
            *outputFiles = transaction->outputs();
        if (progress)
            progress(1.0f);
        if (log)
            log("[Assembler] Final archive verified and published successfully.");
        return true;
    } catch (const std::exception &e) {
        StageMetrics::Scope rollback(metrics, "PublicationRollback");
        if (transaction) {
            try {
                transaction->rollback();
            } catch (const std::exception &recoveryError) {
                log(std::string("[Recovery] Journal retained: ") + pathUtf8(stage) + " / " +
                    recoveryError.what());
            }
        }
        log(std::string("[ERROR] Assembly failed: ") + e.what());
        return false;
    }
}
} // namespace UltraArch
