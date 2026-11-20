#include "../src/core/ArchiveAssembler.hpp"
#include "../src/core/ArchiveExtractor.hpp"
#include "../src/core/ContentAnalyzer.hpp"
#include "../src/core/OptimizerEngine.hpp"
#include "../src/core/PresetManager.hpp"
#include "../src/core/ProcessManager.hpp"
#include "../src/core/ResourceGuard.hpp"
#include "../src/core/SessionManager.hpp"
#include "../src/core/SettingsJson.hpp"
#include "../src/core/TaskMatrixGenerator.hpp"
#include "../src/core/Types.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

using namespace UltraArch;

#define TEST_ASSERT(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n";         \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        std::cout << "[RUNNING] " << #name << "...\n";                                             \
        if (name()) {                                                                              \
            std::cout << "[PASSED]  " << #name << "\n";                                            \
            passedCount++;                                                                         \
        } else {                                                                                   \
            std::cout << "[FAILED]  " << #name << "\n";                                            \
            failedCount++;                                                                         \
        }                                                                                          \
    } while (0)

struct TempDirGuard {
    std::filesystem::path dir;
    TempDirGuard(const std::string &prefix) {
        dir = std::filesystem::temp_directory_path() / (prefix + "_test_" + uniqueId());
        std::filesystem::create_directories(dir);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

bool testContentAnalyzerEntropy() {
    // 1. All zero bytes -> entropy must be 0.0
    std::vector<uint8_t> zeros(1024, 0);
    double eZeros = ContentAnalyzer::calculateEntropy(zeros.data(), zeros.size());
    TEST_ASSERT(eZeros == 0.0, "Entropy of homogeneous zeroes should be 0.0");

    // 2. Uniform random distribution -> entropy should be close to 8.0
    std::vector<uint8_t> uniform(256 * 100);
    for (size_t i = 0; i < uniform.size(); ++i) {
        uniform[i] = static_cast<uint8_t>(i % 256);
    }
    double eUniform = ContentAnalyzer::calculateEntropy(uniform.data(), uniform.size());
    TEST_ASSERT(eUniform > 7.99 && eUniform <= 8.0, "Entropy of uniform bytes should be ~8.0");

    return true;
}

bool testContentAnalyzerMagicBytes() {
    TempDirGuard guard("magic");

    // 1. Create synthetic PE executable file
    std::filesystem::path pePath = guard.dir / "test_app.exe";
    {
        std::ofstream f(pePath, std::ios::binary);
        std::vector<uint8_t> peData(512, 0);
        peData[0] = 'M';
        peData[1] = 'Z';
        // e_lfanew at 0x3C = 0x80
        *reinterpret_cast<uint32_t *>(&peData[0x3C]) = 0x80;
        peData[0x80] = 'P';
        peData[0x81] = 'E';
        peData[0x82] = 0;
        peData[0x83] = 0;
        f.write(reinterpret_cast<char *>(peData.data()), peData.size());
    }

    FileTypeInfo peInfo = ContentAnalyzer::inspectFile(pePath);
    TEST_ASSERT(peInfo.category == FileCategory::Executable_PE, "Should detect synthetic PE file");

    // 2. Create synthetic WAV file
    std::filesystem::path wavPath = guard.dir / "sound.wav";
    {
        std::ofstream f(wavPath, std::ios::binary);
        char riffHeader[] = "RIFF\x24\x00\x00\x00WAVEfmt ";
        f.write(riffHeader, sizeof(riffHeader) - 1);
    }

    FileTypeInfo wavInfo = ContentAnalyzer::inspectFile(wavPath);
    TEST_ASSERT(wavInfo.category == FileCategory::Uncompressed_Media,
                "Should detect WAV as uncompressed media");

    // 3. Create synthetic JSON text file
    std::filesystem::path jsonPath = guard.dir / "config.json";
    {
        std::ofstream f(jsonPath);
        f << "{\n  \"name\": \"UltraArch\",\n  \"version\": \"2.0.0\"\n}\n";
    }

    FileTypeInfo jsonInfo = ContentAnalyzer::inspectFile(jsonPath);
    TEST_ASSERT(jsonInfo.category == FileCategory::Text_Database,
                "Should detect JSON as structured text");

    return true;
}

bool testTaskMatrixGenerator() {
    TaskMatrixGenerator gen;
    AnalysisSummary summary;
    summary.totalFiles = 10;
    summary.totalBytes = 50 * 1024 * 1024;
    summary.hasExecutables = true;
    summary.hasStructuredText = true;
    summary.hasUncompressedMedia = true;

    OptimizerSettings settings;
    settings.profile = OptimizationProfile::DeepSearch;
    settings.ramLimitMB = 4096; // 4 GB limit

    std::vector<TaskConfig> tasks = gen.generateMatrix(summary, settings);
    TEST_ASSERT(!tasks.empty(), "Tasks should not be empty");

    bool hasBcj2 = false;
    bool hasPpmd = false;
    bool hasDelta = false;
    bool hasLzma2Deep = false;

    for (const auto &t : tasks) {
        TEST_ASSERT(t.estimatedRamMB <= settings.ramLimitMB + 1024,
                    "Task estimated memory must fit the bounded manual allowance");
        TEST_ASSERT(t.resourceBlocked == (t.estimatedRamMB > settings.ramLimitMB),
                    "Over-budget tasks must remain explicitly blocked");

        if (t.name.find("BCJ2") != std::string::npos)
            hasBcj2 = true;
        if (t.name.find("PPMd") != std::string::npos)
            hasPpmd = true;
        if (t.name.find("Delta") != std::string::npos)
            hasDelta = true;
        if (t.name.find("bt5") != std::string::npos || t.name.find("mmc") != std::string::npos)
            hasLzma2Deep = true;
    }

    TEST_ASSERT(hasBcj2, "Generated matrix must include BCJ2 candidates for executables");
    TEST_ASSERT(hasPpmd, "Generated matrix must include PPMd candidates for text");
    TEST_ASSERT(hasDelta, "Generated matrix must include Delta candidates for media");
    TEST_ASSERT(hasLzma2Deep,
                "DeepSearch profile must include bt5 and mmc deep match finder variants");

    // Test WinRAR Safe profile filtering
    settings.profile = OptimizationProfile::WinRAR_Safe;
    std::vector<TaskConfig> safeTasks = gen.generateMatrix(summary, settings);
    for (const auto &t : safeTasks) {
        TEST_ASSERT(t.isWinRARSafe, "All tasks in WinRAR_Safe profile must be marked safe");
    }

    return true;
}

bool testResourceGuardTopology() {
    CpuTopology topo = ResourceGuard::detectCpuTopology();
    TEST_ASSERT(topo.logicalProcessors >= 1, "Logical processors must be >= 1");
    TEST_ASSERT(topo.physicalCores >= 1, "Physical cores must be >= 1");
    uint64_t mask0 = ResourceGuard::getWorkerAffinityMask(0, topo);
    uint64_t mask1 = ResourceGuard::getWorkerAffinityMask(1, topo);
    if (topo.processorGroups > 1 || topo.physicalCoreMasks.empty()) {
        TEST_ASSERT(mask0 == 0 && mask1 == 0, "Unsupported topology must not force affinity");
    } else {
        TEST_ASSERT(mask0 > 0 && mask1 > 0,
                    "Known single-group topology must supply affinity masks");
    }

    return true;
}

bool testMemoryEstimator() {
    std::vector<std::string> flags64 = {"-m0=lzma2:d64m:fb273"};
    uint64_t ram64 = TaskMatrixGenerator::estimateMemoryMB(flags64, true);
    TEST_ASSERT(ram64 >= 64 * 10, "64MB dictionary must estimate at least 640MB RAM");

    std::vector<std::string> flags1024 = {"-m0=lzma2:d1024m:fb273"};
    uint64_t ram1024 = TaskMatrixGenerator::estimateMemoryMB(flags1024, true);
    TEST_ASSERT(ram1024 >= 10240, "1024MB dictionary must estimate >= 10GB RAM");

    std::vector<std::string> flagsPpmd = {"-m0=ppmd:mem512m:o16"};
    uint64_t ramPpmd = TaskMatrixGenerator::estimateMemoryMB(flagsPpmd, true);
    TEST_ASSERT(ramPpmd >= 512, "PPMd 512m should estimate >= 512MB RAM");

    return true;
}

bool testOptimizerEngineLifecycle() {
    OptimizerEngine engine;
    TEST_ASSERT(engine.getState() == EngineState::Idle, "Initial state must be Idle");

    OptimizerSettings s;
    s.ramLimitMB = 4096;
    s.workerThreads = 2;
    engine.setSettings(s);
    TEST_ASSERT(engine.getSettings().ramLimitMB == 4096, "Settings must be persisted");

    bool stateChanged = false;
    engine.setStateCallback([&](EngineState) { stateChanged = true; });

    // Start on non-existent path should handle gracefully
    bool started = engine.start("/invalid/non_existent_path_xyz123", "/invalid/out.7z");
    TEST_ASSERT(!started, "Missing input must be rejected");

    // Wait for it to finish gracefully
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    TEST_ASSERT(engine.getState() == EngineState::Failed,
                "Engine must report Failed for invalid input");

    return true;
}

bool testMultiStreamMemoryEstimation() {
    std::vector<std::string> flagsBcj2 = {"-m0=bcj2", "-m1=lzma2:d64m:fb273",
                                          "-m2=lzma2:d64m:fb273", "-m3=lzma2:d16m:fb32"};
    uint64_t ramBcj2 = TaskMatrixGenerator::estimateMemoryMB(flagsBcj2, true);
    TEST_ASSERT(ramBcj2 >= 1500,
                "BCJ2 4-stream config should correctly sum memory and be >= 1500MB");

    std::vector<std::string> flagsSingle = {"-m0=lzma2", "-md=128m"};
    uint64_t ramSingle = TaskMatrixGenerator::estimateMemoryMB(flagsSingle, true);
    TEST_ASSERT(ramSingle >= 1408, "Single stream 128MB dict should be >= 1408MB");

    std::vector<std::string> flagsPpmd = {"-m0=PPMd", "-md=256m"};
    uint64_t ramPpmd = TaskMatrixGenerator::estimateMemoryMB(flagsPpmd, true);
    TEST_ASSERT(ramPpmd >= 256, "PPMd estimation should be correct");

    return true;
}

bool testPeParserSafety() {
    TempDirGuard guard("pe_safety");
    std::filesystem::path pePath = guard.dir / "malicious.exe";

    // e_lfanew out of bounds
    {
        std::ofstream f(pePath, std::ios::binary);
        std::vector<uint8_t> peData(512, 0);
        peData[0] = 'M';
        peData[1] = 'Z';
        *reinterpret_cast<uint32_t *>(&peData[0x3C]) = 0xFFFFFF00;
        f.write(reinterpret_cast<char *>(peData.data()), peData.size());
    }

    FileTypeInfo info = ContentAnalyzer::inspectFile(pePath);
    TEST_ASSERT(info.detectedType == "PE Executable",
                "Should not crash, parsing gracefully fails or identifies as generic");

    // e_lfanew near overflow
    {
        std::ofstream f(pePath, std::ios::binary);
        std::vector<uint8_t> peData(512, 0);
        peData[0] = 'M';
        peData[1] = 'Z';
        *reinterpret_cast<uint32_t *>(&peData[0x3C]) = 0xFFFFFFFC;
        f.write(reinterpret_cast<char *>(peData.data()), peData.size());
    }

    FileTypeInfo info2 = ContentAnalyzer::inspectFile(pePath);
    TEST_ASSERT(info2.detectedType == "PE Executable", "Should not crash with near overflow");

    return true;
}

bool testSmartSkipping() {
    TaskMatrixGenerator gen;
    AnalysisSummary summary;
    summary.totalFiles = 10;
    summary.totalBytes = 50 * 1024 * 1024;
    summary.hasExecutables = false;
    summary.hasUncompressedMedia = false;
    summary.hasStructuredText = true;
    summary.averageEntropy = 5.0; // random

    OptimizerSettings settings;
    settings.profile = OptimizationProfile::WinRAR_Safe;

    std::vector<TaskConfig> tasks = gen.generateMatrix(summary, settings);
    for (const auto &t : tasks) {
        TEST_ASSERT(t.name.find("BCJ") == std::string::npos,
                    "Should not include BCJ tasks if no executables");
        TEST_ASSERT(t.name.find("Delta") == std::string::npos,
                    "Should not include Delta tasks if no media");
    }

    // High entropy
    summary.averageEntropy = 8.0;
    std::vector<TaskConfig> tasksEnt = gen.generateMatrix(summary, settings);
    TEST_ASSERT(!tasksEnt.empty(), "High entropy input must retain candidates");
    TEST_ASSERT(
        std::any_of(tasksEnt.begin(), tasksEnt.end(),
                    [](const auto &t) { return t.name.find("LZMA2") != std::string::npos; }),
        "High byte entropy does not imply absence of repetition");

    return true;
}

bool testSolidBlockVariants() {
    TaskMatrixGenerator gen;
    AnalysisSummary summary;
    summary.totalFiles = 100;
    summary.totalBytes = 100 * 1024 * 1024;

    OptimizerSettings settings;
    settings.profile = OptimizationProfile::DeepSearch;

    std::vector<TaskConfig> tasks = gen.generateMatrix(summary, settings);
    bool hasSolidFlag = false;
    for (const auto &t : tasks) {
        for (const auto &flag : t.flags) {
            if (flag.find("-ms=") != std::string::npos || flag.find("-ms") != std::string::npos) {
                hasSolidFlag = true;
            }
        }
    }
    TEST_ASSERT(hasSolidFlag, "DeepSearch should generate some tasks with solid block variants");

    return true;
}

bool testEncryptionFlags() {
    TaskMatrixGenerator gen;
    AnalysisSummary summary;
    summary.totalFiles = 10;
    summary.totalBytes = 1024 * 1024;

    OptimizerSettings settings;
    settings.password = "test123";
    settings.encryptHeaders = true;

    std::vector<TaskConfig> tasks = gen.generateMatrix(summary, settings);
    for (const auto &t : tasks) {
        bool hasPwd = false;
        bool hasMhe = false;
        for (const auto &flag : t.flags) {
            if (flag == "-ptest123")
                hasPwd = true;
            if (flag == "-mhe=on")
                hasMhe = true;
        }
        TEST_ASSERT(hasPwd && hasMhe,
                    "All tasks must contain encryption flags when password is set");
    }

    return true;
}

bool testDictionaryAutoTuning() {
    TaskMatrixGenerator gen;
    AnalysisSummary summary;
    summary.totalFiles = 10;
    summary.totalBytes = 50 * 1024 * 1024;

    OptimizerSettings settings;
    settings.ramLimitMB = 2048; // 2 GB
    settings.workerThreads = 4;

    std::vector<TaskConfig> tasks = gen.generateMatrix(summary, settings);
    for (const auto &t : tasks) {
        TEST_ASSERT(t.estimatedRamMB <= 3072, "No task should exceed the 2048 MB budget plus 1024 MB allowance");
        TEST_ASSERT(t.resourceBlocked == (t.estimatedRamMB > 2048),
                    "Over-budget tasks must remain explicitly blocked");
    }

    return true;
}

bool testPresetSaveLoad() {
    TempDirGuard guard("presets");
    PresetManager presetMgr;
    presetMgr.initialize(guard.dir);

    OptimizerSettings settings;
    settings.password = "secret";
    settings.volumeSizeMB = 100;

    PresetInfo info;
    info.name = "Test Preset";
    info.description = "A test preset";
    info.settings = settings;

    bool saved = presetMgr.savePreset(info);
    TEST_ASSERT(saved, "Preset should save successfully");

    PresetInfo loaded;
    bool didLoad = presetMgr.loadPreset("Test Preset", loaded);
    TEST_ASSERT(didLoad, "Preset should load successfully");
    TEST_ASSERT(loaded.settings.password.empty(), "Passwords must not be saved in presets");
    TEST_ASSERT(loaded.settings.volumeSizeMB == 100, "Volume size should match");

    return true;
}

bool testSessionCheckpoint() {
    TempDirGuard guard("sessions");
    SessionManager sessionMgr;
    sessionMgr.setSessionDir(guard.dir);

    SessionInfo info;
    info.sessionId = SessionManager::generateSessionId();
    info.inputPath = "/test/input.7z";
    info.completedTaskIds = {1, 2, 3, 4, 5};

    bool saved = sessionMgr.saveCheckpoint(info);
    TEST_ASSERT(saved, "Checkpoint should save successfully");

    OptimizerSettings resumeSettings;
    SessionInfo resumed;
    bool didResume = sessionMgr.tryResumeSession("/test/input.7z", resumeSettings, resumed);
    TEST_ASSERT(didResume, "Session should resume successfully");
    TEST_ASSERT(resumed.completedTaskIds.size() == 5, "Completed tasks should match");

    sessionMgr.clearSession(info.sessionId);
    bool didResume2 = sessionMgr.tryResumeSession("/test/input.7z", resumeSettings, resumed);
    TEST_ASSERT(!didResume2, "Session should not resume after clear");

    return true;
}

bool testArchiveDetection() {
    TempDirGuard guard("archives");

    std::filesystem::path zipPath = guard.dir / "test.zip";
    {
        std::ofstream f(zipPath, std::ios::binary);
        f.write("PK\x03\x04", 4);
    }

    std::filesystem::path rarPath = guard.dir / "test.rar";
    {
        std::ofstream f(rarPath, std::ios::binary);
        f.write("Rar!", 4);
    }

    std::filesystem::path szPath = guard.dir / "test.7z";
    {
        std::ofstream f(szPath, std::ios::binary);
        f.write("7z\xBC\xAF\x27\x1C", 6);
    }

    std::filesystem::path gzPath = guard.dir / "test.gz";
    {
        std::ofstream f(gzPath, std::ios::binary);
        f.write("\x1f\x8b", 2);
    }

    std::filesystem::path txtPath = guard.dir / "test.txt";
    {
        std::ofstream f(txtPath, std::ios::binary);
        f.write("Hello world", 11);
    }

    TEST_ASSERT(ArchiveExtractor::isArchive(zipPath), "ZIP should be detected");
    TEST_ASSERT(ArchiveExtractor::detectFormat(zipPath) == "zip", "Format should be zip");

    TEST_ASSERT(ArchiveExtractor::isArchive(rarPath), "RAR should be detected");
    TEST_ASSERT(ArchiveExtractor::detectFormat(rarPath) == "rar", "Format should be rar");

    TEST_ASSERT(ArchiveExtractor::isArchive(szPath), "7Z should be detected");
    TEST_ASSERT(ArchiveExtractor::detectFormat(szPath) == "7z", "Format should be 7z");

    TEST_ASSERT(ArchiveExtractor::isArchive(gzPath), "GZ should be detected");
    TEST_ASSERT(ArchiveExtractor::detectFormat(gzPath) == "gz" ||
                    ArchiveExtractor::detectFormat(gzPath) == "gzip",
                "Format should be gz/gzip");

    TEST_ASSERT(!ArchiveExtractor::isArchive(txtPath), "TXT should not be detected");

    return true;
}

bool testAdaptivePruning() {
    TaskMatrixGenerator gen;
    AnalysisSummary summary;
    summary.totalFiles = 100;
    summary.totalBytes = 500ULL * 1024 * 1024; // 500MB to avoid small-file dict limits
    summary.hasExecutables = true;
    summary.hasUncompressedMedia = true;
    summary.hasStructuredText = true;
    summary.averageEntropy = 5.0;

    OptimizerSettings settings;
    settings.profile = OptimizationProfile::DeepSearch;
    settings.ramLimitMB = 16384; // 16GB so nothing gets filtered by RAM
    settings.enableAdaptivePruning = true;
    settings.pruningThreshold = 1.2;

    std::vector<TaskConfig> tasks = gen.generateMatrix(summary, settings);
    TEST_ASSERT(tasks.size() > 5,
                "Should generate > 5 tasks for DeepSearch with all content types");

    for (const auto &task : tasks) {
        TEST_ASSERT(task.estimatedRamMB <= settings.ramLimitMB + 1024,
                    "Candidate must fit the bounded manual allowance");
        TEST_ASSERT(task.resourceBlocked == (task.estimatedRamMB > settings.ramLimitMB),
                    "Over-budget candidates must remain explicitly blocked");
    }

    return true;
}

bool testSettingsAndPresetSafety() {
    TempDirGuard guard("settings_safety");
    PresetManager manager;
    manager.initialize(guard.dir);
    PresetInfo p;
    p.name = "../escape";
    TEST_ASSERT(!manager.savePreset(p), "Reject traversing preset names");
    p.name = "CON";
    TEST_ASSERT(!manager.savePreset(p), "Reject Windows reserved names");
    p.name = "Unicode \xD0\xA2\xD0\xB5\xD1\x81\xD1\x82";
    p.description = "quote \" and newline\n";
    p.settings.pruningThreshold = 1.25;
    TEST_ASSERT(manager.savePreset(p), "Save UTF-8 preset");
    PresetInfo loaded;
    TEST_ASSERT(manager.loadPreset(p.name, loaded), "Load UTF-8 preset");
    TEST_ASSERT(loaded.description == p.description && loaded.settings.pruningThreshold == 1.25,
                "Round-trip escaped strings and doubles");
    std::ofstream(guard.dir / "broken.json") << "{broken";
    TEST_ASSERT(!manager.loadPreset("broken", loaded), "Malformed JSON must fail");
    bool rejected = false;
    try {
        settingsFromJson(Json{{"workerThreads", -1}});
    } catch (...) {
        rejected = true;
    }
    TEST_ASSERT(rejected, "Negative workers must be rejected");
    auto builtin = PresetManager::getBuiltinPresets();
    TEST_ASSERT(manager.loadPreset(builtin.front().name, loaded) && loaded.isBuiltin,
                "Built-ins must load");
    return true;
}
bool testWindowsQuotingAndSmallInputs() {
#ifdef _WIN32
    TempDirGuard pathGuard("case-insensitive");
    TEST_ASSERT(
        isWithin(pathGuard.dir / utf8Path("секрет.TXT"), pathGuard.dir / utf8Path("СЕКРЕТ.txt")),
        "Windows input/output comparison must ignore Unicode case");
#endif
    TEST_ASSERT(quoteWindowsArgument("C:\\Path With Spaces\\") == "\"C:\\Path With Spaces\\\\\"",
                "Double terminal backslashes when quoting");
    TEST_ASSERT(quoteWindowsArgument("a\"b") == "\"a\\\"b\"", "Escape embedded quote");
    AnalysisSummary summary;
    summary.totalFiles = 1;
    summary.totalBytes = 2048;
    summary.hasUncompressedMedia = true;
    OptimizerSettings settings;
    settings.profile = OptimizationProfile::WinRAR_Safe;
    settings.cpuThreads = 1;
    settings.workerThreads = 1;
    TaskMatrixGenerator gen;
    auto tasks = gen.generateMatrix(summary, settings);
    TEST_ASSERT(
        std::any_of(tasks.begin(), tasks.end(),
                    [](const auto &t) { return t.name.find("LZMA2") != std::string::npos; }),
        "Small media must have a compressor");
    return true;
}
bool testSessionFullRoundTrip() {
    TempDirGuard guard("full_session");
    SessionManager manager;
    manager.setSessionDir(guard.dir);
    SessionInfo info;
    info.sessionId = SessionManager::generateSessionId();
    info.inputPath = guard.dir / "input";
    info.outputPath = guard.dir / "output";
    info.workspace = guard.dir / "workspace";
    info.fingerprint = "fingerprint";
    info.settings.cpuThreads = 2;
    CandidateResult r;
    r.taskId = 7;
    r.name = "quoted \" candidate";
    r.verified = true;
    r.status = CandidateStatus::Verified;
    r.tempArchivePath = info.workspace / "leader.7z";
    info.results.push_back(r);
    info.leaderTaskId = 7;
    info.completedTaskIds = {7};
    TEST_ASSERT(manager.saveCheckpoint(info), "Save full session");
    SessionInfo out;
    TEST_ASSERT(manager.tryResumeSession(info.inputPath, info.settings, out, "fingerprint"),
                "Resume compatible fingerprint");
    TEST_ASSERT(out.results.size() == 1 && out.results[0].name == r.name &&
                    out.workspace == info.workspace,
                "Restore results and workspace");
    TEST_ASSERT(!manager.tryResumeSession(info.inputPath, info.settings, out, "changed"),
                "Reject changed input fingerprint");
    return true;
}

int main() {
    std::cout << "\n=======================================================\n"
              << " ULTRAARCH STUDIO PRO - AUTOMATED TEST SUITE\n"
              << "=======================================================\n\n";

    int passedCount = 0;
    int failedCount = 0;

    RUN_TEST(testContentAnalyzerEntropy);
    RUN_TEST(testContentAnalyzerMagicBytes);
    RUN_TEST(testMemoryEstimator);
    RUN_TEST(testTaskMatrixGenerator);
    RUN_TEST(testResourceGuardTopology);
    RUN_TEST(testOptimizerEngineLifecycle);

    // NEW TESTS
    RUN_TEST(testMultiStreamMemoryEstimation);
    RUN_TEST(testPeParserSafety);
    RUN_TEST(testSmartSkipping);
    RUN_TEST(testSolidBlockVariants);
    RUN_TEST(testEncryptionFlags);
    RUN_TEST(testDictionaryAutoTuning);
    RUN_TEST(testPresetSaveLoad);
    RUN_TEST(testSessionCheckpoint);
    RUN_TEST(testArchiveDetection);
    RUN_TEST(testAdaptivePruning);
    RUN_TEST(testSettingsAndPresetSafety);
    RUN_TEST(testWindowsQuotingAndSmallInputs);
    RUN_TEST(testSessionFullRoundTrip);

    std::cout << "\n=======================================================\n"
              << " TEST RESULTS: " << passedCount << " PASSED, " << failedCount << " FAILED\n"
              << "=======================================================\n\n";

    return (failedCount == 0) ? 0 : 1;
}
