#pragma once

#include "CompressionSpec.hpp"
#include "FileUtils.hpp"
#include "MemoryContent.hpp"
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace UltraArch {

enum class OptimizationProfile {
    WinRAR_Safe,   // Strict standard 7z (LZMA, LZMA2, PPMd, BCJ, BCJ2, Delta)
    DeepSearch,    // Additional LZMA2 contexts and deep match-finder search
    Extreme_RePack // Optional resource preprocessing (Leanify/ECT) + 7z
};

enum class SearchMode { Fast, Balanced, Exhaustive };
enum class SelectionGoal { MinimumSize, FastWithinTolerance };
enum class ContentPolicy { ExactBytes, OptimizeResources };
enum class VerificationMode { Disk, Dual, Streaming };
enum class AffinityMode { None, PhysicalCores, CpuSets };

enum class FileCategory {
    Executable_PE,             // PE32 / PE64 (.exe, .dll, .sys)
    Text_Database,             // JSON, XML, SQL, SQLite, TXT, HTML, Logs
    Uncompressed_Media,        // WAV, BMP, TGA (benefits from Delta filter)
    Precompressed_HighEntropy, // MP4, MKV, JPG, PNG, ZIP, RAR (classification hint)
    Generic_Binary             // General binary data
};

enum class CandidateStatus { Queued, Running, Passed, Failed, Verified, Skipped, OOM_Terminated, DiscardedBySize, Deferred };

enum class ErrorCode {
    None,
    ProcessCrash,
    OutOfMemory,
    DiskFull,
    Timeout,
    InvalidArchive,
    ToolNotFound,
    PermissionDenied,
    Unknown
};

enum class EngineState {
    Idle,
    Analyzing,
    Preprocessing, // after Analyzing
    Extracting,    // for repack mode
    TestingCandidates,
    AssemblingFinal,
    Paused,
    Completed,
    Cancelled,
    Failed
};

inline const char *engineStateName(EngineState state) {
    static constexpr const char *names[] = {"Idle",       "Analyzing",         "Preprocessing",
                                            "Extracting", "TestingCandidates", "AssemblingFinal",
                                            "Paused",     "Completed",         "Cancelled",
                                            "Failed"};
    int index = static_cast<int>(state);
    return index >= 0 && index < 10 ? names[index] : "Unknown";
}

struct FileTypeInfo {
    std::filesystem::path path;
    uint64_t sizeBytes = 0;
    FileCategory category = FileCategory::Generic_Binary;
    double entropy = 0.0;
    std::string detectedType;
    int64_t modifiedTicks = 0;
    std::string fileIdentity;    // Volume/file ID for Windows diagnostics and scan stability.
    std::string contentChecksum; // Filled for the immutable run manifest.
    std::array<uint64_t, 4> similaritySketch{}; // Sampling hint only; never an integrity proof.
    std::shared_ptr<const MemoryContent> memoryContent;
};

struct TaskConfig {
    bool resourceBlocked = false;
    bool ramCandidate = false;
    bool reuseInputArchive = false; // A compatible original is a candidate, never a new codec.
    bool groupedSearch = false;
    bool similarityPartition = false;
    CompressionSpec spec;
    std::string configId;
    uint32_t cpuTokens = 1;
    int taskId = 0;
    std::string name;
    FileCategory category = FileCategory::Generic_Binary;
    std::vector<std::string> flags;
    uint64_t estimatedRamMB = 512;
    bool isWinRARSafe = true;
    std::string description;
};

struct CandidateResult {
    std::string storage = "disk", storageFallback;
    uint64_t temporaryFileBytes = 0, ramBufferBytes = 0;
    std::string configId;
    int taskId = 0;
    std::string name;
    std::string flagsStr;
    std::string stage = "Queued", admissionOrigin = "Automatic";
    uint64_t sampleCoveredBytes = 0, sampleTotalBytes = 0, leaderGainBytes = 0;
    uint64_t outputBytes = 0;
    uint64_t originalBytes = 0;
    double compressionRatio = 0.0; // % of original size (smaller is better)
    double durationSeconds = 0.0;
    double verificationSeconds = 0.0, waitSeconds = 0.0;
    bool contentVerified = false;
    uint32_t retryCount = 0;
    bool memoryOverride = false;
    uint64_t peakRamMB = 0;
    uint64_t readBytes = 0, writeBytes = 0; // Main compression process, Windows only.
    int toolExitCode = 0;
    uint32_t win32Error = 0;
    CandidateStatus status = CandidateStatus::Queued;
    ErrorCode errorCode = ErrorCode::None;
    std::string statusMessage;
    bool isLeader = false;
    bool verified = false;
    std::filesystem::path tempArchivePath;
};

struct EngineSnapshot {
    struct ActiveCandidate { std::string group, configId, name, stage; };
    struct MemoryOverrideOffer {
        std::string group, configId, name;
        uint64_t extraMiB = 0, requiredRamMB = 0;
    };
    struct LocalGroup {
        std::string group, leaderName, stage;
        uint64_t leaderBytes = 0;
        bool contentVerified = false;
        uint32_t started = 0, finished = 0, deferred = 0, total = 0;
    };
    MemoryOverrideOffer memoryOverrideOffer;
    bool memoryOverrideEligible = false;
    std::vector<LocalGroup> localGroups;
    uint64_t sourceArchiveBytes = 0, leaderBytes = 0;
    std::string leaderName;
    std::string searchStage;
    bool leaderContentVerified = false;
    uint32_t startedCandidates = 0, finishedCandidates = 0, deferredCandidates = 0;
    double groupAssemblySeconds = 0, groupAssemblyPredictedSeconds = 0;
    struct WaitingCandidate {
        std::string group, configId, name, flags, reason;
        uint64_t requiredRamMB = 0;
        uint32_t requiredCpu = 0;
        bool deferred = false;
    };
    std::vector<ActiveCandidate> activeCandidates;
    std::vector<WaitingCandidate> waitingCandidates; // First 64 only; waitingCandidatesTotal includes all.
    uint64_t waitingCandidatesTotal = 0;
    uint64_t memoryOverrideMB = 0, effectiveRamLimitMB = 0, actualCommittedMB = 0;
    std::string blockedReason;
    EngineState state = EngineState::Idle;
    uint64_t totalFiles = 0, totalBytes = 0, analyzedFiles = 0, analyzedBytes = 0;
    uint64_t reservedRamMB = 0;
    uint32_t activeTasks = 0, reservedCpu = 0;
    uint32_t groupsCompleted = 0, groupsTotal = 0, groupsActive = 0;
    int completed = 0, total = 0;
    float progress = 0, phaseProgress = 0;
    double approximateEtaSeconds = 0;
    double secondsSinceCandidateFinished = 0;
    std::vector<std::string> activeTaskNames;
    std::string error, stopReason;
};

struct SystemTelemetry {
    float cpuUsagePercent = 0.0f;
    uint64_t ramUsedMB = 0;
    uint64_t ramTotalMB = 0;
    float diskReadMBs = 0.0f;
    float diskWriteMBs = 0.0f;
};

struct OptimizerSettings {
    bool enableRamCandidates = false;
    uint64_t ramGroupLimitMB = 40;
    uint64_t ramTemporaryBudgetMB = 1024; // Sub-budget inside ramLimitMB.
    bool enableGrouping = true;
    bool enableSimilarityGroups = false; // Additional competitor; original groups remain eligible.
    bool groupedSearchOnly = false; // Separate grouping ablation; keep Store as fallback.
    bool sortByType = false;
    uint32_t cpuLimitPercent = 0; // Windows job hard cap; 0 disables it.
    uint32_t diskTaskLimit = 0;   // Concurrent disk-producing tasks; not a bandwidth cap.
    VerificationMode verificationMode = VerificationMode::Disk;
    SearchMode searchMode = SearchMode::Exhaustive; // Preserve 2.1 CLI behavior.
    SelectionGoal selectionGoal = SelectionGoal::MinimumSize;
    ContentPolicy contentPolicy = ContentPolicy::ExactBytes;
    AffinityMode affinityMode = AffinityMode::None;
    bool backgroundPriority = false;
    double sizeTolerancePercent = 1.0;
    uint64_t extractionLimitMB = 65536, extractionFileLimitMB = 16384;
    uint32_t extractionFileCount = 1000000;
    OptimizationProfile profile = OptimizationProfile::DeepSearch;
    uint32_t workerThreads = 2;
    bool autoCpu = false;
    uint64_t ramLimitMB = 8192; // 8 GB default cap
    uint64_t maxLzmaDictionaryMB = 0; // 0 = automatic; ceiling for requested LZMA1/LZMA2 history.
    bool verifyArchives = true;
    bool immediateCleanInferior = true;
    bool sfxOutput = false;
    std::filesystem::path toolsBinDir = "bin";
    std::filesystem::path
        customTempDir; // Custom folder for temporary test archives (avoids C: drive exhaustion)

    std::string password;               // AES-256 encryption password
    bool encryptHeaders = false;        // -mhe=on
    uint64_t volumeSizeMB = 0;          // 0 = no split, else -v{size}m
    std::string solidBlockSize;         // "" = default, "on", "off", "e", "1g", "512m", etc.
    bool enablePreprocessors = false;   // Enable Leanify/ECT resource preprocessing
    bool enableAdaptivePruning = false; // Enable quick probe + intermediate check
    double pruningThreshold = 1.2;      // Skip candidates > threshold * leader
    double quickProbePercent = 1.0;     // % of data for quick probe
    uint32_t probeSampleMB = 25, probeBudgetSeconds = 60, probeTimeoutSeconds = 30;
    uint32_t probePasses = 2, fullAttemptLimit = 0;
    bool portableMode = false;          // Store settings next to exe
    bool autoShutdown = false;          // Shutdown PC after completion
    uint32_t cpuThreads = 0;            // 0 = detected physical core count
    uint32_t processTimeoutSeconds = 0; // 0 = unlimited
    uint32_t timeBudgetSeconds = 0;     // 0 = complete search
    std::string language = "en";        // "en" or "ru"
};

struct PreprocessStats {
    std::string stepName;
    uint64_t inputBytes = 0;
    uint64_t outputBytes = 0;
    double durationSeconds = 0.0;
    bool success = true;
    std::string errorMessage;
};

struct SessionInfo {
    std::string sessionId;
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    OptimizerSettings settings;
    std::vector<int> completedTaskIds;
    std::vector<CandidateResult> results;
    int leaderTaskId = -1;
    std::string fingerprint;
    std::filesystem::path workspace;
    std::chrono::system_clock::time_point startTime;
    std::chrono::system_clock::time_point lastCheckpoint;
};

struct PresetInfo {
    std::string name;
    std::string description;
    std::string migrationWarning;
    OptimizerSettings settings;
    bool isBuiltin = false;
};

} // namespace UltraArch
