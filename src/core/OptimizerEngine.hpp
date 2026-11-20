#pragma once
#include "ArchiveAssembler.hpp"
#include "ArchiveExtractor.hpp"
#include "ContentAnalyzer.hpp"
#include "CostModel.hpp"
#include "Diagnostics.hpp"
#include "PreprocessorPipeline.hpp"
#include "ProcessManager.hpp"
#include "ResourceGuard.hpp"
#include "ResourceScheduler.hpp"
#include "AdmissionBroker.hpp"
#include "RamCandidate.hpp"
#include "RamInput.hpp"
#include "SessionManager.hpp"
#include "SettingsJson.hpp"
#include "TaskMatrixGenerator.hpp"
#include "Types.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
namespace UltraArch {
class OptimizerEngine {
  public:
    OptimizerEngine() = default;
    ~OptimizerEngine();
    void setSettings(const OptimizerSettings &settings);
    OptimizerSettings getSettings() const;
    bool start(const std::filesystem::path &input, const std::filesystem::path &output);
    void pause();
    void resume();
    void skipCurrent();
    bool skipCandidate(const std::string &group, const std::string &configId);
    bool skipGroup(const std::string &group);
    bool startNextCandidate();
    bool requestMemoryOverride();
    bool requestMemoryOverride(const std::string &group, const std::string &configId, uint64_t maxExtraMiB);
    void finishAndAssembleBest();
    void cancel();
    void wait(); // Join before destroying callback recipients.
    EngineState getState() const;
    std::vector<CandidateResult> getResults() const;
    CandidateResult getBestLeader() const;
    AnalysisSummary getAnalysisSummary() const;
    EngineSnapshot getSnapshot() const;
    Json getRunReport() const;
    bool selectAndAssemble(const std::string &configId);
    bool retest(const std::string &configId);
    std::vector<SessionInfo> sessions(const std::filesystem::path &base) const;
    bool discardSession(const SessionInfo &session);
    bool exportSessionLeader(const SessionInfo &session, const std::filesystem::path &target,
                             const std::string &password,
                             const std::atomic<bool> *cancel = nullptr);
    bool resumeSession(const SessionInfo &session, const std::string &password);
    bool saveRunReport(const std::filesystem::path &path) const;
    std::filesystem::path getLogPath() const;
    float getProgress() const;
    double getEstimatedTimeRemaining() const;
    int getCompletedCount() const;
    int getTotalCount() const;
    std::string getCurrentTaskName() const;
    std::string getLastError() const;
    std::vector<std::filesystem::path> getOutputFiles() const;
    void setLogCallback(std::function<void(const std::string &)> cb);
    void setCandidateCallback(std::function<void(const CandidateResult &)> cb);
    void setStateCallback(std::function<void(EngineState)> cb);
    void setProgressCallback(std::function<void(float, int, int)> cb);

  private:
    friend struct OptimizerEngineTestAccess;
    struct RunContext {
        OptimizerSettings settings;
        std::filesystem::path input, output, effectiveInput, workspace, list;
        std::string sessionId, fingerprint, inputFingerprint, effectiveFingerprint;
        std::chrono::steady_clock::time_point start;
    } m_run;
    void coordinatorLoop();
    void workerFunc(uint32_t index);
    std::shared_ptr<AdmissionBroker::Lease> acquireLease(const std::string &key, uint64_t ram,
        uint32_t cpu, const std::atomic<bool> *taskCancel = nullptr);
    void changeState(EngineState state);
    void notifyState(EngineState state);
    void log(const std::string &message);
    bool checkpoint();
    void cleanupWorkspace();
    void runQuickProbe(std::vector<TaskConfig> &tasks, uint32_t ordinal = 0);
    bool runGroupedSearch(const std::filesystem::path &archive, uint32_t worker,
                          const ProcessControl &control, ProcessResult &accounting, bool similarity = false);
    std::string fingerprintInput(const std::filesystem::path &input) const;
    InputManifest manifestForInput(const std::filesystem::path &input) const;
    bool busy() const;
    OptimizerSettings m_settings;
    mutable std::mutex m_mutex, m_callbackMutex, m_checkpointMutex, m_logFileMutex;
    std::mutex m_materializeMutex;
    std::condition_variable m_cv;
    std::thread m_coordinator;
    std::vector<std::thread> m_workers;
    std::atomic<EngineState> m_state{EngineState::Idle};
    std::atomic<bool> m_cancel{false}, m_pause{false}, m_finish{false};
    std::vector<TaskConfig> m_tasks;
    std::vector<CandidateResult> m_results;
    std::vector<std::shared_ptr<std::atomic<bool>>> m_taskCancel;
    CandidateResult m_best;
    bool m_hasDurableCheckpoint = false; // Protected by m_mutex; reset for each run.
    AnalysisSummary m_summary;
    std::vector<std::filesystem::path> m_outputs, m_changedFiles;
    std::string m_error, m_stopReason, m_manualWinner, m_requestedSession;
    uint64_t m_smallestBytes = UINT64_MAX;
    uint32_t m_generatedCount = 0;
    uint32_t m_groupsCompleted = 0, m_groupsTotal = 0, m_groupsActive = 0;
    std::filesystem::path m_logPath;
    StructuredError m_structuredError;
    bool acquireDisk(const std::shared_ptr<AdmissionBroker::Lease> &lease,
                     const std::atomic<bool> *taskCancel = nullptr,
                     const std::function<bool()> &stop = {});
    StageMetrics m_metrics;
    Json m_applicationIOStart, m_applicationIO;
    double m_searchClosedElapsed = -1; // First global search-stop request or normal drain.
    void markSearchClosed(); // Caller holds m_mutex.
    std::map<int, double> m_phaseSeconds;
    std::chrono::steady_clock::time_point m_phaseStart;
    uint64_t m_peakScratchBytes = 0;
    std::atomic<uint64_t> m_probeFileWriteBytes{0}, m_ramProbes{0};
    std::atomic<uint64_t> m_probeInputWriteBytes{0}, m_materializedInputBytes{0};
    uint64_t m_compatibleInputArchiveBytes = 0;
    Json m_toolIdentities = Json::array();
    Json m_groupReports = Json::array(), m_probeEvidence = Json::array(), m_assemblyEvidence = Json::array();
    double m_groupAssemblySeconds = 0, m_groupAssemblyPredictedSeconds = 0;
    uint64_t m_analyzedFiles = 0, m_analyzedBytes = 0;
    uint64_t m_reservedMemory = 0;
    int m_completed = 0, m_active = 0;
    double m_durationSum = 0, m_eta = 0, m_finishedElapsed = -1;
    double m_finalizationStartedElapsed = -1; // Protected by m_mutex; relative to run start.
    uint32_t m_fullAttemptsStarted = 0;
    std::unique_ptr<CostModel> m_costModel;
    std::map<int, double> m_predictedSeconds;
    std::map<int, std::chrono::steady_clock::time_point> m_taskStarts;
    std::map<size_t, std::string> m_activeGroupKeys;
    std::chrono::steady_clock::time_point m_lastCandidateFinished;
    float m_progress = 0;
    size_t m_admissionLimit = SIZE_MAX;
    ResourceScheduler m_scheduler;
    std::shared_ptr<AdmissionBroker> m_broker;
    std::map<std::string, std::shared_ptr<AdmissionBroker::Lease>> m_overrideLeases;
    std::map<std::string, OptimizerEngine *> m_groupEngines; // Lifetime protected by m_mutex.
    bool m_retainedGroups = false, m_searchInterrupted = false;
    std::string m_stopAction, m_publicationOutcome, m_searchStage;
    std::filesystem::path m_seedArchive;
    std::string m_seedConfig;
    std::vector<CandidateResult> m_seedResults;
    std::atomic<bool> m_earlyAssembly{false};
    bool m_baselineOnly = false;
    bool m_startNextPending = false;
    uint32_t m_executorTarget = 1;
    void event(const std::string &stage, const std::string &configId = "", uint32_t pid = 0,
               const std::string &action = "", const Json &details = Json::object());
    uint64_t m_diagnosticBytes = 0;
    ResourceGuard m_guard;
    ResourceGuard *m_sharedGuard = nullptr; // Group children share the parent's hard job limits.
    std::shared_ptr<RamBudget> m_ramBudget;
    std::vector<FileTypeInfo> m_memoryFiles, m_injectedMemoryFiles;
    uint64_t m_inputRamReservedMB = 0, m_inputRamBytes = 0, m_inputDiskBytes = 0;
    double m_inputDecoderCpuSeconds = 0;
    Json m_inputDecodeMetrics = Json::object();
    uint32_t m_workerOffset = 0;
    ResourceGuard &activeGuard() { return m_sharedGuard ? *m_sharedGuard : m_guard; }
    const ResourceGuard &activeGuard() const { return m_sharedGuard ? *m_sharedGuard : m_guard; }
    std::mutex m_capabilityCacheMutex;
    SessionManager m_sessions;
    std::function<void(const std::string &)> m_logCallback;
    std::function<void(const CandidateResult &)> m_candidateCallback;
    std::function<void(EngineState)> m_stateCallback;
    std::function<void(float, int, int)> m_progressCallback;
};
} // namespace UltraArch
