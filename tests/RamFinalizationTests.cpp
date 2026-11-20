#include "core/OptimizerEngine.hpp"
#include "core/InputManifest.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
using namespace UltraArch;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
struct Temp {
    fs::path path = fs::temp_directory_path() / utf8Path("ram-final-日本語-" + uniqueId());
    Temp() { fs::create_directories(path); }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
static void write(const fs::path &path, const std::string &data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out.write(data.data(), data.size()); CHECK(out.good());
}
static std::string noise(size_t count) {
    std::mt19937 rng(117); std::string data(count, '\0');
    for (auto &byte : data) byte = static_cast<char>(rng());
    return data;
}
static fs::path tools(const char *exe) {
    if (const char *env = std::getenv("ULTRAARCH_TOOLS_DIR")) {
        auto found = ProcessManager::resolveExecutable("7z", utf8Path(env), true);
        if (!found.empty()) return found;
    }
    auto parent = fs::absolute(utf8Path(exe)).parent_path();
    for (unsigned i = 0; i < 8; ++i) {
        auto found = ProcessManager::resolveExecutable("7z", parent / "bin", true);
        if (!found.empty()) return found;
        parent = parent.parent_path();
    }
    return ProcessManager::resolveExecutable("7z", "bin");
}
static OptimizerSettings settings(const fs::path &tool, const fs::path &base) {
    OptimizerSettings s;
    s.enableRamCandidates = true; s.ramGroupLimitMB = 4; s.ramTemporaryBudgetMB = 512;
    s.ramLimitMB = 8192; s.workerThreads = 2; s.cpuThreads = 4;
    s.maxLzmaDictionaryMB = 1; // Also prohibits original-archive reuse.
    s.enableGrouping = false; s.enablePreprocessors = false;
    s.profile = OptimizationProfile::DeepSearch; s.searchMode = SearchMode::Fast;
    s.verificationMode = VerificationMode::Dual; s.processTimeoutSeconds = 20;
    s.toolsBinDir = tool.parent_path(); s.customTempDir = base / "scratch";
    return s;
}
static void runCase(const fs::path &tool, const fs::path &base, bool sfx, bool volumes, bool encrypted) {
    const auto source = base / "source";
    write(source / utf8Path("папка-日本語") / "text.txt", std::string(40000, 't'));
    // A >1 MiB incompressible result is required to exercise multiple actual volumes.
    write(source / "noise.bin", noise((volumes ? 1048576 : 0) + 100 * 1024));
    write(source / "zero.txt", ""); fs::create_directories(source / utf8Path("пусто-日本語") / "leaf");
    const auto original = InputManifest::scan(source);
    const auto archive = base / "input.7z";
    ProcessControl control{nullptr, 20};
    CHECK(ProcessManager::execute(pathUtf8(tool), {"a", "-y", "-t7z", "-mx0", pathUtf8(archive), "."},
                                  source, nullptr, 0, nullptr, nullptr, &control).success);
    auto s = settings(tool, base);
    s.sfxOutput = sfx; s.volumeSizeMB = volumes ? 1 : 0;
    if (encrypted) { s.password = "finalization-contract-only-日本語-42"; s.encryptHeaders = true; }
    const auto output = base / (sfx ? "output.exe" : "output.7z");
    std::atomic<bool> leakedSecret{false};
    OptimizerEngine engine; engine.setSettings(s);
    engine.setLogCallback([&](const std::string &line) {
        if (!s.password.empty() && line.find(s.password) != std::string::npos) leakedSecret = true;
    });
    CHECK(engine.start(archive, output));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(75);
    bool badBudget = false;
    while (engine.getState() != EngineState::Completed && engine.getState() != EngineState::Failed &&
           engine.getState() != EngineState::Cancelled) {
        const auto snapshot = engine.getSnapshot();
        badBudget |= snapshot.reservedRamMB > s.ramLimitMB || snapshot.reservedCpu > s.cpuThreads;
        if (std::chrono::steady_clock::now() > deadline) {
            engine.cancel(); engine.wait(); throw std::runtime_error("RAM finalization deadline exceeded");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.wait();
    if (engine.getState() != EngineState::Completed) throw std::runtime_error(engine.getLastError());
    CHECK(!badBudget && !leakedSecret);
    const auto results = engine.getResults();
    CHECK(std::any_of(results.begin(), results.end(), [](const auto &candidate) {
        return candidate.storage == "ram" && candidate.status == CandidateStatus::Verified;
    }));
    auto outputs = engine.getOutputFiles(); CHECK(!outputs.empty());
    std::sort(outputs.begin(), outputs.end());
    const auto first = volumes ? utf8Path(pathUtf8(output) + ".001") : output;
    CHECK(fs::is_regular_file(first));
    if (volumes) {
        CHECK(outputs.size() >= 2);
        CHECK(fs::file_size(first) == 1048576);
        CHECK(fs::is_regular_file(utf8Path(pathUtf8(output) + ".002")));
    }
    if (sfx) {
        std::ifstream stream(first, std::ios::binary); char signature[2]{};
        stream.read(signature, 2); CHECK(stream.gcount() == 2 && signature[0] == 'M' && signature[1] == 'Z');
        // Decode through normal 7-Zip SFX support; never execute generated SFX code.
    }
    if (sfx || volumes) {
        auto budget = std::make_shared<RamBudget>(512 * RamPolicy::MiB);
        ResourceGuard guard; CHECK(guard.initialize(s.ramLimitMB));
        auto fallback = RamInput::extract(first, base / "hybrid-unsupported", s, budget, &guard, nullptr);
        CHECK(!fallback.success && !fallback.fallbackReason.empty() && budget->used() == 0);
        CHECK(fs::is_empty(base / "hybrid-unsupported"));
    }
    CHECK(ArchiveExtractor::extract(first, base / "restored", tool.parent_path(), nullptr, nullptr,
                                    s.password, nullptr, 20));
    const auto restored = InputManifest::scan(base / "restored");
    CHECK(restored.fingerprint == original.fingerprint);
    CHECK(restored.summary.totalFiles == original.summary.totalFiles);
    CHECK(restored.summary.totalBytes == original.summary.totalBytes);
    for (const auto &file : original.summary.files)
        CHECK(fileChecksum(file.path) == fileChecksum(base / "restored" / file.path.lexically_relative(source)));
    for (const auto &directory : original.summary.emptyDirectories)
        CHECK(fs::is_directory(base / "restored" / directory.lexically_relative(source)));
    const auto report = engine.getRunReport();
    CHECK(report["inputStorage"]["ramBytes"].get<uint64_t>() > 0);
    CHECK(s.password.empty() || report.dump().find(s.password) == std::string::npos);
    std::cout << "[PASS] Native RAM finalization " << (sfx ? "SFX" : volumes ? "multiple volumes" : "encrypted headers") << '\n';
}
static void oom(const fs::path &tool, const fs::path &base) {
    const auto root = base / "input";
    // PPMd's Normalize() reduces requested memory to roughly 16x input size.
    // Instead force the mandatory immutable input allocation beyond the worker
    // Job limit; this is a resource failure independent of codec heuristics.
    write(root / "bounded-input.txt", std::string(16 * 1048576, 'p'));
    const auto manifest = InputManifest::scan(root);
    auto s = settings(tool, base);
    s.ramGroupLimitMB = 40;
    TaskConfig task;
    CodecSpec method; method.codec = Codec::PPMd; method.dictionary = MiB{512}.bytes(); method.order = 8;
    task.spec.methods = {method}; task.spec.threads = 1; task.spec.solid = "on";
    task.configId = task.spec.id(); task.flags = task.spec.compile();
    task.name = "bounded PPMd worker OOM"; task.taskId = 1;
    task.estimatedRamMB = task.spec.memoryMB();
    auto budget = std::make_shared<RamBudget>(512 * RamPolicy::MiB);
    ResourceGuard guard; CHECK(guard.initialize(16));
    ProcessControl control{nullptr, 20};
    auto result = RamCandidate::run(task, manifest.summary, root, s, budget, &guard, 0, nullptr, &control);
    if (result.artifact || result.process.success || !result.process.killedByOOM) {
        std::cerr << "RAM worker OOM diagnostic: artifact=" << bool(result.artifact)
                  << " success=" << result.process.success << " oom=" << result.process.killedByOOM
                  << " exit=" << result.process.exitCode << " win32=" << result.process.win32Error
                  << " peakMB=" << result.process.peakMemoryMB << " fallback=" << result.fallbackReason
                  << " stderr=" << result.process.errOutput.substr(0, 2048)
                  << " stdout=" << result.process.stdOutput.substr(0, 2048) << '\n';
    }
    CHECK(!result.artifact && !result.process.success && result.process.killedByOOM);
    CHECK(result.fallbackReason.empty()); CHECK(budget->used() == 0);
    CHECK(!fs::exists(base / "output.7z"));
    std::cout << "[PASS] Native RAM encoder OOM, nonfallback, reservation release\n";
}
int main(int argc, char **argv) {
    (void)argc;
#ifndef _WIN32
    (void)argv; return 77;
#else
    try {
        auto tool = tools(argv[0]); CHECK(!tool.empty()); Temp temp;
        if (argc == 2 && std::string(argv[1]) == "--oom-only") {
            oom(tool, temp.path / "oom"); return 0;
        }
        runCase(tool, temp.path / "sfx", true, false, false);
        runCase(tool, temp.path / "volumes", false, true, false);
        runCase(tool, temp.path / "encrypted", false, false, true);
        oom(tool, temp.path / "oom");
        std::cout << "RAM finalization contracts passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
#endif
}
