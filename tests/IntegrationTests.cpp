#include "core/InputManifest.hpp"
#include "core/GroupPlanner.hpp"
#include "core/OptimizerEngine.hpp"
#include "core/GroupCheckpoint.hpp"
#include <condition_variable>
#include "core/SettingsJson.hpp"
#include "core/StreamingVerifier.hpp"
#include "core/ToolCapabilities.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
using namespace UltraArch;
namespace fs = std::filesystem;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error(std::string("Check failed: ") + #condition);                  \
    } while (false)
struct Temp {
    fs::path path = fs::temp_directory_path() / ("ultraarch-integration-" + uniqueId());
    Temp() {
        fs::create_directories(path);
    }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
static fs::path executable;
static fs::path tool;
namespace UltraArch {
struct ResourceGuardTestAccess {
    static Json limits(const ResourceGuard &guard) {
#ifdef _WIN32
        std::lock_guard lock(guard.m_jobMutex);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        CHECK(QueryInformationJobObject(guard.m_jobHandle, JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits), nullptr));
        CHECK(QueryInformationJobObject(guard.m_jobHandle, JobObjectCpuRateControlInformation,
                                         &cpu, sizeof(cpu), nullptr));
        return Json{{"limitFlags", limits.BasicLimitInformation.LimitFlags},
            {"cpuFlags", cpu.ControlFlags}, {"cpuRate", cpu.CpuRate},
            {"jobLimit", uint64_t(limits.JobMemoryLimit)},
            {"processLimit", uint64_t(limits.ProcessMemoryLimit)}};
#else
        (void)guard;
        return Json::object();
#endif
    }
};
struct OptimizerEngineTestAccess {
    static std::shared_ptr<void> occupyTemporaryRam(OptimizerEngine &engine, uint64_t bytes) {
        auto budget = engine.m_ramBudget;
        CHECK(budget && budget->acquire(bytes));
        return std::shared_ptr<void>(nullptr, [budget, bytes](void *) { budget->release(bytes); });
    }
    static void groupedFinishPrecedence() {
        OptimizerEngine engine;
        engine.m_state = EngineState::TestingCandidates;
        engine.m_run.start = std::chrono::steady_clock::now();
        engine.m_best.taskId = 1;
        TaskConfig group; group.taskId = 2; group.groupedSearch = true;
        engine.m_tasks.push_back(group);
        CandidateResult queued; queued.status = CandidateStatus::Queued;
        engine.m_results.push_back(queued);
        engine.finishAndAssembleBest(); engine.finishAndAssembleBest();
        CHECK(engine.m_earlyAssembly);
        const auto stopped = engine.m_searchClosedElapsed;
        engine.pause();
        CHECK(!engine.m_pause && engine.m_state == EngineState::TestingCandidates);
        engine.resume();
        CHECK(!engine.startNextCandidate());
        CHECK(engine.m_stopAction == "AssembleBest" && engine.m_searchClosedElapsed == stopped);
        engine.m_state = EngineState::Completed;
    }
    static void phaseAccounting() {
        OptimizerEngine engine;
        engine.m_state = EngineState::TestingCandidates;
        engine.m_run.start = std::chrono::steady_clock::now();
        engine.m_phaseStart = engine.m_run.start - std::chrono::seconds(2);
        engine.pause(); engine.pause();
        CHECK(engine.m_phaseSeconds.at(static_cast<int>(EngineState::TestingCandidates)) >= 2);
        engine.m_phaseStart -= std::chrono::seconds(3);
        engine.resume(); engine.resume();
        CHECK(engine.m_phaseSeconds.at(static_cast<int>(EngineState::Paused)) >= 3);
        CHECK(engine.m_phaseSeconds.at(static_cast<int>(EngineState::TestingCandidates)) < 3);
        engine.m_state = EngineState::Completed;
        StageMetrics metrics;
        auto add = [&] { for (int i = 0; i < 100; ++i) metrics.add("Concurrent", 1); };
        auto other = std::async(std::launch::async, add); add(); other.get();
        try { StageMetrics::Scope failed(&metrics, "Failed"); throw std::runtime_error("test"); }
        catch (const std::runtime_error &) {}
        auto report = metrics.report();
        CHECK(report["stages"]["Concurrent"]["seconds"] == 200);
        CHECK(report["stages"]["Concurrent"]["calls"] == 200);
        CHECK(report["stages"]["Failed"]["calls"] == 1);
        CHECK(report["largestAccumulatedStage"] == "Concurrent");
        metrics.clear(); CHECK(metrics.report()["stages"].empty());
    }
    static void cancellationPrecedence() {
        for (int order : {0, 1, 2}) {
            OptimizerEngine engine;
            engine.m_state = EngineState::TestingCandidates;
            engine.m_run.sessionId = "cancellation-precedence";
            engine.m_run.start = std::chrono::steady_clock::now();
            engine.m_best.taskId = 1;
            TaskConfig active;
            active.taskId = 2; active.configId = "active-verification";
            engine.m_tasks.push_back(active);
            CandidateResult result;
            result.taskId = 2; result.configId = active.configId;
            result.status = CandidateStatus::Running; result.stage = "Verify";
            engine.m_results.push_back(result);
            engine.m_taskCancel.push_back(std::make_shared<std::atomic<bool>>(false));
            if (order == 1) engine.finishAndAssembleBest();
            if (order == 2) {
                std::promise<void> ready;
                auto gate = ready.get_future().share();
                auto cancel = std::async(std::launch::async, [&] { gate.wait(); engine.cancel(); });
                auto finish = std::async(std::launch::async, [&] { gate.wait(); engine.finishAndAssembleBest(); });
                auto skip = std::async(std::launch::async, [&] { gate.wait(); engine.skipCandidate("", active.configId); });
                ready.set_value();
                cancel.get(); finish.get(); skip.get();
            }
            engine.cancel();
            engine.cancel();
            engine.finishAndAssembleBest();
            CHECK(engine.m_stopAction == "Cancel");
            engine.pause();
            CHECK(engine.getState() != EngineState::Paused);
            engine.resume();
            CHECK(!engine.skipCandidate("", active.configId));
            CHECK(engine.m_stopAction == "Cancel");
            engine.m_state = EngineState::Completed;
            engine.m_cancel = false;
            engine.m_stopAction = "Published";
            engine.cancel(); engine.finishAndAssembleBest(); engine.pause(); engine.resume();
            CHECK(!engine.skipCandidate("", active.configId));
            CHECK(!engine.m_cancel && engine.m_stopAction == "Published");
        }
    }
    static void expireSearchBudget(OptimizerEngine &engine) {
        std::lock_guard lock(engine.m_mutex);
        // Advance only the budget clock after a real verified leader exists.
        // No sleep or machine-speed threshold is needed to exercise expiry.
        engine.m_run.start -= std::chrono::seconds(engine.m_run.settings.timeBudgetSeconds + 1);
    }
    static void exactOverridePreview(OptimizerSettings settings) {
        auto fixture = [&] {
            auto engine = std::make_unique<OptimizerEngine>();
            settings.fullAttemptLimit = 0;
            settings.ramLimitMB = 1000;
            engine->m_run.settings = settings;
            engine->m_run.start = std::chrono::steady_clock::now();
            engine->m_run.sessionId = "override-offer-" + uniqueId();
            engine->m_scheduler = {1000, 0, 4, 0};
            engine->m_broker = std::make_shared<AdmissionBroker>(1000, 4, 1);
            CHECK(engine->m_guard.initialize(1000));
            engine->m_state = EngineState::TestingCandidates;
            TaskConfig task;
            task.taskId = 1; task.configId = "exact-offer"; task.name = task.configId;
            task.estimatedRamMB = 1500; task.cpuTokens = 1;
            engine->m_tasks.push_back(task);
            CandidateResult result;
            result.taskId = 1; result.configId = task.configId; result.status = CandidateStatus::Deferred;
            engine->m_results.push_back(result);
            engine->m_taskCancel.push_back(std::make_shared<std::atomic<bool>>(false));
            return engine;
        };
        auto engine = fixture();
        const auto offer = engine->getSnapshot();
        CHECK(offer.memoryOverrideEligible && offer.memoryOverrideOffer.extraMiB == 500);
        CHECK(offer.memoryOverrideOffer.group.empty() && offer.memoryOverrideOffer.configId == "exact-offer");
        CHECK(!engine->requestMemoryOverride("", "exact-offer", 499));
        CHECK(engine->m_guard.getMemoryLimitMB() == 1000 && engine->m_overrideLeases.empty());
        auto cpuOccupied = engine->m_broker->tryAcquire("cpu-occupied", 0, 4, false, false);
        CHECK(cpuOccupied && !engine->getSnapshot().memoryOverrideEligible);
        CHECK(!engine->requestMemoryOverride("", "exact-offer", 500));
        cpuOccupied.reset();
        auto diskOccupied = engine->m_broker->tryAcquire("disk-occupied", 0, 0);
        CHECK(diskOccupied && !engine->getSnapshot().memoryOverrideEligible);
        CHECK(!engine->requestMemoryOverride("", "exact-offer", 500));
        diskOccupied.reset();
        CHECK(engine->m_guard.getMemoryLimitMB() == 1000 && engine->m_overrideLeases.empty());
        if (!ResourceGuard::hasMemoryHeadroom(1500)) {
            std::cout << "[SKIP] exact override admission needs 1500 MiB plus OS reserve\n";
            return;
        }
        CHECK(engine->requestMemoryOverride("", "exact-offer", 500));
        CHECK(engine->m_guard.getMemoryLimitMB() == 1500 && engine->m_overrideLeases.size() == 1);
        CHECK(engine->m_broker->snapshot().ramReserved == 1500);
        engine->cancel();
        CHECK(engine->m_overrideLeases.empty() && engine->m_broker->snapshot().ramReserved == 0);

        auto parent = fixture(), child = fixture();
        parent->m_tasks.clear(); parent->m_results.clear(); parent->m_taskCancel.clear();
        child->m_broker = parent->m_broker;
        child->m_sharedGuard = &parent->m_guard;
        parent->m_groupEngines["routed-group"] = child.get();
        struct UnlinkGroup {
            OptimizerEngine &parent;
            ~UnlinkGroup() { parent.m_groupEngines.clear(); }
        } unlink{*parent};
        const auto routed = parent->getSnapshot();
        CHECK(routed.memoryOverrideEligible && routed.memoryOverrideOffer.group == "routed-group");
        CHECK(routed.memoryOverrideOffer.extraMiB == 500);
        CHECK(!parent->requestMemoryOverride("unknown-group", "exact-offer", 500));
        CHECK(!parent->requestMemoryOverride("routed-group", "exact-offer", 499));
        CHECK(parent->requestMemoryOverride("routed-group", "exact-offer", 500));
        CHECK(parent->m_overrideLeases.empty() && child->m_overrideLeases.size() == 1);
        CHECK(parent->m_guard.getMemoryLimitMB() == 1500);
        child->cancel();
        CHECK(child->m_overrideLeases.empty() && parent->m_broker->snapshot().ramReserved == 0);
        parent->m_groupEngines.clear();
    }
    static void queuedOverrideWorker(const fs::path &input, const fs::path &workspace,
                                     OptimizerSettings settings, bool budgetAlreadySpent) {
        OptimizerEngine engine;
        fs::create_directories(workspace);
        settings.verifyArchives = false;
        settings.fullAttemptLimit = 1;
        settings.processTimeoutSeconds = 5;
        engine.m_run.settings = settings;
        engine.m_run.input = engine.m_run.effectiveInput = input;
        engine.m_run.workspace = workspace;
        engine.m_run.list = workspace / "inputs.txt";
        engine.m_run.sessionId = "override-worker-" + uniqueId();
        engine.m_run.start = std::chrono::steady_clock::now();
        engine.m_logPath = workspace / "worker.log";
        {
            std::ofstream list(engine.m_run.list, std::ios::binary);
            list << pathUtf8(input.filename()) << '\n';
        }
        ContentAnalyzer analyzer;
        engine.m_summary = analyzer.analyze(input);
        engine.m_scheduler = {1500, 0, 4, 0};
        engine.m_broker = std::make_shared<AdmissionBroker>(1000, 4, 2);
        CHECK(engine.m_guard.initialize(1000));
        engine.m_executorTarget = 1;
        engine.m_state = EngineState::TestingCandidates;
        for (int index = 0; index < 2; ++index) {
            TaskConfig task;
            task.taskId = index + 1;
            task.configId = index ? "late-override" : "earlier-normal";
            task.name = task.configId;
            task.flags = {"-mx0"};
            task.estimatedRamMB = index ? 1500 : 600;
            task.cpuTokens = 1;
            engine.m_tasks.push_back(task);
            CandidateResult result;
            result.taskId = task.taskId; result.configId = task.configId;
            engine.m_results.push_back(result);
            engine.m_taskCancel.push_back(std::make_shared<std::atomic<bool>>(false));
        }
        const auto key = engine.m_run.sessionId + ":late-override";
        auto pending = engine.m_broker->raiseAndAcquire(key, 1500, 1, 1500, [&] {
            return engine.m_guard.updateMemoryLimits(1500);
        });
        CHECK(pending);
        engine.m_overrideLeases[key] = std::move(pending);
        if (budgetAlreadySpent) engine.m_fullAttemptsStarted = 1;
        std::vector<std::string> finished;
        engine.setCandidateCallback([&](const CandidateResult &result) { finished.push_back(result.configId); });
        auto worker = std::async(std::launch::async, [&] { engine.workerFunc(0); });
        const bool completed = worker.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
        if (!completed) engine.cancel();
        worker.get();
        CHECK(completed);
        CHECK(engine.m_overrideLeases.empty());
        CHECK(engine.m_broker->snapshot().ramReserved == 0 && engine.m_broker->snapshot().active == 0);
        CHECK(engine.m_stopAction == "FullAttemptBudget");
        if (budgetAlreadySpent) {
            CHECK(finished.empty());
            CHECK(engine.m_results[1].status == CandidateStatus::Queued);
        } else {
            CHECK(finished.size() == 1 && finished.front() == "late-override");
            CHECK(engine.m_results[1].status == CandidateStatus::Passed);
            CHECK(engine.m_results[0].status == CandidateStatus::Queued);
        }
        engine.m_state = EngineState::Idle;
    }
};
}
static void write(const fs::path &file, const std::string &data) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(data.data(), data.size());
    CHECK(out.good());
}
static std::string randomBytes(size_t count) {
    std::mt19937 rng(42);
    std::string result(count, '\0');
    for (char &c : result)
        c = static_cast<char>(rng());
    return result;
}
static OptimizerSettings settingsFor(const Temp &temp) {
    OptimizerSettings s;
#ifdef _WIN32
    s.verificationMode = VerificationMode::Dual;
#endif
    s.profile = OptimizationProfile::WinRAR_Safe;
    s.workerThreads = 1;
    s.cpuThreads = 1;
    s.ramLimitMB = 256;
    s.processTimeoutSeconds = 15;
    s.customTempDir = temp.path / "scratch";
    s.toolsBinDir = tool.parent_path();
    return s;
}
static void run(OptimizerEngine &engine, const fs::path &input, const fs::path &output,
                const OptimizerSettings &settings) {
    engine.setSettings(settings);
    CHECK(engine.start(input, output));
    engine.wait();
    if (engine.getState() == EngineState::Failed)
        std::cerr << "Engine failed: " << engine.getLastError() << std::endl;
}
static void verifyExtracted(const fs::path &archive, const fs::path &original, const fs::path &dest,
                            const OptimizerSettings &s) {
    CHECK(ArchiveExtractor::extract(archive, dest, s.toolsBinDir, nullptr, nullptr, s.password,
                                    nullptr, 15));
    auto root = fs::is_directory(original) ? original : original.parent_path();
    ContentAnalyzer analyzer;
    auto summary = analyzer.analyze(original);
    for (const auto &file : summary.files)
        CHECK(fileChecksum(file.path) == fileChecksum(dest / file.path.lexically_relative(root)));
}
static void originalArchiveNeverGrows() {
    Temp temp;
    auto s = settingsFor(temp);
    auto input = temp.path / "source";
    write(input / "random.bin", randomBytes(65536));
    fs::create_directories(input / "empty");
    auto original = temp.path / "original.7z";
    auto compressed = ProcessManager::execute(pathUtf8(tool),
        {"a", "-t7z", "-mx0", pathUtf8(original), "."}, input);
    CHECK(compressed.success);
    auto before = fileChecksum(original);
    OptimizerEngine engine;
    run(engine, original, temp.path / "result.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(fs::file_size(temp.path / "result.7z") <= fs::file_size(original));
    CHECK(fileChecksum(original) == before);
    auto results = engine.getResults();
    CHECK(std::any_of(results.begin(), results.end(), [](const auto &r) {
        return r.name == "Original archive (preserve compression)" && r.verified && r.contentVerified;
    }));
    verifyExtracted(temp.path / "result.7z", input, temp.path / "restored", s);
}
static void similarityFromSingleTextGroup() {
    Temp temp;
    auto settings = settingsFor(temp);
    settings.profile = OptimizationProfile::Extreme_RePack;
    settings.searchMode = SearchMode::Fast;
    settings.enableGrouping = true;
    settings.groupedSearchOnly = true;
    settings.enableSimilarityGroups = true;
    settings.ramLimitMB = 512;
    settings.workerThreads = 2;
    settings.cpuThreads = 2;
    settings.diskTaskLimit = 0;
    const auto input = temp.path / utf8Path("исходные-日本語");
    const auto folderA = input / utf8Path("пара-α");
    const auto folderB = input / utf8Path("пара-β");
    // Exact duplicate pairs guarantee two clusters without relying on sketch
    // collisions or on the platform's filename sorting order.
    const std::string alpha(1048576, 'a'), beta(1048576, 'z');
    write(folderA / "one.txt", alpha); write(folderA / "two.txt", alpha);
    write(folderB / "one.txt", beta); write(folderB / "two.txt", beta);
    const auto empty = input / utf8Path("пустая-日本語");
    fs::create_directories(empty);
    const auto manifest = InputManifest::scan(input);
    CHECK(manifest.summary.totalBytes == 4ULL * 1048576);
    CHECK(GroupPlanner::plan(manifest.summary).size() == 1);
    CHECK(GroupPlanner::plan(manifest.summary, true).size() == 2);
    OptimizerEngine engine;
    const auto output = temp.path / "similarity.7z";
    run(engine, input, output, settings);
    CHECK(engine.getState() == EngineState::Completed);
    const auto results = engine.getResults();
    for (const auto *name : {"Grouped methods (verified assembly)",
                             "Content similarity groups (verified competitor)"}) {
        const auto candidate = std::find_if(results.begin(), results.end(), [&](const auto &result) {
            return result.name == name;
        });
        CHECK(candidate != results.end() && candidate->status == CandidateStatus::Verified);
    }
    const auto restored = temp.path / "restored";
    verifyExtracted(output, input, restored, settings);
    CHECK(fs::is_directory(restored / empty.lexically_relative(input)));
    CHECK(InputManifest::scan(restored).fingerprint == manifest.fingerprint);
}
static void groupedMethodsAndWrapping() {
    Temp temp;
    auto s = settingsFor(temp);
    s.profile = OptimizationProfile::Extreme_RePack;
    s.ramLimitMB = 1024;
    s.workerThreads = 2;
    s.cpuThreads = 2;
    s.diskTaskLimit = 0;
    s.cpuLimitPercent = 25;
    s.searchMode = SearchMode::Balanced;
    s.groupedSearchOnly = true;
    s.password = "группы 日本語";
    s.encryptHeaders = true;
    auto input = temp.path / "mixed";
    auto nested = input / std::string(80, 'a') / std::string(80, 'b') / std::string(80, 'c');
    write(nested / utf8Path("текст 日本語.lua"), std::string(2 * 1048576, 'a'));
    write(input / "audio.wav", "RIFF" + randomBytes(1150000));
    fs::create_directories(input / "empty");
    OptimizerEngine interrupted;
    std::atomic<bool> stopped{false};
    interrupted.setLogCallback([&](const auto &line) {
        if (line.rfind("[Group ready]", 0) == 0 && !stopped.exchange(true))
            interrupted.cancel();
    });
    run(interrupted, input, temp.path / "interrupted.7z", s);
    CHECK(interrupted.getState() == EngineState::Cancelled && stopped.load());
    CHECK(!fs::exists(temp.path / "interrupted.7z"));
    OptimizerEngine engine;
    std::atomic<bool> reused{false};
    engine.setLogCallback([&](const auto &line) {
        if (line.find("(verified cache)") != std::string::npos)
            reused = true;
    });
    run(engine, input, temp.path / "mixed.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(reused.load());
    auto results = engine.getResults();
    auto grouped = std::find_if(results.begin(), results.end(), [](const auto &r) {
        return r.name == "Grouped methods (verified assembly)";
    });
    CHECK(grouped != results.end() && grouped->status == CandidateStatus::Verified);
    verifyExtracted(temp.path / "mixed.7z", input, temp.path / "restored", s);
    CHECK(engine.getBestLeader().configId == grouped->configId);
    AssemblyPlan plan;
    plan.inputSourcePath = input;
    plan.bestCandidateArchive = temp.path / "mixed.7z";
    plan.reuseCandidate = true;
    plan.password = s.password;
    plan.encryptHeaders = true;
    plan.targetOutputPath = temp.path / "wrapped.exe";
    plan.createSfx = true;
    auto summary = InputManifest::scan(input).summary;
    CHECK(ArchiveAssembler::assembleFinalArchive(plan, summary, s, nullptr, nullptr, nullptr, nullptr));
    plan.targetOutputPath = temp.path / "split.7z";
    plan.createSfx = false;
    plan.volumeSizeMB = 1;
    CHECK(ArchiveAssembler::assembleFinalArchive(plan, summary, s, nullptr, nullptr, nullptr, nullptr));
    CHECK(fs::is_regular_file(temp.path / "split.7z.002"));
    verifyExtracted(temp.path / "split.7z.001", input, temp.path / "split-restored", s);
}
static void smallMediaAndRepetition() {
    Temp temp;
    auto s = settingsFor(temp);
    OptimizerEngine engine;
    auto bmp = temp.path / utf8Path("маленький рисунок.bmp");
    write(bmp, "BM" + std::string(2046, '\0'));
    run(engine, bmp, temp.path / "bmp.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    verifyExtracted(temp.path / "bmp.7z", bmp, temp.path / "bmp-extracted", s);
    auto file = temp.path / "repeated.bin";
    auto block = randomBytes(65536);
    std::string data;
    for (int i = 0; i < 64; ++i)
        data += block;
    write(file, data);
    run(engine, file, temp.path / "repeated.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(fs::file_size(temp.path / "repeated.7z") < 100000);
    verifyExtracted(temp.path / "repeated.7z", file, temp.path / "repeated-extracted", s);
}
static void encryptionVolumesAndManifest() {
    Temp temp;
    auto s = settingsFor(temp);
#ifdef _WIN32
    s.verificationMode = VerificationMode::Dual;
#endif
    s.password = "секрет 日本語 \\\" with slash\\";
    s.encryptHeaders = true;
    s.volumeSizeMB = 1;
    auto input = temp.path / utf8Path("папка с пробелами");
    write(input / "nested" / "payload.bin", randomBytes(2 * 1048576));
    write(input / "note.txt", "Hello\n");
    fs::create_directories(input / "empty-directory");
    OptimizerEngine engine;
    run(engine, input, temp.path / "split.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    auto eventsPath = utf8Path(engine.getRunReport().at("log").get<std::string>());
    eventsPath.replace_extension("events.jsonl");
    std::ifstream events(eventsPath);
    CHECK(events.good());
    std::string eventLine;
    while (std::getline(events, eventLine)) {
        auto event = Json::parse(eventLine);
        std::function<void(const Json &)> checkSecret = [&](const Json &value) {
            if (value.is_string()) CHECK(value.get<std::string>().find(s.password) == std::string::npos);
            else if (value.is_structured()) for (const auto &child : value) checkSecret(child);
        };
        checkSecret(event);
    }

    auto outputs = engine.getOutputFiles();
    CHECK(outputs.size() >= 3);
    for (const auto &file : outputs)
        CHECK(fs::file_size(file) <= 1048576);
    CHECK(!fs::exists(temp.path / "split.7z"));
    std::string error;
    CHECK(!ArchiveAssembler::verifyArchive(outputs.front(), s.toolsBinDir, error, "wrong", nullptr,
                                           nullptr, 5));
    verifyExtracted(outputs.front(), input, temp.path / "extracted", s);
    CHECK(fs::is_directory(temp.path / "extracted" / "empty-directory"));
#ifdef _WIN32
    AssemblyPlan badPassword;
    badPassword.inputSourcePath = input;
    badPassword.password = "wrong";
    auto encryptedSummary = InputManifest::scan(input).summary;
    CHECK(StreamingVerifier::verify(outputs.front(), badPassword, encryptedSummary, s, nullptr,
                                    nullptr, error) == StreamingVerifier::Result::Failed);
#endif
    for (const auto &result : engine.getResults())
        CHECK(result.flagsStr.find(s.password) == std::string::npos);
}
static void solidAndRepack() {
    Temp temp;
    auto s = settingsFor(temp);
    s.solidBlockSize = "off";
    s.enableAdaptivePruning = true;
    auto input = temp.path / "input";
    write(input / "nested" / "a.txt", std::string(32768, 'a'));
    write(input / "nested" / "b.txt", std::string(32768, 'b'));
    OptimizerEngine engine;
    run(engine, input, temp.path / "original.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    for (const auto &r : engine.getResults())
        CHECK(r.flagsStr.find("-ms=off") != std::string::npos);
    auto r = ProcessManager::execute(pathUtf8(tool),
                                     {"l", "-slt", pathUtf8(temp.path / "original.7z")}, temp.path);
    CHECK(r.success && r.stdOutput.find("Solid = -") != std::string::npos);
    s.enableAdaptivePruning = false;
    run(engine, temp.path / "original.7z", temp.path / "repacked.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    verifyExtracted(temp.path / "repacked.7z", input, temp.path / "extracted", s);
}
static void preserveOutputOnFailure() {
    Temp temp;
    auto s = settingsFor(temp);
    auto input = temp.path / "input.txt";
    write(input, "input");
    auto output = temp.path / "existing.7z";
    write(output, "old output bytes");
    auto originalChecksum = fileChecksum(output);
    ContentAnalyzer analyzer;
    auto summary = analyzer.analyze(input);
    AssemblyPlan plan;
    plan.inputSourcePath = input;
    plan.targetOutputPath = output;
    plan.winningConfig.flags = {"-invalid-option-for-test"};
    StageMetrics assemblyMetrics;
    CHECK(!ArchiveAssembler::assembleFinalArchive(plan, summary, s, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, &assemblyMetrics));
    CHECK(!assemblyMetrics.report()["stages"].contains("FinalVerification"));
    CHECK(assemblyMetrics.report()["stages"]["FinalStaging"]["calls"] == 1);
    CHECK(!assemblyMetrics.report()["stages"].contains("Publication"));
    CHECK(assemblyMetrics.report()["stages"]["PublicationRollback"]["calls"] == 1);
    CHECK(fileChecksum(output) == originalChecksum);
    OptimizerEngine engine;
    auto bad = temp.path / "output-directory";
    fs::create_directory(bad);
    run(engine, input, bad, s);
    CHECK(engine.getState() == EngineState::Failed);
    CHECK(engine.getOutputFiles().empty());
    CHECK(!engine.start(input, input));
    CHECK(!engine.start(temp.path, temp.path / "inside.7z"));
}
static void cancellationAndResumeCase(bool requireGain, bool ram) {
    Temp temp;
    auto s = settingsFor(temp);
    s.enableRamCandidates = ram;
    auto input = temp.path / "input.bin";
    write(input, requireGain ? std::string(65536, 'c') : randomBytes(65536));
    OptimizerEngine first;
    std::atomic<bool> cancelled{false};
    first.setCandidateCallback([&](const CandidateResult &r) {
        if (r.status == CandidateStatus::Verified &&
            (!requireGain || r.outputBytes < r.originalBytes / 2) && !cancelled.exchange(true)) {
            first.cancel();
            first.cancel();
            first.finishAndAssembleBest();
            first.pause();
            first.resume();
        }
    });
    run(first, input, temp.path / "cancelled.7z", s);
    CHECK(cancelled && first.getState() == EngineState::Cancelled);
    CHECK(first.getRunReport().at("stopAction") == "Cancel");
    CHECK(!fs::exists(temp.path / "cancelled.7z"));
    SessionManager manager;
    manager.setSessionDir(s.customTempDir / "sessions");
    CHECK(!manager.getSessionHistory().empty());
    const auto checkpoint = manager.getSessionHistory().front();
    CHECK(first.exportSessionLeader(checkpoint, temp.path / "checkpoint.7z", ""));
    verifyExtracted(temp.path / "checkpoint.7z", input, temp.path / "checkpoint-restored", s);
    const auto durableBytes = fs::file_size(temp.path / "checkpoint.7z");
    if (requireGain) CHECK(durableBytes < fs::file_size(input) / 2);
    OptimizerEngine second;
    std::atomic<bool> resumed{false};
    second.setLogCallback([&](const std::string &msg) {
        if (msg.find("[Session] Resumed") != std::string::npos)
            resumed = true;
    });
    run(second, input, temp.path / "resumed.7z", s);
    CHECK(second.getState() == EngineState::Completed);
    CHECK(resumed.load());
    if (requireGain) CHECK(fs::file_size(temp.path / "resumed.7z") <= durableBytes);
    if (!requireGain) CHECK(manager.getSessionHistory().empty());
    else {
        const auto report = second.getRunReport();
        std::cout << "[GAIN] " << (ram ? "RAM" : "Disk") << " checkpoint=" << durableBytes
                  << " published=" << fs::file_size(temp.path / "resumed.7z")
                  << " stop=" << report.at("stopAction") << std::endl;
        if (!manager.getSessionHistory().empty()) {
            CHECK(report.at("searchInterrupted").get<bool>() && report.at("sessionRetained").get<bool>());
            CHECK(second.exportSessionLeader(manager.getSessionHistory().front(), temp.path / "retained.7z", ""));
            verifyExtracted(temp.path / "retained.7z", input, temp.path / "retained-restored", s);
        }
    }
    verifyExtracted(temp.path / "resumed.7z", input, temp.path / "extracted", s);
}
static void cancellationAndResume() { cancellationAndResumeCase(false, false); }
static void compressionGainSurvivesCancellation() {
    cancellationAndResumeCase(true, false);
    cancellationAndResumeCase(true, true);
}
static void explicitFullAttemptBudget() {
    Temp temp;
    auto s = settingsFor(temp);
    s.enableGrouping = false;
    s.searchMode = SearchMode::Exhaustive;
    s.fullAttemptLimit = 1;
    s.workerThreads = 2;
    s.cpuThreads = 2;
    const auto input = temp.path / "budget.txt";
    write(input, std::string(65536, 'b'));
    OptimizerEngine engine;
    std::atomic<unsigned> started{0};
    engine.setLogCallback([&](const std::string &line) {
        if (line.rfind("[Candidate start] ", 0) == 0) ++started;
    });
    run(engine, input, temp.path / "bounded.7z", s);
    CHECK(engine.getState() == EngineState::Completed && started == 1);
    const auto report = engine.getRunReport();
    CHECK(report.at("searchInterrupted").get<bool>() && report.at("stopAction") == "FullAttemptBudget");
    CHECK(report.at("sessionRetained").get<bool>());
    const double elapsed = report.at("elapsedSeconds");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(engine.getRunReport().at("elapsedSeconds").get<double>() == elapsed);
    verifyExtracted(temp.path / "bounded.7z", input, temp.path / "restored", s);
    // A spent explicit budget must not launch a sample tournament afterwards.
    s.searchMode = SearchMode::Balanced;
    s.customTempDir = temp.path / "balanced-scratch";
    OptimizerEngine balanced;
    bool probed = false;
    balanced.setLogCallback([&](const std::string &line) {
        if (line.find("[Probe]") != std::string::npos) probed = true;
    });
    run(balanced, input, temp.path / "balanced.7z", s);
    CHECK(balanced.getState() == EngineState::Completed && !probed);
    CHECK(balanced.getRunReport().at("stopAction") == "FullAttemptBudget");
}
static void timeBudgetPublication() {
    for (bool repeatedFinish : {true, false}) {
        Temp temp;
        auto s = settingsFor(temp);
        s.enableGrouping = false;
        s.searchMode = SearchMode::Exhaustive;
        s.workerThreads = 1;
        s.timeBudgetSeconds = 3600;
        const auto input = temp.path / "budget.txt";
        write(input, std::string(65536, 'b'));
        const auto original = fileChecksum(input);
        OptimizerEngine engine;
        bool expired = false, finishSent = false;
        unsigned starts = 0, startsAtExpiry = 0, assemblies = 0;
        engine.setLogCallback([&](const std::string &line) {
            if (line.rfind("[Candidate start] ", 0) == 0) {
                ++starts;
                if (starts == 1) engine.pause();
            }
            if (repeatedFinish && line.rfind("[Search] Time budget", 0) == 0) {
                engine.finishAndAssembleBest();
                engine.finishAndAssembleBest();
                finishSent = true;
            }
        });
        engine.setStateCallback([&](EngineState state) {
            if (state == EngineState::AssemblingFinal) ++assemblies;
        });
        engine.setCandidateCallback([&](const CandidateResult &result) {
            if (!expired && result.status == CandidateStatus::Verified) {
                expired = true;
                startsAtExpiry = starts;
                OptimizerEngineTestAccess::expireSearchBudget(engine);
                engine.resume();
            }
        });
        run(engine, input, temp.path / "output.7z", s);
        CHECK(expired && starts == startsAtExpiry && assemblies == 1);
        CHECK(finishSent == repeatedFinish);
        CHECK(engine.getState() == EngineState::Completed);
        const auto report = engine.getRunReport();
        CHECK(report.at("stopAction") == "TimeBudget");
        CHECK(report.at("searchCompletion") == "Interrupted");
        CHECK(report.at("publicationOutcome") == "CandidatePublished");
        CHECK(report.at("sessionRetained").get<bool>());
        const auto stages = report.at("operationTiming").at("stages");
        for (const auto *name : {"CandidateEncoding", "CandidateVerification", "Checkpoint",
             "FinalStaging", "FinalVerification", "Publication", "ResourceAdmission"}) {
            CHECK(stages.at(name).at("calls").get<unsigned>() > 0);
            CHECK(stages.at(name).at("seconds").get<double>() >= 0);
        }
        CHECK(report.at("resources").at("jobAccountingAvailable").get<bool>());
        CHECK(report.at("resources").at("processes").get<unsigned>() > 0);
        CHECK(!report.at("resources").at("applicationLogicalIO").is_null());
        auto eventsPath = utf8Path(report.at("log").get<std::string>());
        eventsPath.replace_extension("events.jsonl");
        std::ifstream events(eventsPath);
        CHECK(events.good());
        unsigned finishedEvents = 0;
        std::string line;
        while (std::getline(events, line)) {
            const auto event = Json::parse(line);
            if (event.at("stage") == "Candidate" && event.at("action") == "Finished") {
                ++finishedEvents;
                CHECK(event.at("details").contains("storage"));
                CHECK(event.at("details").contains("gainBytes"));
                CHECK(event.at("details").contains("admission"));
            }
        }
        CHECK(finishedEvents == starts);
        CHECK(report.at("searchClosedAtSeconds").get<double>() >= 0);
        CHECK(report.at("searchClosedAtSeconds").get<double>() <= report.at("finalization").at("startedAtSeconds").get<double>());
        const auto timing = report.at("finalization");
        CHECK(timing.at("startedAtSeconds").get<double>() >= 0);
        CHECK(timing.at("finishedAtSeconds").get<double>() >= timing.at("startedAtSeconds").get<double>());
        CHECK(timing.at("seconds").get<double>() >= 0);
        engine.finishAndAssembleBest();
        CHECK(engine.getRunReport().at("finalization") == timing);
        CHECK(engine.getRunReport().at("stopAction") == "TimeBudget");
        CHECK(fileChecksum(input) == original);
        verifyExtracted(temp.path / "output.7z", input, temp.path / "restored", s);
        // The public history API resolves the engine's actual session location.
        const auto sessions = engine.sessions(s.customTempDir);
        CHECK(!sessions.empty());
        CHECK(sessions.front().leaderTaskId > 0);
        const auto leader = engine.getBestLeader();
        CHECK(leader.verified && fs::is_regular_file(leader.tempArchivePath));
        CHECK(engine.exportSessionLeader(sessions.front(), temp.path / "checkpoint.7z", ""));
        verifyExtracted(temp.path / "checkpoint.7z", input, temp.path / "checkpoint-restored", s);
    }
}
static void scopedCandidateSkipAndManualAdmission() {
    Temp temp;
    auto s = settingsFor(temp);
    s.enableGrouping = false;
    s.workerThreads = 1;
    s.cpuThreads = 1;
    const auto input = temp.path / "input.txt";
    write(input, std::string(65536, 'a'));
    OptimizerEngine engine;
    std::atomic<bool> skipped{false}, skipAccepted{false}, admissionChecked{false};
    std::atomic<bool> hardLimitRespected{true};
    std::string skippedId;
    engine.setLogCallback([&](const auto &line) {
        if (line.rfind("[Candidate start] ", 0) != 0) return;
        const auto snapshot = engine.getSnapshot();
        if (snapshot.reservedCpu > s.cpuThreads || snapshot.reservedRamMB > s.ramLimitMB)
            hardLimitRespected = false;
        // The single CPU token is already leased before the start callback.
        if (!admissionChecked.exchange(true) && engine.startNextCandidate())
            hardLimitRespected = false;
        if (line.find("Store (") == std::string::npos && !skipped.exchange(true)) {
            skippedId = line.substr(line.rfind(" / ") + 3);
            skipAccepted = engine.skipCandidate("", skippedId);
            CHECK(engine.skipCandidate("", skippedId)); // Repeating the same command cannot skip a sibling.
        }
    });
    run(engine, input, temp.path / "output.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(skipped.load() && skipAccepted.load() && admissionChecked.load() && hardLimitRespected.load());
    unsigned skippedCount = 0, verifiedSiblings = 0;
    for (const auto &result : engine.getResults()) {
        if (result.configId == skippedId) CHECK(result.status == CandidateStatus::Skipped);
        if (result.status == CandidateStatus::Skipped) ++skippedCount;
        if (result.configId != skippedId && result.status == CandidateStatus::Verified) ++verifiedSiblings;
    }
    CHECK(skippedCount == 1 && verifiedSiblings > 0);
    const auto report = engine.getRunReport();
    CHECK(report.at("searchInterrupted").get<bool>() && report.at("stopAction") == "SkipCandidate");
    CHECK(report.at("publicationOutcome") == "CandidatePublished");
    verifyExtracted(temp.path / "output.7z", input, temp.path / "restored", s);
}
static void ramExecutorCase(bool forceDiskFallback) {
    Temp temp;
    auto settings = settingsFor(temp);
    settings.enableRamCandidates = true;
    settings.enableGrouping = false;
    settings.ramLimitMB = 1024;
    settings.workerThreads = 2; settings.cpuThreads = 2;
    settings.diskTaskLimit = 1; settings.maxLzmaDictionaryMB = 1;
    const auto input = temp.path / "input.txt";
    write(input, std::string(65536, 'r'));
    OptimizerEngine engine;
    std::mutex mutex;
    std::condition_variable gate;
    unsigned starts = 0;
    bool overlapped = false, timedOut = false;
    std::shared_ptr<void> heldBudget;
    engine.setLogCallback([&](const std::string &line) {
        if (line.rfind("[Candidate start] ", 0) != 0) return;
        std::unique_lock lock(mutex);
        if (++starts == 1) {
            if (forceDiskFallback) heldBudget = OptimizerEngineTestAccess::occupyTemporaryRam(engine, settings.ramTemporaryBudgetMB * 1048576);
            if (!gate.wait_for(lock, std::chrono::seconds(5), [&] { return starts >= 2; })) timedOut = true;
        } else if (starts == 2) {
            overlapped = true;
            gate.notify_all();
        }
    });
    run(engine, input, temp.path / "output.7z", settings);
    CHECK(engine.getState() == EngineState::Completed && overlapped && !timedOut);
    unsigned ram = 0;
    for (const auto &result : engine.getResults()) if (result.storage == "ram" && result.verified) ++ram;
    if (forceDiskFallback) {
        CHECK(ram == 0);
        unsigned fallbacks = 0;
        for (const auto &result : engine.getResults())
            if (result.storageFallback == "Shared temporary RAM budget occupied") ++fallbacks;
        CHECK(fallbacks >= 2);
    } else CHECK(ram >= 2);
    const auto admission = engine.getRunReport().at("resources").at("admission");
    CHECK(admission.at("peakExecutors").get<unsigned>() == 2);
    CHECK(admission.at("peakDiskTasks").get<unsigned>() <= 1);
    CHECK(admission.at("peakPlannedTotalMB").get<uint64_t>() <= settings.ramLimitMB);
    CHECK(admission.at("remainingWaiters").get<unsigned>() == 0);
    verifyExtracted(temp.path / "output.7z", input, temp.path / "restored", settings);
}
static void ramExecutorsRespectDiskLimit() { ramExecutorCase(false); }
static void diskFallbackAdmission() { ramExecutorCase(true); }
static void smallGroupBackfillsIdleExecutors() {
    Temp temp;
    auto settings = settingsFor(temp);
    settings.profile = OptimizationProfile::Extreme_RePack;
    settings.searchMode = SearchMode::Fast;
    settings.groupedSearchOnly = true;
    settings.workerThreads = 2; settings.cpuThreads = 4;
    settings.ramLimitMB = 512; settings.fullAttemptLimit = 1;
    settings.maxLzmaDictionaryMB = 1;
    const auto input = temp.path / "input";
    const std::string payload(64ULL * 1048576, 'z');
    write(input / "one.largea", payload); write(input / "two.largeb", payload);
    write(input / "small.txt", std::string(3 * 1048576, 'a'));
    const auto manifest = InputManifest::scan(input);
    CHECK(GroupPlanner::plan(manifest.summary).size() == 3);
    std::mutex mutex;
    std::condition_variable gate;
    bool largeStarted = false, smallStarted = false, timedOut = false;
    std::atomic<bool> largeClaimed{false}, smallClaimed{false};
    OptimizerEngine engine;
    engine.setLogCallback([&](const auto &line) {
        if (line.find("] [Candidate start] Store") == std::string::npos) return;
        if ((line.find(".largea]") != std::string::npos || line.find(".largeb]") != std::string::npos) &&
            !largeClaimed.exchange(true)) {
            std::unique_lock lock(mutex);
            largeStarted = true; gate.notify_all();
            if (!gate.wait_for(lock, std::chrono::seconds(10), [&] { return smallStarted; })) timedOut = true;
        } else if (line.find(".txt]") != std::string::npos && !smallClaimed.exchange(true)) {
            std::unique_lock lock(mutex);
            if (!gate.wait_for(lock, std::chrono::seconds(10), [&] { return largeStarted; })) timedOut = true;
            smallStarted = true; gate.notify_all();
        }
    });
    engine.setSettings(settings);
    CHECK(engine.start(input, temp.path / "output.7z"));
    bool badReservation = false;
    while (engine.getState() != EngineState::Completed && engine.getState() != EngineState::Failed &&
           engine.getState() != EngineState::Cancelled) {
        const auto snapshot = engine.getSnapshot();
        badReservation |= snapshot.reservedRamMB > settings.ramLimitMB ||
            snapshot.reservedCpu > settings.cpuThreads || snapshot.activeTasks > settings.workerThreads;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.wait();
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(largeStarted && smallStarted && !timedOut && !badReservation);
    verifyExtracted(temp.path / "output.7z", input, temp.path / "restored", settings);
    CHECK(InputManifest::scan(temp.path / "restored").fingerprint == manifest.fingerprint);
}
static void skippedGroupRetainedAndEarlyAssembly() {
    Temp temp;
    auto s = settingsFor(temp);
    s.profile = OptimizationProfile::Extreme_RePack;
    s.searchMode = SearchMode::Fast;
    s.enableGrouping = true;
    s.groupedSearchOnly = true;
    s.ramLimitMB = 512;
    s.immediateCleanInferior = false; // Retain the complete grouped fallback even if the original is smaller.
    const auto input = temp.path / "input";
    write(input / "one.txt", std::string(5 * 262144, 'a'));
    write(input / "two.lua", std::string(5 * 262144, 'a'));
    fs::create_directories(input / "empty" / "nested");
    const auto manifest = InputManifest::scan(input);
    CHECK(GroupPlanner::plan(manifest.summary).size() == 2);
    const auto original = temp.path / "original.7z";
    CHECK(ProcessManager::execute(pathUtf8(tool),
        {"a", "-y", "-t7z", "-mx9", pathUtf8(original), "."}, input).success);
    const auto originalHash = fileChecksum(original);
    OptimizerEngine first;
    std::string completedGroup;
    std::mutex groupGate;
    std::atomic<bool> skipAttempted{false}, skipAccepted{false};
    first.setLogCallback([&](const auto &line) {
        std::lock_guard gate(groupGate);
        if (line.rfind("[Group ready] ", 0) == 0) {
            const auto keyStart = std::strlen("[Group ready] ");
            completedGroup = line.substr(keyStart, line.find(": ", keyStart) - keyStart);
        }
        if (!completedGroup.empty() && line.rfind("[Group ", 0) == 0 &&
            line.find("] [Candidate start] ") != std::string::npos && !skipAttempted.exchange(true)) {
            const auto group = line.substr(7, line.find(']') - 7);
            skipAccepted = first.skipGroup(group);
        }
    });
    const auto output = temp.path / "skipped.7z";
    run(first, original, output, s);
    CHECK(first.getState() == EngineState::Completed);
    CHECK(!completedGroup.empty() && skipAttempted.load() && skipAccepted.load());
    auto report = first.getRunReport();
    CHECK(report.at("searchInterrupted").get<bool>() && report.at("stopAction") == "SkipGroup");
    CHECK(report.at("publicationOutcome") == "OriginalPublished");
    CHECK(report.at("sessionRetained").get<bool>());
    CHECK(report.at("groupsTotal") == 2 && report.at("groupsCompleted") == 1);
    CHECK(fileChecksum(output) == originalHash && fileChecksum(original) == originalHash);
    SessionManager manager;
    manager.setSessionDir(s.customTempDir / "sessions");
    const auto sessions = manager.getSessionHistory();
    CHECK(!sessions.empty());
    const auto groupRoot = sessions.front().workspace / "groups" / Hasher::text(completedGroup).substr(0, 16);
    CHECK(fs::is_regular_file(groupRoot / "winner.json"));
    Json marker; std::ifstream(groupRoot / "winner.json") >> marker;
    CHECK(marker.at("searchComplete").get<bool>());
    CHECK(marker.at("sha256") == fileChecksum(GroupCheckpoint::artifact(groupRoot, marker)));

    // The next run reuses the completed group, then requests an early complete publication.
    OptimizerEngine second;
    std::atomic<bool> reused{false}, assembledEarly{false};
    second.setLogCallback([&](const auto &line) {
        if (line.find(completedGroup + ":") != std::string::npos &&
            line.find("(verified cache)") != std::string::npos && !reused.exchange(true)) {
            second.finishAndAssembleBest();
            assembledEarly = true;
        }
    });
    const auto earlyOutput = temp.path / "early.7z";
    run(second, original, earlyOutput, s);
    CHECK(second.getState() == EngineState::Completed && reused.load() && assembledEarly.load());
    report = second.getRunReport();
    CHECK(report.at("searchInterrupted").get<bool>() && report.at("stopAction") == "AssembleBest");
    CHECK(report.at("sessionRetained").get<bool>());
    CHECK(fs::file_size(earlyOutput) <= fs::file_size(original));
    verifyExtracted(earlyOutput, input, temp.path / "restored", s);
    CHECK(InputManifest::scan(temp.path / "restored").fingerprint == manifest.fingerprint);
    CHECK(!manager.getSessionHistory().empty());

    // A valid grouped wrapper from early assembly is not a completed method
    // search. Resume must keep its durable archive while finishing the missing
    // group search, rather than skip the Verified wrapper and delete its groups.
    const auto earlyResults = second.getResults();
    for (const auto &candidate : earlyResults)
        if (candidate.name == "Grouped methods (verified assembly)" && candidate.status != CandidateStatus::Verified)
            std::cerr << "Early grouped failure: " << candidate.statusMessage << " status=" << static_cast<int>(candidate.status) << '\n';
    CHECK(std::any_of(earlyResults.begin(), earlyResults.end(), [](const auto &result) {
        return result.name == "Grouped methods (verified assembly)" &&
               result.status == CandidateStatus::Verified;
    }));
    OptimizerEngine third;
    std::atomic<bool> continued{false}, reusedAgain{false};
    third.setLogCallback([&](const auto &line) {
        if (line.find("unfinished group searches queued") != std::string::npos)
            continued = true;
        if (line.find("(verified cache)") != std::string::npos) reusedAgain = true;
        if (line.find("] [Candidate start] ") != std::string::npos) continued = true;
    });
    const auto resumedOutput = temp.path / "continued.7z";
    run(third, original, resumedOutput, s);
    if (third.getState() != EngineState::Completed || !continued.load() || !reusedAgain.load())
        std::cerr << "Third group resume: state=" << static_cast<int>(third.getState()) <<
            " continued=" << continued.load() << " reused=" << reusedAgain.load() <<
            " error=" << third.getLastError() << std::endl;
    CHECK(third.getState() == EngineState::Completed && continued.load() && reusedAgain.load());
    report = third.getRunReport();
    CHECK(report.at("groupsTotal") == 2 && report.at("groupsCompleted") == 2);
    CHECK(report.at("sessionRetained").get<bool>());
    verifyExtracted(resumedOutput, input, temp.path / "continued-extracted", s);
    CHECK(InputManifest::scan(temp.path / "continued-extracted").fingerprint == manifest.fingerprint);
}
static void inputMutation() {
    Temp temp;
    auto s = settingsFor(temp);
    auto input = temp.path / "input.txt";
    write(input, std::string(4096, 'a'));
    auto output = temp.path / "out.7z";
    write(output, "preserve me");
    auto hash = fileChecksum(output);
    OptimizerEngine engine;
    std::atomic<bool> changed{false};
    engine.setCandidateCallback([&](const auto &r) {
        if (r.status == CandidateStatus::Verified && !changed.exchange(true)) {
            std::ofstream out(input, std::ios::app);
            out << 'b';
        }
    });
    run(engine, input, output, s);
    CHECK(engine.getState() == EngineState::Failed);
    CHECK(fileChecksum(output) == hash);
}
static void deepMatrixUsesValidFlags() {
    Temp temp;
    auto s = settingsFor(temp);
    s.profile = OptimizationProfile::DeepSearch;
    s.ramLimitMB = 512;
    s.workerThreads = 2;
    std::string pe(2048, '\0');
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[0x3c] = char(0x80);
    pe[0x80] = 'P';
    pe[0x81] = 'E';
    pe[0x84] = char(0x64);
    pe[0x85] = char(0x86);
    auto input = temp.path / "input";
    write(input / "program.exe", pe);
    write(input / "data.txt", std::string(65536, 'a'));
    write(input / "image.bmp", "BM" + std::string(8192, '\0'));
    OptimizerEngine engine;
    run(engine, input, temp.path / "output.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    bool bcj2 = false, deep = false, ppmd = false, delta = false;
    for (const auto &result : engine.getResults()) {
        if (result.status != CandidateStatus::Verified && result.status != CandidateStatus::DiscardedBySize &&
            result.status != CandidateStatus::Deferred)
            throw std::runtime_error(result.name + ": " + result.statusMessage);
        bcj2 |= result.name.find("BCJ2") != std::string::npos;
        deep |= result.name.find("bt5") != std::string::npos;
        ppmd |= result.name.find("PPMd") != std::string::npos;
        delta |= result.name.find("Delta") != std::string::npos;
    }
    CHECK(bcj2 && deep && ppmd && delta);
    verifyExtracted(temp.path / "output.7z", input, temp.path / "restored", s);
}
static void parallelFinishWhilePaused() {
    Temp temp;
    auto s = settingsFor(temp);
    s.workerThreads = 2;
    s.ramLimitMB = 512;
    write(temp.path / "input.txt", std::string(1048576, 'a'));
    OptimizerEngine engine;
    std::atomic<bool> stopped{false};
    engine.setCandidateCallback([&](const CandidateResult &result) {
        if (result.status == CandidateStatus::Verified && !stopped.exchange(true)) {
            engine.pause();
            engine.finishAndAssembleBest();
            engine.finishAndAssembleBest();
        }
    });
    run(engine, temp.path / "input.txt", temp.path / "output.7z", s);
    CHECK(stopped.load());
    CHECK(engine.getState() == EngineState::Completed);
    bool skipped = false;
    for (const auto &result : engine.getResults()) {
        CHECK(result.status != CandidateStatus::Queued &&
              result.status != CandidateStatus::Running);
        skipped |= result.status == CandidateStatus::Skipped;
    }
    CHECK(skipped);
    CHECK(engine.getCompletedCount() == engine.getTotalCount());
    verifyExtracted(temp.path / "output.7z", temp.path / "input.txt", temp.path / "restored", s);
}
static void verifyContentsAndCallbackSafety() {
    Temp temp;
    auto s = settingsFor(temp);
    auto input = temp.path / "input" / "source.bin";
    auto other = temp.path / "other" / "source.bin";
    write(input, std::string(4096, 'a'));
    write(other, std::string(4096, 'b'));
    auto candidate = temp.path / "candidate.7z";
    CHECK(ProcessManager::execute(pathUtf8(tool),
                                  {"a", "-t7z", "-mx0", pathUtf8(candidate), "source.bin"},
                                  other.parent_path())
              .success);
    ContentAnalyzer analyzer;
    auto summary = analyzer.analyze(input);
    summary.files.front().contentChecksum = fileChecksum(input);
    AssemblyPlan plan;
    plan.inputSourcePath = input;
    plan.targetOutputPath = temp.path / "output.7z";
    plan.bestCandidateArchive = candidate;
    plan.reuseCandidate = true;
    write(plan.targetOutputPath, "original output");
    auto original = fileChecksum(plan.targetOutputPath);
    StageMetrics assemblyMetrics;
    CHECK(!ArchiveAssembler::assembleFinalArchive(plan, summary, s, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, &assemblyMetrics));
    CHECK(assemblyMetrics.report()["stages"]["FinalVerification"]["calls"] == 1);
    CHECK(!assemblyMetrics.report()["stages"].contains("Publication"));
    CHECK(assemblyMetrics.report()["stages"]["PublicationRollback"]["calls"] == 1);
    CHECK(fileChecksum(plan.targetOutputPath) == original);
    fs::remove(candidate);
    CHECK(ProcessManager::execute(pathUtf8(tool),
                                  {"a", "-t7z", "-mx0", pathUtf8(candidate), "source.bin"},
                                  input.parent_path())
              .success);
    std::string assemblyLog;
    const bool assembled = ArchiveAssembler::assembleFinalArchive(
        plan, summary, s, nullptr, nullptr,
        [&](const auto &line) {
            assemblyLog += line + '\n';
            throw std::runtime_error("UI callback");
        },
        [](float) { throw std::runtime_error("UI callback"); }, nullptr, &assemblyMetrics);
    CHECK(assemblyMetrics.report()["stages"]["FinalVerification"]["calls"] == 2);
    CHECK(assemblyMetrics.report()["stages"]["Publication"]["calls"] == 1);
    if (!assembled)
        throw std::runtime_error("Assembly with throwing callbacks failed: " + assemblyLog);
    verifyExtracted(plan.targetOutputPath, input, temp.path / "restored", s);
}
static void processControl() {
    Temp temp;
    std::atomic<bool> cancel{false};
    ProcessControl timeout{nullptr, 1};
    auto start = std::chrono::steady_clock::now();
    auto timed = ProcessManager::execute(pathUtf8(executable), {"--helper-sleep"}, temp.path,
                                         nullptr, 0, nullptr, nullptr, &timeout);
    CHECK(!timed.success && timed.timedOut);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    std::thread stop([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel = true;
    });
    auto cancelled = ProcessManager::execute(pathUtf8(executable), {"--helper-sleep"}, temp.path,
                                             nullptr, 0, &cancel);
    stop.join();
    CHECK(cancelled.cancelled && !cancelled.success);
    auto echo = ProcessManager::execute(
        pathUtf8(executable),
        {"--helper-echo", "spaces and \\\"quotes\\\"", "C:\\folder with spaces\\"}, temp.path);
    CHECK(echo.success);
    Json args = Json::parse(echo.stdOutput);
    CHECK(args[0] == "spaces and \\\"quotes\\\"" && args[1] == "C:\\folder with spaces\\");
    ProcessControl lifecycle;
    std::vector<std::pair<uint32_t, std::string>> events;
    lifecycle.lifecycleCallback = [&](uint32_t pid, const char *event) { events.emplace_back(pid, event); };
    const auto streams = ProcessManager::execute(pathUtf8(executable), {"--helper-streams"}, temp.path,
                                                 nullptr, 0, nullptr, nullptr, &lifecycle);
    CHECK(streams.success && streams.pid != 0);
    CHECK(events.size() == 2 && events[0].first == streams.pid && events[1].first == streams.pid);
    CHECK(events[0].second == "Start" && events[1].second == "Finish");
    CHECK(streams.stdOutput.find("stdout-only") != std::string::npos);
#ifdef _WIN32
    CHECK(streams.stdOutput.find("stderr-only") == std::string::npos);
    CHECK(streams.errOutput.find("stderr-only") != std::string::npos);
    CHECK(streams.errOutput.find("stdout-only") == std::string::npos);
#endif
#ifdef _WIN32
    ResourceGuard guard;
    CHECK(guard.initialize(64));
    auto oom =
        ProcessManager::execute(pathUtf8(executable), {"--helper-allocate"}, temp.path, &guard);
    CHECK(!oom.success && oom.killedByOOM);
    auto nested = ProcessManager::execute(pathUtf8(executable), {"--helper-nested-job"},
                                           temp.path, &guard);
    CHECK(nested.success);
    {
        ResourceGuard updating;
        CHECK(updating.initialize(256) && updating.setCpuRate(50));
        std::atomic<bool> stopHelper{false};
        std::atomic<uint32_t> helperPid{0};
        ProcessControl helperControl{&stopHelper, 15};
        helperControl.lifecycleCallback = [&](uint32_t pid, const char *event) {
            if (std::string(event) == "Start") helperPid.store(pid);
        };
        auto helper = std::async(std::launch::async, [&] {
            return ProcessManager::execute(pathUtf8(executable),
                {"--helper-job-update", pathUtf8(temp.path)}, temp.path, &updating,
                0, nullptr, nullptr, &helperControl);
        });
        struct StopHelper {
            std::atomic<bool> &stop;
            ~StopHelper() { stop.store(true); }
        } stopOnExit{stopHelper};
        auto waitMarker = [&](const fs::path &marker) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!fs::exists(marker) && std::chrono::steady_clock::now() < deadline &&
                   helper.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(fs::exists(marker));
        };
        waitMarker(temp.path / "job-before-ready");
        const auto before = ResourceGuardTestAccess::limits(updating);
        CHECK(updating.getCurrentMemoryMB() >= 128);
        CHECK(updating.updateMemoryLimits(384) && updating.getMemoryLimitMB() == 384);
        CHECK(!updating.updateMemoryLimits(64) && updating.getMemoryLimitMB() == 384);
        CHECK(!updating.updateMemoryLimits(UINT64_MAX) && updating.getMemoryLimitMB() == 384);
        write(temp.path / "job-query-after", "query");
        waitMarker(temp.path / "job-after-ready");
        const auto after = ResourceGuardTestAccess::limits(updating);
        CHECK(helperPid.load() != 0);
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, helperPid.load());
        CHECK(child != nullptr);
        const auto childState = WaitForSingleObject(child, 0);
        CloseHandle(child);
        CHECK(childState == WAIT_TIMEOUT); // Updating the existing Job must not kill its child.
        CHECK(before["limitFlags"] == after["limitFlags"]);
        CHECK((after["limitFlags"].get<DWORD>() & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0);
        CHECK(before["cpuFlags"] == after["cpuFlags"] && before["cpuRate"] == after["cpuRate"]);
        CHECK(after["cpuRate"] == 5000);
        CHECK(before["jobLimit"] == 256ULL * 1048576 && before["processLimit"] == 256ULL * 1048576);
        CHECK(after["jobLimit"] == 384ULL * 1048576 && after["processLimit"] == 384ULL * 1048576);
        stopHelper.store(true);
        CHECK(helper.get().cancelled);
        CHECK(updating.updateMemoryLimits(256) && updating.getMemoryLimitMB() == 256);
    }
    const auto processors = ResourceGuard::detectCpuTopology().logicalProcessors;
    if (processors < 80) {
        ResourceGuard limited;
        CHECK(limited.initialize(64) && limited.setCpuRate(1));
        auto capped = ProcessManager::execute(pathUtf8(executable), {"--helper-cpu"}, temp.path, &limited);
        CHECK(capped.success);
        auto cpu = limited.getCpuSeconds();
        CHECK(cpu > 0 && cpu <= capped.durationSeconds * processors * 0.01 * 1.35 + 0.2);
        std::cout << "[CPU cap] " << cpu << " CPU seconds / " << capped.durationSeconds
                  << " wall seconds; " << processors << " logical processors\n";
    }
#endif
}
static void queuedOverrideDispatchAndBudgetCleanup() {
#ifdef _WIN32
    Temp temp;
    const auto input = temp.path / "tiny.txt";
    write(input, "A tiny actual compressor input for deterministic override dispatch.\n");
    auto settings = settingsFor(temp);
    OptimizerEngineTestAccess::queuedOverrideWorker(input, temp.path / "dispatch", settings, false);
    OptimizerEngineTestAccess::queuedOverrideWorker(input, temp.path / "budget-stop", settings, true);
    OptimizerEngineTestAccess::exactOverridePreview(settings);
#endif
}
static void preprocessingUsesOutput() {
    Temp temp;
    auto s = settingsFor(temp);
    auto tools = temp.path / "tools";
    fs::create_directory(tools);
#ifdef _WIN32
    auto fake = tools / "leanify.exe";
#else
    auto fake = tools / "leanify";
#endif
    fs::copy_file(executable, fake);
    fs::copy_file(tool, tools / tool.filename());
    if (fs::exists(tool.parent_path() / "7z.dll"))
        fs::copy_file(tool.parent_path() / "7z.dll", tools / "7z.dll");
    s.toolsBinDir = tools;
    s.profile = OptimizationProfile::Extreme_RePack;
    s.enablePreprocessors = true;
    auto png = temp.path / "input.png";
    write(png, std::string("\x89PNG\r\n\x1a\n", 8) + std::string(4096, 'x'));
    auto before = fileChecksum(png);
    OptimizerEngine engine;
    run(engine, png, temp.path / "processed.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(fileChecksum(png) == before);
    CHECK(ArchiveExtractor::extract(temp.path / "processed.7z", temp.path / "extracted", tools,
                                    nullptr, nullptr, "", nullptr, 15));
    CHECK(fileChecksum(temp.path / "extracted" / "input.png") ==
          before); // Reject destructive fake optimizer.
}
static void sfx() {
#ifdef _WIN32
    Temp temp;
    auto s = settingsFor(temp);
    s.verificationMode = VerificationMode::Dual;
    if (!fs::exists(s.toolsBinDir / "7z.sfx")) {
#ifdef ULTRAARCH_REQUIRE_TOOLS
        throw std::runtime_error("Required SFX module unavailable");
#else
        std::cout << "[SKIP] SFX: module is unavailable\n";
#endif
        return;
    }
    s.sfxOutput = true;
    write(temp.path / "input.txt", "SFX contents");
    OptimizerEngine engine;
    run(engine, temp.path / "input.txt", temp.path / "output.exe", s);
    CHECK(engine.getState() == EngineState::Completed);
    verifyExtracted(temp.path / "output.exe", temp.path / "input.txt", temp.path / "extracted", s);
    auto actual = temp.path / "sfx-executed";
    fs::create_directories(actual);
    ResourceGuard guard;
    CHECK(guard.initialize(s.ramLimitMB));
    ProcessControl control{nullptr, 30};
    auto launched =
        ProcessManager::execute(pathUtf8(temp.path / "output.exe"), {"-y", "-o" + pathUtf8(actual)},
                                temp.path, &guard, 0, nullptr, nullptr, &control);
    CHECK(launched.success);
    CHECK(fileChecksum(actual / "input.txt") == fileChecksum(temp.path / "input.txt"));
#else
    std::cout << "[SKIP] SFX requires Windows\n";
#endif
}
static void globalResourceBudget() {
    Temp temp;
    auto input = temp.path / "input.bin";
    write(input, randomBytes(262144) + std::string(4 * 1048576, 'a'));
    auto s = settingsFor(temp);
    s.searchMode = SearchMode::Balanced;
    s.ramLimitMB = 128;
    s.workerThreads = 4;
    s.cpuThreads = 2;
    OptimizerEngine engine;
    engine.setSettings(s);
    CHECK(engine.start(input, temp.path / "global.7z"));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (engine.getState() != EngineState::Completed &&
           engine.getState() != EngineState::Failed) {
        auto snapshot = engine.getSnapshot();
        CHECK(snapshot.reservedRamMB <= s.ramLimitMB && snapshot.reservedCpu <= s.cpuThreads &&
              snapshot.activeTasks <= s.workerThreads);
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.wait();
    CHECK(engine.getState() == EngineState::Completed);
    auto summary = engine.getAnalysisSummary();
    TaskMatrixGenerator generator;
    auto tasks = generator.generateMatrix(summary, s);
    bool heavyRan = false;
    for (const auto &task : tasks)
        if (task.estimatedRamMB > s.ramLimitMB / s.workerThreads)
            for (const auto &result : engine.getResults())
                if (result.configId == task.configId && result.status == CandidateStatus::Verified)
                    heavyRan = true;
    CHECK(heavyRan);
}
static void fractionalProbeDictionary() {
    Temp temp;
    auto input = temp.path / "large.bin";
    write(input, "sample");
    fs::resize_file(input, 101ULL * 1048576 + 17);
    auto s = settingsFor(temp);
    s.searchMode = SearchMode::Balanced;
    s.workerThreads = 2;
    s.cpuThreads = 2;
    OptimizerEngine engine;
    run(engine, input, temp.path / "large.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    CHECK(fs::file_size(temp.path / "large.7z") < 100000);
}
static void extractionQuotasAndPreview() {
    Temp temp;
    auto input = temp.path / "source.bin";
    write(input, std::string(262144, 'a'));
    auto s = settingsFor(temp);
    OptimizerEngine engine;
    run(engine, input, temp.path / "archive.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    auto preview = ArchiveExtractor::preview(temp.path / "archive.7z", s.toolsBinDir);
    CHECK(preview.totalBytes == 262144 && preview.totalFiles == 1);
    CHECK(!ArchiveExtractor::extract(temp.path / "archive.7z", temp.path / "limited", s.toolsBinDir,
                                     nullptr, nullptr, "", nullptr, 15, {65536, 65536, 100}));
    auto workspace = temp.path / "runtime-quota";
    fs::create_directory(workspace);
    ProcessControl control{nullptr, 10};
    control.resourceStop = [&] {
        try {
            WorkspaceManager::check(workspace, {65536, 65536, 10});
            return false;
        } catch (...) {
            return true;
        }
    };
    auto result = ProcessManager::execute(pathUtf8(executable),
                                          {"--helper-grow-file", pathUtf8(workspace / "large.bin")},
                                          workspace, nullptr, 0, nullptr, nullptr, &control);
    CHECK(result.cancelled && !result.success);
}
static void windowsContracts() {
#ifdef _WIN32
    Temp temp;
    auto s = settingsFor(temp);
    s.verificationMode = VerificationMode::Dual;
    auto input = temp.path / utf8Path("данные 日本語") / "source.txt";
    write(input, std::string(4096, 'a'));
    OptimizerEngine engine;
    run(engine, input, temp.path / "dual.7z", s);
    CHECK(engine.getState() == EngineState::Completed);
    auto longRoot = temp.path / "long";
    for (int n = 0; n < 7; ++n)
        longRoot /= std::string(40, 'x');
    auto longFile = longRoot / "source.txt";
    write(longFile, std::string(2048, 'b'));
    OptimizerEngine longEngine;
    run(longEngine, longFile, temp.path / "long.7z", s);
    CHECK(longEngine.getState() == EngineState::Completed);
    auto readonly = temp.path / "readonly.7z";
    write(readonly, "previous output");
    auto before = fileChecksum(readonly);
    CHECK(SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY));
    OptimizerEngine readonlyEngine;
    run(readonlyEngine, input, readonly, s);
    CHECK(readonlyEngine.getState() == EngineState::Failed);
    CHECK(fileChecksum(readonly) == before);
    SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_NORMAL);
    // ADS are rejected before compression, rather than silently discarded.
    auto ads = utf8Path(pathUtf8(input) + ":private");
    write(ads, "alternate stream");
    OptimizerEngine adsEngine;
    run(adsEngine, input, temp.path / "ads.7z", s);
    CHECK(adsEngine.getState() == EngineState::Failed);
    DeleteFileW(ads.c_str());
    ResourceGuard guard;
    CHECK(guard.initialize(64));
    auto one = std::async(std::launch::async, [&] {
        return ProcessManager::execute(pathUtf8(executable), {"--helper-allocate"}, temp.path,
                                       &guard, 0);
    });
    auto two = std::async(std::launch::async, [&] {
        return ProcessManager::execute(pathUtf8(executable), {"--helper-allocate"}, temp.path,
                                       &guard, 1);
    });
    auto a = one.get(), b = two.get();
    CHECK(!a.success && !b.success && a.killedByOOM && b.killedByOOM);
    auto summary = InputManifest::scan(input).summary;
    AssemblyPlan plan;
    plan.inputSourcePath = input;
    std::string error;
    auto wrong = summary;
    wrong.files.front().contentChecksum = Hasher::text(std::string(4096, 'z'));
    CHECK(StreamingVerifier::verify(temp.path / "dual.7z", plan, wrong, s, &guard, nullptr,
                                    error) == StreamingVerifier::Result::Failed);
    auto corrupt = temp.path / "corrupt.7z";
    fs::copy_file(temp.path / "dual.7z", corrupt);
    fs::resize_file(corrupt, 32);
    CHECK(StreamingVerifier::verify(corrupt, plan, summary, s, &guard, nullptr, error) ==
          StreamingVerifier::Result::Failed);
#endif
}

static void capabilitySingleFlightAndDecoderRetry() {
#ifdef _WIN32
    Temp temp;
    const auto fake = temp.path / "capability-helper.exe";
    fs::copy_file(executable, fake);
    ResourceGuard guard;
    CHECK(guard.initialize(256));
    TaskConfig task;
    task.name = "capability single-flight fixture";
    task.spec.methods = {CodecSpec{}};
    task.configId = task.spec.id();
    const auto countLaunches = [&](const std::string &command) {
        size_t count = 0;
        for (const auto &file : fs::directory_iterator(temp.path))
            if (file.path().filename().string().rfind(command + "-launch-", 0) == 0)
                ++count;
        return count;
    };
    std::promise<void> start;
    const auto gate = start.get_future().share();
    const auto checkIn = [&](const fs::path &scratch) {
        gate.wait();
        return ToolCapabilities::check({task}, fake, scratch, &guard, nullptr, nullptr);
    };
    // Separate disk-cache parents ensure only the process-wide registry can coalesce work.
    auto first = std::async(std::launch::async, checkIn, temp.path / "one" / "scratch");
    auto second = std::async(std::launch::async, checkIn, temp.path / "two" / "scratch");
    start.set_value();
    CHECK(first.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(second.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(first.get().size() == 1);
    CHECK(second.get().size() == 1);
    CHECK(countLaunches("a") == 1);
    CHECK(countLaunches("t") == 1);

    // A different normalized configuration avoids the previous successful session record.
    task.spec.methods.front().lc = 2;
    task.configId = task.spec.id();
    const auto retryScratch = temp.path / "retry" / "scratch";
    const auto retryCache = retryScratch.parent_path() / "capabilities-v2-sha256.json";
    CHECK(writeAtomically(temp.path / "capability-mode.txt", "oom"));
    CHECK(ToolCapabilities::check({task}, fake, retryScratch, &guard, nullptr, nullptr).size() == 1);
    CHECK(countLaunches("a") == 2);
    CHECK(countLaunches("t") == 2);
    // No record, including a false record, may survive an inconclusive decoder result.
    CHECK(!fs::exists(retryCache));
    CHECK(writeAtomically(temp.path / "capability-mode.txt", "ok"));
    CHECK(ToolCapabilities::check({task}, fake, retryScratch, &guard, nullptr, nullptr).size() == 1);
    CHECK(countLaunches("a") == 3);
    CHECK(countLaunches("t") == 3);
    std::ifstream in(retryCache);
    Json cache;
    in >> cache;
    CHECK(cache.is_object() && cache.size() == 1 && cache.begin().value() == true);
    CHECK(ToolCapabilities::check({task}, fake, temp.path / "cached" / "scratch", &guard,
                                  nullptr, nullptr).size() == 1);
    CHECK(countLaunches("a") == 3);
    CHECK(countLaunches("t") == 3);
#endif
}

#if defined(_WIN32) && defined(ULTRAARCH_FAULT_INJECTION)
static void cancelDuringDecodedVerification() {
    Temp temp;
    auto s = settingsFor(temp);
    const auto input = temp.path / "source.bin", archive = temp.path / "candidate.7z";
    write(input, std::string(1048576, 'v'));
    CHECK(ProcessManager::execute(pathUtf8(tool),
        {"a", "-t7z", "-mx1", pathUtf8(archive), "source.bin"}, temp.path).success);
    const auto inputHash = fileChecksum(input), archiveHash = fileChecksum(archive);
    ContentAnalyzer analyzer;
    auto summary = analyzer.analyze(input);
    summary.files.front().contentChecksum = inputHash;
    AssemblyPlan plan;
    plan.inputSourcePath = input;
    const auto name = utf8Path("Local\\ultraarch-verify-" + uniqueId()).wstring();
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, (name + L"-entered").c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, (name + L"-release").c_str());
    CHECK(entered && release);
    struct Cleanup {
        HANDLE entered, release;
        ~Cleanup() {
            SetEnvironmentVariableW(L"ULTRAARCH_VERIFY_BARRIER", nullptr);
            SetEvent(release); CloseHandle(entered); CloseHandle(release);
        }
    } cleanup{entered, release};
    CHECK(SetEnvironmentVariableW(L"ULTRAARCH_VERIFY_BARRIER", name.c_str()));
    ResourceGuard guard;
    CHECK(guard.initialize(256));
    std::atomic<bool> cancel{false};
    std::string error;
    auto verifying = std::async(std::launch::async, [&] {
        return StreamingVerifier::verify(archive, plan, summary, s, &guard, &cancel, error);
    });
    const bool reached = WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0;
    cancel = true;
    const bool stopped = verifying.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    SetEvent(release); // Also release the fixture if cancellation regresses.
    const auto result = verifying.get();
    CHECK(reached && stopped);
    CHECK(result == StreamingVerifier::Result::Failed && error == "Worker cancelled");
    CHECK(fileChecksum(input) == inputHash && fileChecksum(archive) == archiveHash);
    SetEnvironmentVariableW(L"ULTRAARCH_VERIFY_BARRIER", nullptr);
    cancel = false;
    CHECK(StreamingVerifier::verify(archive, plan, summary, s, &guard, &cancel, error) ==
          StreamingVerifier::Result::Verified);
}
#endif
static void workerProcessControls() {
#ifdef _WIN32
    Temp temp;
    auto s = settingsFor(temp);
    auto app = executable.parent_path();
    auto worker = app / "verify-worker.exe";
    if (!fs::exists(worker))
        worker = app.parent_path().parent_path() / app.filename() / "verify-worker.exe";
    if (!fs::exists(worker))
        worker = app.parent_path() / "verify-worker.exe";
    CHECK(fs::is_regular_file(worker));
    auto manifest = temp.path / "manifest.json";
    write(manifest, Json{{"schema", 1}, {"files", Json::array()},
                         {"directories", Json::array()}}.dump());
    ResourceGuard guard;
    CHECK(guard.initialize(64));
    // A local pipe blocks DLL identity reading without sleeps or test hooks in the worker.
    for (bool cancelCase : {false, true}) {
        auto pipeName = utf8Path("\\\\.\\pipe\\ultraarch-worker-test-" + uniqueId()).wstring();
        HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                      1, 4096, 4096, 0, nullptr);
        CHECK(pipe != INVALID_HANDLE_VALUE);
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CHECK(event);
        struct PipeOwner {
            HANDLE pipe, event;
            ~PipeOwner() {
                CancelIoEx(pipe, nullptr);
                CloseHandle(pipe);
                CloseHandle(event);
            }
        } owner{pipe, event};
        OVERLAPPED pending{};
        pending.hEvent = event;
        CHECK(!ConnectNamedPipe(pipe, &pending) && GetLastError() == ERROR_IO_PENDING);
        auto body = Json{{"dll", pathUtf8(fs::path(pipeName))},
                         {"archive", pathUtf8(temp.path / "unused.7z")},
                         {"manifest", pathUtf8(manifest)},
                         {"manifestHash", fileChecksum(manifest)}}.dump();
        ProcessControl control{nullptr, cancelCase ? 10u : 1u};
        control.stdinData = "UAV1";
        for (int i = 0; i < 4; ++i)
            control.stdinData.push_back(static_cast<char>(uint32_t(body.size()) >> (8 * i)));
        control.stdinData += body;
        std::atomic<bool> cancel{false};
        std::thread stop;
        if (cancelCase)
            stop = std::thread([&] {
                // Cancel only once the real worker enters the blocking identity read.
                WaitForSingleObject(event, 5000);
                cancel = true;
            });
        auto result = ProcessManager::execute(pathUtf8(worker), {}, worker.parent_path(),
                                             &guard, 0, &cancel, nullptr, &control);
        if (stop.joinable())
            stop.join();
        CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
        CHECK(!result.success && (cancelCase ? result.cancelled : result.timedOut));
        CHECK(result.durationSeconds < 5);
    }
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // The capability fixture invokes a copied integration EXE with ordinary 7-Zip commands.
    if (argc > 1 && (std::string(argv[1]) == "a" || std::string(argv[1]) == "t")) {
        wchar_t module[32768]{};
        const auto length = GetModuleFileNameW(nullptr, module, 32768);
        if (!length || length >= 32768)
            return 2;
        const auto root = fs::path(module).parent_path();
        try {
            const auto command = std::string(argv[1]);
            write(root / (command + "-launch-" + uniqueId()), "started");
            if (command == "a") {
                for (int i = 2; i < argc; ++i) {
                    const auto path = utf8Path(argv[i]);
                    if (path.filename() == "smoke.7z") {
                        write(path, "capability fixture archive");
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        return 0;
                    }
                }
                return 7;
            }
            std::ifstream mode(root / "capability-mode.txt");
            std::string value;
            mode >> value;
            if (value == "oom") {
                std::cerr << "Out of memory\n";
                return 8;
            }
            return 0;
        } catch (...) {
            return 2;
        }
    }
#endif
    if (argc > 1 && std::string(argv[1]) == "--helper-cpu") {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        volatile uint64_t checksum = 1;
        while (std::chrono::steady_clock::now() < until)
            for (int n = 0; n < 10000; ++n)
                checksum = checksum * 1664525 + 1013904223;
        return 0;
    }
#ifdef _WIN32
    if (argc > 1 && std::string(argv[1]) == "--helper-nested-job") {
        wchar_t module[32768]{};
        if (!GetModuleFileNameW(nullptr, module, 32768))
            return 1;
        ResourceGuard guard;
        if (!guard.initialize(48))
            return 1;
        auto child = ProcessManager::execute(pathUtf8(fs::path(module)), {"--helper-allocate"},
                                              fs::temp_directory_path(), &guard);
        return !child.success && child.killedByOOM ? 0 : 1;
    }
#endif
    if (argc > 2 && std::string(argv[1]) == "--helper-grow-file") {
        std::ofstream out(utf8Path(argv[2]), std::ios::binary);
        std::string chunk(32768, 'x');
        for (int n = 0; n < 100; ++n) {
            out.write(chunk.data(), chunk.size());
            out.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--helper-sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--helper-echo") {
        Json args = Json::array();
        for (int i = 2; i < argc; ++i)
            args.push_back(argv[i]);
        std::cout << args.dump();
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--helper-streams") {
        std::cout << "stdout-only\n";
        std::cerr << "stderr-only\n";
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--helper-allocate") {
        try {
            std::vector<char> memory(256 * 1048576, 1);
            std::cout << memory.back();
            return 0;
        } catch (const std::bad_alloc &) {
            return 8;
        }
    }
#ifdef _WIN32
    if (argc > 2 && std::string(argv[1]) == "--helper-job-update") {
        const auto directory = utf8Path(argv[2]);
        // A bounded allocation proves current commit without risking machine-wide OOM.
        std::vector<char> memory(128 * 1048576, 1);
        // ProcessManager also owns an inner task Job. Parent limits must therefore
        // be queried using ResourceGuard's handle, rather than a null Job handle here.
        write(directory / "job-before-ready", "ready");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (!fs::exists(directory / "job-query-after") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        write(directory / "job-after-ready", "ready");
        std::this_thread::sleep_until(deadline);
        std::cout << memory.back();
        return 0;
    }
#endif
    if (argc > 1 && std::string(argv[1]) == "-i") {
        fs::resize_file(utf8Path(argv[argc - 1]), 128);
        return 0;
    }
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    executable = fs::path(path);
#else
    executable = fs::absolute(utf8Path(argv[0]));
#endif
#ifdef _WIN32
    auto parent = executable.parent_path();
    for (int level = 0; level < 7 && tool.empty(); ++level) {
        tool = ProcessManager::resolveExecutable("7z", parent / "bin", true);
        parent = parent.parent_path();
    }
#endif
    if (tool.empty())
        tool = ProcessManager::resolveExecutable("7z", fs::path("bin"));
    if (tool.empty()) {
#ifdef ULTRAARCH_REQUIRE_TOOLS
        std::cerr << "Required 7-Zip unavailable\n";
        return 1;
#endif
        std::cerr << "7-Zip unavailable: integration tests require a real compressor\n";
        return 77;
    }
    try {
        const std::string testFilter = argc == 3 && std::string(argv[1]) == "--test" ? argv[2] : "";
        bool matched = false;
        for (const auto &test : std::vector<std::pair<const char *, void (*)()>>{
                 {"smallMediaAndRepetition", smallMediaAndRepetition},
                 {"originalArchiveNeverGrows", originalArchiveNeverGrows},
                 {"groupedMethodsAndWrapping", groupedMethodsAndWrapping},
                 {"similarityFromSingleTextGroup", similarityFromSingleTextGroup},
                 {"encryptionVolumesAndManifest", encryptionVolumesAndManifest},
                 {"solidAndRepack", solidAndRepack},
                 {"preserveOutputOnFailure", preserveOutputOnFailure},
                 {"cancellationAndResume", cancellationAndResume},
                 {"compressionGainSurvivesCancellation", compressionGainSurvivesCancellation},
                 {"cancellationPrecedence", OptimizerEngineTestAccess::cancellationPrecedence},
                 {"explicitFullAttemptBudget", explicitFullAttemptBudget},
                 {"phaseAccounting", OptimizerEngineTestAccess::phaseAccounting},
                 {"groupedFinishPrecedence", OptimizerEngineTestAccess::groupedFinishPrecedence},
                 {"timeBudgetPublication", timeBudgetPublication},
                 {"scopedCandidateSkipAndManualAdmission", scopedCandidateSkipAndManualAdmission},
                 {"skippedGroupRetainedAndEarlyAssembly", skippedGroupRetainedAndEarlyAssembly},
                 {"smallGroupBackfillsIdleExecutors", smallGroupBackfillsIdleExecutors},
                 {"ramExecutorsRespectDiskLimit", ramExecutorsRespectDiskLimit},
                 {"diskFallbackAdmission", diskFallbackAdmission},
                 {"inputMutation", inputMutation},
                 {"deepMatrixUsesValidFlags", deepMatrixUsesValidFlags},
                 {"parallelFinishWhilePaused", parallelFinishWhilePaused},
                 {"verifyContentsAndCallbackSafety", verifyContentsAndCallbackSafety},
                 {"processControl", processControl},
                 {"queuedOverrideDispatchAndBudgetCleanup", queuedOverrideDispatchAndBudgetCleanup},
                 {"preprocessingUsesOutput", preprocessingUsesOutput},
                 {"sfx", sfx},
                 {"globalResourceBudget", globalResourceBudget},
                 {"fractionalProbeDictionary", fractionalProbeDictionary},
                 {"extractionQuotasAndPreview", extractionQuotasAndPreview},
                 {"windowsContracts", windowsContracts},
                 {"capabilitySingleFlightAndDecoderRetry", capabilitySingleFlightAndDecoderRetry},
#if defined(_WIN32) && defined(ULTRAARCH_FAULT_INJECTION)
                 {"cancelDuringDecodedVerification", cancelDuringDecodedVerification},
#endif
                 {"workerProcessControls", workerProcessControls}}) {
            if (!testFilter.empty() && testFilter != test.first) continue;
            matched = true;
            std::cout << "[RUN] " << test.first << std::endl;
            test.second();
            std::cout << "[PASS] " << test.first << std::endl;
        }
        CHECK(matched);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
