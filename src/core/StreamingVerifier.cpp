#include "StreamingVerifier.hpp"
#include "BackendIdentity.hpp"
#include "SettingsJson.hpp"
namespace UltraArch {
StreamingVerifier::Result
StreamingVerifier::verify(const std::filesystem::path &archive, const AssemblyPlan &plan,
                          const AnalysisSummary &summary, const OptimizerSettings &settings,
                          ResourceGuard *guard, const std::atomic<bool> *cancel,
                          std::string &error, const ProcessControl *processControl) {
#ifndef _WIN32
    (void)archive;
    (void)plan;
    (void)summary;
    (void)settings;
    (void)guard;
    (void)cancel;
    (void)processControl;
    error = "Streaming worker requires Windows";
    return Result::Unavailable;
#else
    namespace fs = std::filesystem;
    auto dll = fs::absolute(settings.toolsBinDir / "7z.dll");
    if (!fs::is_regular_file(dll) ||
        fileChecksum(dll, cancel) !=
            pinned7ZipDllSha256) {
        error = std::string("Pinned official 7-Zip ") + pinned7ZipVersion + " DLL unavailable";
        return Result::Unavailable;
    }
    wchar_t module[32768]{};
    if (!GetModuleFileNameW(nullptr, module, 32768)) {
        error = "Cannot locate worker";
        return Result::Unavailable;
    }
    auto app = fs::path(module).parent_path();
    std::vector<fs::path> candidates = {app / "verify-worker.exe",
                                        app.parent_path() / "verify-worker.exe",
                                        app.parent_path().parent_path() / app.filename() /
                                            "verify-worker.exe",
                                        settings.toolsBinDir.parent_path() / "verify-worker.exe"};
    fs::path worker;
    for (const auto &candidate : candidates)
        if (fs::is_regular_file(candidate)) {
            worker = candidate;
            break;
        }
    if (worker.empty()) {
        error = "verify-worker.exe unavailable";
        return Result::Unavailable;
    }
    auto root = fs::is_directory(plan.inputSourcePath) ? plan.inputSourcePath
                                                       : plan.inputSourcePath.parent_path();
    Json manifest = {{"schema", 1}, {"files", Json::array()}, {"directories", Json::array()}};
    for (const auto &file : summary.files)
        manifest["files"].push_back(
            {{"path", pathUtf8(file.path.lexically_relative(root))},
             {"size", file.sizeBytes},
             {"sha256", file.contentChecksum.empty() ? fileChecksum(file.path, cancel)
                                                     : file.contentChecksum}});
    for (const auto &dir : summary.emptyDirectories)
        manifest["directories"].push_back(pathUtf8(dir.lexically_relative(root)));
    auto path = (settings.customTempDir.empty() ? archive.parent_path() : settings.customTempDir) /
                ("verify-manifest-" + uniqueId() + ".json");
    std::error_code directoryError;
    fs::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        error = "Cannot create worker manifest directory: " + directoryError.message();
        return Result::Failed;
    }
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(p, ec);
        }
    } cleanup{path};
    if (!writeAtomically(path, manifest.dump())) {
        error = "Cannot write worker manifest";
        return Result::Failed;
    }
    Json request = {{"dll", pathUtf8(dll)},
                    {"archive", pathUtf8(fs::absolute(archive))},
                    {"manifest", pathUtf8(fs::absolute(path))},
                    {"manifestHash", fileChecksum(path, cancel)},
                    {"password", plan.password}};
    auto text = request.dump();
    if (text.size() > 65536) {
        error = "Worker request exceeds IPC bound";
        return Result::Failed;
    }
    ProcessControl control = processControl ? *processControl : ProcessControl{};
    if (!control.timeoutSeconds)
        control.timeoutSeconds = settings.processTimeoutSeconds;
    control.stdinData = "UAV1";
    for (int i = 0; i < 4; ++i)
        control.stdinData.push_back(
            static_cast<char>(static_cast<uint32_t>(text.size()) >> (8 * i)));
    control.stdinData += text;
    auto process = ProcessManager::execute(pathUtf8(worker), {}, worker.parent_path(), guard, 0,
                                           cancel, nullptr, &control);
    try {
        const auto &frame = process.stdOutput;
        if (frame.size() < 8 || frame.substr(0, 4) != "UAV1")
            throw std::runtime_error("Invalid worker IPC reply");
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i)
            length |= uint32_t(static_cast<unsigned char>(frame[4 + i])) << (8 * i);
        if (length != frame.size() - 8)
            throw std::runtime_error("Truncated worker IPC reply");
        auto reply = Json::parse(frame.substr(8));
        if (!process.success || !reply.value("success", false) || reply.value("schema", 0) != 1 ||
            reply.value("files", uint64_t(0)) != summary.totalFiles ||
            reply.value("bytes", uint64_t(0)) != summary.totalBytes)
            throw std::runtime_error(
                reply.value("error", std::string("Worker verification failed")));
        return Result::Verified;
    } catch (const std::exception &e) {
        error = process.cancelled     ? "Worker cancelled"
                : process.timedOut    ? "Worker timeout"
                : process.killedByOOM ? "Worker memory limit exceeded"
                                      : e.what();
        return Result::Failed;
    }
#endif
}
} // namespace UltraArch
