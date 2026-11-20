#include "core/OptimizerEngine.hpp"
#include "core/InputManifest.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
using namespace UltraArch;
namespace fs = std::filesystem;
namespace UltraArch {
struct OptimizerEngineTestAccess {
    static std::shared_ptr<RamBudget> budget(OptimizerEngine &engine) { return engine.m_ramBudget; }
    static bool checkpoint(OptimizerEngine &engine) { return engine.checkpoint(); }
};
}
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
struct Temp {
    fs::path path = fs::temp_directory_path() / utf8Path("ram-engine-日本語-" + uniqueId());
    Temp() { fs::create_directories(path); }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
static void write(const fs::path &path, const std::string &data) {
    fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out.write(data.data(), data.size()); CHECK(out.good());
}
static fs::path tools(const char *exe) {
    if (const char *env = std::getenv("ULTRAARCH_TOOLS_DIR")) {
        auto tool = ProcessManager::resolveExecutable("7z", utf8Path(env), true);
        if (!tool.empty()) return tool;
    }
    auto parent = fs::absolute(utf8Path(exe)).parent_path();
    for (unsigned i = 0; i < 8; ++i) {
        auto tool = ProcessManager::resolveExecutable("7z", parent / "bin", true);
        if (!tool.empty()) return tool; parent = parent.parent_path();
    }
    return ProcessManager::resolveExecutable("7z", "bin");
}
static void verify(const fs::path &archive, const fs::path &dest, const InputManifest &original,
                   const fs::path &tool) {
    CHECK(ArchiveExtractor::extract(archive, dest, tool.parent_path(), nullptr, nullptr, "", nullptr, 20));
    auto restored = InputManifest::scan(dest);
    CHECK(restored.fingerprint == original.fingerprint);
    CHECK(restored.summary.totalFiles == original.summary.totalFiles);
    for (const auto &dir : original.summary.emptyDirectories)
        CHECK(fs::is_directory(dest / dir.lexically_relative(original.root)));
}
static void runCase(const fs::path &tool, const fs::path &input, const InputManifest &original,
                    const fs::path &base, bool grouped, bool mixed,
                    SelectionGoal goal = SelectionGoal::MinimumSize, bool keepAll = false,
                    bool requireRamProbe = false) {
    OptimizerSettings s; s.enableRamCandidates = true; s.ramGroupLimitMB = requireRamProbe ? 4 : 1;
    if (requireRamProbe) s.maxLzmaDictionaryMB = 1;
    s.ramTemporaryBudgetMB = 512; s.ramLimitMB = 8192; s.workerThreads = 2; s.cpuThreads = 4;
    s.searchMode = SearchMode::Fast; s.profile = grouped ? OptimizationProfile::Extreme_RePack : OptimizationProfile::DeepSearch;
    s.enableGrouping = grouped; s.groupedSearchOnly = grouped; s.enablePreprocessors = false;
    s.verificationMode = VerificationMode::Dual; s.processTimeoutSeconds = 20;
    s.selectionGoal = goal; s.immediateCleanInferior = !keepAll;
    s.customTempDir = base / "scratch"; s.toolsBinDir = tool.parent_path();
    fs::create_directories(base);
    std::cout << "[RUN] " << pathUtf8(base.filename()) << std::endl;
    OptimizerEngine engine; engine.setSettings(s);
    std::atomic<bool> extractionBudgetValid{true};
    engine.setLogCallback([&](const std::string &line) {
        if (line.rfind("[RAM input]", 0) == 0) {
            // Input buffers still belong to the extraction lease at this boundary.
            const auto snapshot = engine.getSnapshot();
            if (snapshot.reservedRamMB > s.ramLimitMB) extractionBudgetValid = false;
        }
    });
    std::atomic<bool> badRetention{false}; std::atomic<unsigned> retainedCount{0};
    if (goal == SelectionGoal::FastWithinTolerance || keepAll)
        engine.setCandidateCallback([&](const CandidateResult &result) {
            if (result.status != CandidateStatus::Verified || result.storage != "ram") return;
            if (result.tempArchivePath.empty() || !fs::is_regular_file(result.tempArchivePath) ||
                result.temporaryFileBytes != result.outputBytes) badRetention = true;
            ++retainedCount;
        });
    CHECK(engine.start(input, base / "output.7z"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(75);
    bool badBudget = false;
    while (engine.getState() != EngineState::Completed && engine.getState() != EngineState::Failed &&
           engine.getState() != EngineState::Cancelled) {
        const auto snapshot = engine.getSnapshot();
        badBudget |= snapshot.reservedRamMB > s.ramLimitMB || snapshot.reservedCpu > s.cpuThreads;
        if (std::chrono::steady_clock::now() > deadline) {
            engine.cancel(); engine.wait(); throw std::runtime_error("RAM engine deadline exceeded");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.wait(); CHECK(!badBudget); CHECK(extractionBudgetValid.load());
    if (engine.getState() != EngineState::Completed) throw std::runtime_error(engine.getLastError());
    verify(base / "output.7z", base / "restore", original, tool);
    const auto report = engine.getRunReport();
    CHECK(engine.saveRunReport(base / "report.json"));
    CHECK(report.contains("temporaryIO") && report.contains("inputStorage"));
    CHECK(report["inputStorage"]["ramBytes"].get<uint64_t>() > 0);
    if (mixed) CHECK(report["inputStorage"]["diskBytes"].get<uint64_t>() > 1048576);
    CHECK(report["inputStorage"]["reservedMB"].get<uint64_t>() <= s.ramTemporaryBudgetMB);
    if (requireRamProbe) {
        CHECK(report["temporaryIO"]["ramProbes"].get<uint64_t>() > 0);
        CHECK(report["temporaryIO"]["probeInputFileBytes"].get<uint64_t>() == 0);
    }
    bool sawRam = false, sawZeroWriteLoser = false;
    auto checkResults = [&](const Json &results) {
        for (const auto &result : results) {
            if (result.value("storage", "disk") != "ram") continue;
            sawRam = true;
            if (goal == SelectionGoal::MinimumSize && !keepAll && !result.value("isLeader", false) &&
                result.value("archive", "").empty() && result.value("temporaryFileBytes", uint64_t(0)) == 0)
                sawZeroWriteLoser = true;
        }
    };
    checkResults(report.at("results"));
    for (const auto &group : report.at("groups"))
        if (group.contains("results")) checkResults(group.at("results"));
    if ((grouped || !mixed) && !sawRam) {
        std::cerr << "No RAM candidates in " << pathUtf8(base.filename()) << '\n';
        auto describe = [](const Json &results) {
            size_t shown = 0;
            for (const auto &r : results) {
                if (shown++ == 8) { std::cerr << "Further candidate diagnostics omitted\n"; break; }
                std::cerr << r.value("name", "?") << " storage=" << r.value("storage", "disk")
                          << " fallback=" << r.value("storageFallback", "")
                          << " status=" << r.at("status") << '\n';
            }
        };
        describe(report.at("results"));
        for (const auto &group : report.at("groups")) {
            std::cerr << "group " << group.value("groupKey", "?") << '\n';
            if (group.contains("results")) describe(group.at("results"));
        }
        throw std::runtime_error("Expected native RAM candidates; see storage diagnostics");
    }
    // Sample selection can retain only improving full candidates; a loser is not guaranteed.
    // The dedicated tiny-input case exercises zero-write losers without sample pruning.
    if (!requireRamProbe && !grouped && !mixed && goal == SelectionGoal::MinimumSize && !keepAll) CHECK(sawZeroWriteLoser);
    if (goal == SelectionGoal::FastWithinTolerance || keepAll) {
        unsigned retained = 0;
        for (const auto &result : engine.getResults())
            if (result.status == CandidateStatus::Verified && result.storage == "ram") {
                CHECK(result.temporaryFileBytes == result.outputBytes); ++retained;
            }
        CHECK(retained > 0 && retainedCount > 0 && !badRetention);
    }
    CHECK(engine.saveRunReport(base / "report.json"));
}
static void contracts(const fs::path &tool) {
    Temp temp; const auto source = temp.path / "source";
    write(source / utf8Path("папка-日本語") / "one.txt", std::string(30000, 'a'));
    write(source / "two.txt", std::string(35000, 'b'));
    write(source / "zero.txt", ""); fs::create_directories(source / "empty" / "leaf");
    std::mt19937 rng(42); std::string noise(2200000, '\0');
    for (auto &c : noise) c = static_cast<char>(rng());
    write(source / "noise.bin", noise);
    const auto original = InputManifest::scan(source);
    const auto archive = temp.path / "input.7z";
    CHECK(ProcessManager::execute(pathUtf8(tool), {"a", "-y", "-t7z", "-mx=1", pathUtf8(archive), "."}, source).success);
    runCase(tool, archive, original, temp.path / "plain", false, true);
    runCase(tool, archive, original, temp.path / "grouped", true, true);
    const auto small = temp.path / "small";
    write(small / "tiny.txt", std::string(20000, 'c')); fs::create_directories(small / "empty");
    const auto smallManifest = InputManifest::scan(small);
    const auto smallArchive = temp.path / "small.7z";
    CHECK(ProcessManager::execute(pathUtf8(tool), {"a", "-y", "-t7z", "-mx=1", pathUtf8(smallArchive), "."}, small).success);
    runCase(tool, smallArchive, smallManifest, temp.path / "minimum", false, false);
    runCase(tool, smallArchive, smallManifest, temp.path / "speed", false, false, SelectionGoal::FastWithinTolerance);
    runCase(tool, smallArchive, smallManifest, temp.path / "keep-all", false, false, SelectionGoal::MinimumSize, true);
    const auto probeSource = temp.path / "probe-source";
    write(probeSource / "probe.txt", std::string(3 * 1048576, 'p'));
    const auto probeManifest = InputManifest::scan(probeSource);
    const auto probeArchive = temp.path / "probe.7z";
    CHECK(ProcessManager::execute(pathUtf8(tool), {"a", "-y", "-t7z", "-mx=1", pathUtf8(probeArchive), "."}, probeSource).success);
    runCase(tool, probeArchive, probeManifest, temp.path / "ram-probes", false, false,
            SelectionGoal::MinimumSize, false, true);
    // Lose all volatile input mappings between instances; only a durable leader may resume.
    OptimizerSettings resumeSettings;
    resumeSettings.enableRamCandidates = true; resumeSettings.ramGroupLimitMB = 1;
    resumeSettings.ramTemporaryBudgetMB = 512; resumeSettings.ramLimitMB = 8192;
    resumeSettings.workerThreads = 2; resumeSettings.cpuThreads = 4;
    resumeSettings.profile = OptimizationProfile::DeepSearch; resumeSettings.searchMode = SearchMode::Fast;
    resumeSettings.enableGrouping = false; resumeSettings.maxLzmaDictionaryMB = 1;
    resumeSettings.toolsBinDir = tool.parent_path(); resumeSettings.processTimeoutSeconds = 20;
    resumeSettings.customTempDir = temp.path / "resume-scratch";
    std::atomic<bool> cancelled{false};
    {
        OptimizerEngine first; first.setSettings(resumeSettings);
        first.setCandidateCallback([&](const CandidateResult &r) {
            if (r.storage == "ram" && r.status == CandidateStatus::Verified && !cancelled.exchange(true))
                first.cancel();
        });
        CHECK(first.start(smallArchive, temp.path / "cancelled.7z")); first.wait();
        CHECK(cancelled && first.getState() == EngineState::Cancelled);
        CHECK(!fs::exists(temp.path / "cancelled.7z"));
    }
    SessionManager manager; manager.setSessionDir(resumeSettings.customTempDir / "sessions");
    CHECK(!manager.getSessionHistory().empty());
    bool resumed = false;
    {
        OptimizerEngine second; second.setSettings(resumeSettings);
        second.setLogCallback([&](const std::string &line) {
            if (line.find("[Session] Resumed") != std::string::npos) resumed = true;
        });
        CHECK(second.start(smallArchive, temp.path / "resumed.7z")); second.wait();
        if (second.getState() != EngineState::Completed) throw std::runtime_error(second.getLastError());
    }
    CHECK(resumed && manager.getSessionHistory().empty());
    verify(temp.path / "resumed.7z", temp.path / "resumed-tree", smallManifest, tool);
    // A failed checkpoint must preserve the previously restorable archive.
    auto fault = resumeSettings;
    fault.verifyArchives = false; // RAM candidates still have mandatory SHA verification.
    fault.workerThreads = fault.cpuThreads = 1;
    fault.customTempDir = temp.path / "checkpoint-failure";
    const auto journalDir = fault.customTempDir / "sessions";
    const auto savedJournal = fault.customTempDir / "sessions-saved";
    fs::path oldLeader; std::string oldSha, callbackError;
    unsigned leaders = 0; bool blockedCheckpoint = false;
    {
        OptimizerEngine engine; engine.setSettings(fault);
        engine.setCandidateCallback([&](const CandidateResult &r) {
            if (!r.isLeader || r.storage != "ram" || !r.verified) return;
            try {
                if (++leaders == 1) { oldLeader = r.tempArchivePath; oldSha = fileChecksum(oldLeader); }
                else if (!blockedCheckpoint) {
                    CHECK(fs::is_directory(journalDir) && !fs::is_empty(journalDir));
                    fs::rename(journalDir, savedJournal);
                    write(journalDir, "Checkpoint directory deliberately blocked by this test");
                    blockedCheckpoint = true; engine.cancel();
                }
            } catch (const std::exception &e) { callbackError = e.what(); engine.cancel(); }
        });
        CHECK(engine.start(smallArchive, temp.path / "checkpoint-cancelled.7z")); engine.wait();
        CHECK(callbackError.empty() && blockedCheckpoint && engine.getState() == EngineState::Cancelled);
    }
    CHECK(fs::is_regular_file(oldLeader) && fileChecksum(oldLeader) == oldSha);
    CHECK(fs::is_regular_file(journalDir)); fs::remove(journalDir); fs::rename(savedJournal, journalDir);
    SessionManager preserved; preserved.setSessionDir(journalDir);
    CHECK(!preserved.getSessionHistory().empty());
    {
        bool restoredCheckpoint = false;
        OptimizerEngine engine; engine.setSettings(fault);
        engine.setLogCallback([&](const std::string &line) {
            restoredCheckpoint |= line.find("[Session] Resumed") != std::string::npos;
        });
        CHECK(engine.start(smallArchive, temp.path / "checkpoint-restored.7z")); engine.wait();
        CHECK(engine.getState() == EngineState::Completed && restoredCheckpoint);
    }
    verify(temp.path / "checkpoint-restored.7z", temp.path / "checkpoint-restored-tree", smallManifest, tool);
    // Exhaust the real temporary budget after a verified RAM leader. The next
    // smaller Disk candidate is unchecked; it must not replace the saved journal
    // even when another verified worker would request a checkpoint.
    auto mixedFault = fault;
    mixedFault.customTempDir = temp.path / "mixed-checkpoint";
    std::shared_ptr<RamBudget> heldBudget;
    uint64_t heldBytes = 0;
    bool forcedDiskLeader = false, rejectedUncheckedCheckpoint = false;
    fs::path durableLeader; std::string durableSha, mixedError;
    {
        OptimizerEngine engine; engine.setSettings(mixedFault);
        engine.setCandidateCallback([&](const CandidateResult &r) {
            try {
                if (durableLeader.empty() && r.isLeader && r.verified && r.storage == "ram") {
                    durableLeader = r.tempArchivePath; durableSha = fileChecksum(durableLeader);
                    heldBudget = OptimizerEngineTestAccess::budget(engine);
                    heldBytes = mixedFault.ramTemporaryBudgetMB * 1048576ULL - heldBudget->used();
                    CHECK(heldBytes && heldBudget->acquire(heldBytes));
                } else if (r.isLeader && !r.verified && r.storage == "disk" && heldBudget) {
                    forcedDiskLeader = true;
                    rejectedUncheckedCheckpoint = !OptimizerEngineTestAccess::checkpoint(engine);
                    heldBudget->release(heldBytes); heldBytes = 0;
                    engine.cancel();
                }
            } catch (const std::exception &e) { mixedError = e.what(); engine.cancel(); }
        });
        CHECK(engine.start(smallArchive, temp.path / "mixed-cancelled.7z")); engine.wait();
        if (heldBytes) { heldBudget->release(heldBytes); heldBytes = 0; }
        CHECK(mixedError.empty() && forcedDiskLeader && rejectedUncheckedCheckpoint);
        CHECK(engine.getState() == EngineState::Cancelled);
    }
    CHECK(fs::is_regular_file(durableLeader) && fileChecksum(durableLeader) == durableSha);
    SessionManager mixedManager; mixedManager.setSessionDir(mixedFault.customTempDir / "sessions");
    const auto mixedHistory = mixedManager.getSessionHistory();
    CHECK(mixedHistory.size() == 1);
    const auto savedLeader = std::find_if(mixedHistory[0].results.begin(), mixedHistory[0].results.end(),
        [&](const auto &r) { return r.taskId == mixedHistory[0].leaderTaskId; });
    CHECK(savedLeader != mixedHistory[0].results.end() && savedLeader->verified);
    CHECK(savedLeader->tempArchivePath == durableLeader);
    {
        bool resumedMixed = false;
        OptimizerEngine engine; engine.setSettings(mixedFault);
        engine.setLogCallback([&](const std::string &line) {
            resumedMixed |= line.find("[Session] Resumed") != std::string::npos;
        });
        CHECK(engine.start(smallArchive, temp.path / "mixed-restored.7z")); engine.wait();
        CHECK(engine.getState() == EngineState::Completed && resumedMixed);
    }
    verify(temp.path / "mixed-restored.7z", temp.path / "mixed-restored-tree", smallManifest, tool);
    // A valid 256 MiB / eight-worker configuration leaves no room for input-cache
    // reservations. It must fall back rather than underflow the candidate budget.
    auto low = resumeSettings; low.ramLimitMB = 256; low.workerThreads = 8;
    low.profile = OptimizationProfile::WinRAR_Safe;
    low.customTempDir = temp.path / "low-scratch";
    validateSettings(low);
    {
        OptimizerEngine engine; engine.setSettings(low);
        CHECK(engine.start(smallArchive, temp.path / "low.7z")); engine.wait();
        if (engine.getState() != EngineState::Completed) throw std::runtime_error(engine.getLastError());
        const auto report = engine.getRunReport();
        CHECK(report["inputStorage"]["reservedMB"].get<uint64_t>() == 0);
        CHECK(report["inputStorage"]["ramBytes"].get<uint64_t>() == 0);
        CHECK(engine.getSnapshot().reservedRamMB <= low.ramLimitMB);
    }
    verify(temp.path / "low.7z", temp.path / "low-tree", smallManifest, tool);
}
int main(int argc, char **argv) {
    (void)argc;
#ifndef _WIN32
    (void)argv; return 77;
#else
    try { auto tool = tools(argv[0]); CHECK(!tool.empty()); contracts(tool);
        std::cout << "RAM engine contracts passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
#endif
}
