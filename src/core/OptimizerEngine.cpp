#include "OptimizerEngine.hpp"
#include "InputManifest.hpp"
#include "GroupPlanner.hpp"
#include "GroupCheckpoint.hpp"
#include "ProbePlanner.hpp"
#include "MemoryModel.hpp"
#include "PublicationTransaction.hpp"
#include "SearchPlanner.hpp"
#include "SettingsJson.hpp"
#include "ToolCapabilities.hpp"
#include "StreamingVerifier.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <deque>
#include <future>
#include <numeric>
#include <optional>
#include <set>
#include <ctime>
namespace UltraArch {
namespace fs = std::filesystem;
namespace {
Json applicationIO() {
#ifdef _WIN32
    IO_COUNTERS counters{};
    if (GetProcessIoCounters(GetCurrentProcess(), &counters))
        return {{"readBytes", counters.ReadTransferCount}, {"writeBytes", counters.WriteTransferCount}};
#endif
    return nullptr;
}
std::string digestText(const std::string &text) {
    return Hasher::text(text);
}
void rotateDiagnostic(const fs::path &path, uint64_t maxBytes = 16ULL * 1024 * 1024) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) < maxBytes || ec) return;
    auto rotated = path; rotated += ".1";
    fs::remove(rotated, ec); ec.clear();
    fs::rename(path, rotated, ec);
}
uint32_t coresForGrouping(const OptimizerSettings &settings) {
    return settings.cpuThreads ? settings.cpuThreads
                              : ResourceGuard::availableCpuThreads(static_cast<int>(settings.affinityMode));
}
} // namespace
bool OptimizerEngine::acquireDisk(const std::shared_ptr<AdmissionBroker::Lease> &lease,
    const std::atomic<bool> *taskCancel, const std::function<bool()> &stop) {
    StageMetrics::Scope timing(&m_metrics, "DiskAdmission");
    if (!m_broker || !lease) return true;
    while (!m_cancel && !(taskCancel && taskCancel->load()) && !(stop && stop())) {
        if (m_broker->snapshot().admissionsBlocked) return false;
        if (m_broker->tryEnableDisk(lease)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
std::shared_ptr<AdmissionBroker::Lease> OptimizerEngine::acquireLease(const std::string &key,
    uint64_t ram, uint32_t cpu, const std::atomic<bool> *taskCancel) {
    StageMetrics::Scope admission(&m_metrics, "ResourceAdmission");
    if (!m_broker) return {};
    const auto limits = m_broker->snapshot();
    if (limits.admissionsBlocked) throw std::runtime_error(limits.blockReason);
    if (ram > limits.ramLimit || cpu > limits.cpuLimit)
        throw std::runtime_error("Resource requirement exceeds the effective hard admission budget");
    while (!m_cancel && !(taskCancel && taskCancel->load())) {
        const auto current = m_broker->snapshot();
        if (current.admissionsBlocked) { m_broker->withdraw(key); throw std::runtime_error(current.blockReason); }
        if (auto lease = m_broker->tryAcquire(key, ram, cpu)) return lease;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    m_broker->withdraw(key);
    throw std::runtime_error("Cancelled while waiting for resource admission");
}
bool OptimizerEngine::runGroupedSearch(const fs::path &archive, uint32_t worker,
                                      const ProcessControl &control, ProcessResult &accounting, bool similarity) {
    auto groups = GroupPlanner::plan(m_summary, similarity);
    if (groups.empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groupsTotal = static_cast<uint32_t>(groups.size());
        m_groupsCompleted = m_groupsActive = 0;
    }
    const auto root = fs::is_directory(m_run.effectiveInput) ? m_run.effectiveInput
                                                            : m_run.effectiveInput.parent_path();
    const auto directory = m_run.workspace / (similarity ? "similarity-groups" : "groups");
    fs::create_directories(directory);
    std::atomic<bool> abortGroups{false};
    auto stopped = [&] {
        return abortGroups || m_cancel || m_finish || (control.taskCancel && control.taskCancel->load());
    };
    struct GroupOutcome {
        TaskConfig winner;
        ProcessResult counters;
        bool complete = false;
        bool searchComplete = false;
        double compressionSeconds = 0;
    };
    auto searchOne = [&](const std::string &key, const std::vector<FileTypeInfo> &files,
                         size_t number, OptimizerSettings settings, uint32_t workerOffset) -> GroupOutcome {
        ProcessResult counters;
        if (stopped())
            return {};
        auto groupRoot = directory / Hasher::text(key).substr(0, 16);
        auto input = groupRoot / "input";
        auto output = groupRoot / "winner.7z";
        auto marker = groupRoot / "winner.json";
        try { output = GroupCheckpoint::artifact(groupRoot, GroupCheckpoint::read(groupRoot)); } catch (...) {}
        fs::create_directories(input);
        for (const auto &file : files) {
            if (stopped())
                return {};
            auto copy = input / file.path.lexically_relative(root);
            if (file.memoryContent) {
                fs::create_directories(copy.parent_path());
                continue;
            }
            if (!fs::exists(copy)) {
                fs::create_directories(copy.parent_path());
                std::error_code error;
                fs::create_hard_link(file.path, copy, error);
                if (error) {
                    auto copyLease = acquireLease(m_run.sessionId + ":group-input:" + key, 16, 0, control.taskCancel);
                    fs::copy_file(file.path, copy);
                }
            }
        }
        if (number == 1)
            for (const auto &empty : m_summary.emptyDirectories)
                fs::create_directories(input / empty.lexically_relative(root));
        settings.enableGrouping = false;
        settings.enableSimilarityGroups = false;
        settings.groupedSearchOnly = false;
        settings.enablePreprocessors = false;
        settings.contentPolicy = ContentPolicy::ExactBytes;
        settings.sfxOutput = false;
        settings.volumeSizeMB = 0;
        settings.customTempDir = groupRoot / "search";
        settings.solidBlockSize = m_run.settings.solidBlockSize.empty() ? "on" : m_run.settings.solidBlockSize;
        settings.sortByType = m_run.settings.sortByType;
        if (settings.timeBudgetSeconds) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_run.start).count();
            if (elapsed >= settings.timeBudgetSeconds)
                return {};
            settings.timeBudgetSeconds -= static_cast<uint32_t>(elapsed);
        }
        auto manifest = InputManifest::scan(input, &m_cancel);
        std::vector<FileTypeInfo> memoryFiles;
        for (const auto &file : files)
            if (file.memoryContent) {
                auto memoryFile = file;
                memoryFile.path = input / file.path.lexically_relative(root);
                memoryFiles.push_back(std::move(memoryFile));
            }
        if (!memoryFiles.empty()) {
            auto combined = manifest.summary.files;
            combined.insert(combined.end(), memoryFiles.begin(), memoryFiles.end());
            auto empty = manifest.summary.emptyDirectories;
            empty.erase(std::remove_if(empty.begin(), empty.end(), [&](const auto &directory) {
                return std::any_of(memoryFiles.begin(), memoryFiles.end(), [&](const auto &file) {
                    auto relative = file.path.lexically_relative(directory);
                    return !relative.empty() && *relative.begin() != "..";
                });
            }), empty.end());
            manifest = InputManifest::fromEntries(input, std::move(combined), std::move(empty));
        }
        auto canonical = settings;
        canonical.customTempDir.clear();
        canonical.timeBudgetSeconds = m_run.settings.timeBudgetSeconds;
        auto identity = Hasher::text("grouped-search-9:" + manifest.fingerprint +
                                    m_toolIdentities.dump() + settingsJson(canonical).dump());
        if (similarity && !fs::exists(output)) {
            const auto previous = m_run.workspace / "groups" / Hasher::text(key).substr(0, 16);
            const auto priorMarker = previous / "winner.json";
            auto priorOutput = previous / "winner.7z";
            try { priorOutput = GroupCheckpoint::artifact(previous, GroupCheckpoint::read(previous)); } catch (...) {}
            if (fs::is_regular_file(priorMarker) && fs::is_regular_file(priorOutput) &&
                !isLinkOrReparse(priorMarker) && !isLinkOrReparse(priorOutput)) {
                try {
                    Json saved; std::ifstream in(priorMarker); in >> saved;
                    if (saved.value("identity", "") == identity &&
                        saved.value("sha256", "") == fileChecksum(priorOutput, &m_cancel)) {
                        auto copyLease = acquireLease(m_run.sessionId + ":group-cache-copy:" + key, 16, 0, control.taskCancel);
                        GroupCheckpoint::copyTo(previous, groupRoot);
                        output = GroupCheckpoint::artifact(groupRoot, GroupCheckpoint::read(groupRoot));
                    }
                } catch (...) { if (m_cancel) throw; }
            }
        }
        auto matrix = TaskMatrixGenerator{}.generateMatrix(manifest.summary, settings);
        TaskConfig winner;
        bool cached = false;
        try {
            std::ifstream saved(marker);
            Json record;
            saved >> record;
            if (record.value("identity", "") == identity && record.value("searchComplete", true) && fs::is_regular_file(output) &&
                record.value("sha256", "") == fileChecksum(output, &m_cancel)) {
                auto task = std::find_if(matrix.begin(), matrix.end(), [&](const auto &candidate) {
                    return candidate.configId == record.value("configId", "");
                });
                if (task != matrix.end()) {
                    auto lease = acquireLease(m_run.sessionId + ":cache:" + key,
                        std::max<uint64_t>(64, task->spec.memoryMB(false)), 1, control.taskCancel);
                    std::string error;
                    cached = ArchiveAssembler::verifyArchive(output, settings.toolsBinDir, error,
                        settings.password, &m_cancel, &activeGuard(), settings.processTimeoutSeconds, &control);
                    winner = *task;
                }
            }
        } catch (...) {
        }
        log("[Group " + std::to_string(number) + "/" + std::to_string(groups.size()) + "] " + key +
            ": " + std::to_string(files.size()) + " files" + (cached ? " (verified cache)" : ""));
        if (cached) {
            Json report = {{"groupKey", key}, {"cached", true}, {"winner", winner.name},
                           {"outputBytes", fs::file_size(output)}};
            std::lock_guard<std::mutex> lock(m_mutex);
            m_groupReports.push_back(std::move(report));
            m_retainedGroups = true;
        }
        if (!cached) {
            // Reuse only capability records tied to the executable, DLL and typed graph hashes.
            // Each child writes its own copy; the parent cache stays immutable during search.
            auto capabilities = m_run.workspace.parent_path() / "capabilities-v2-sha256.json";
            fs::create_directories(settings.customTempDir);
            {
                std::lock_guard<std::mutex> lock(m_capabilityCacheMutex);
                if (fs::is_regular_file(capabilities))
                    fs::copy_file(capabilities, settings.customTempDir / "capabilities-v2-sha256.json",
                                  fs::copy_options::overwrite_existing);
            }
            OptimizerEngine search;
            search.m_sharedGuard = &activeGuard();
            search.m_broker = m_broker;
            search.m_ramBudget = m_ramBudget;
            search.m_injectedMemoryFiles = memoryFiles;
            search.m_workerOffset = workerOffset;
            search.m_baselineOnly = m_earlyAssembly;
            search.setSettings(settings);
            search.setLogCallback([this, key](const auto &line) { log("[Group " + key + "] " + line); });
            std::mutex groupCheckpointMutex;
            std::string durableConfigId;
            auto joinSearch = std::shared_ptr<void>(nullptr, [&](void *) { search.cancel(); search.wait(); });
            search.setCandidateCallback([&, this](const auto &) {
                std::lock_guard checkpointLock(groupCheckpointMutex);
                const auto best = search.getBestLeader();
                if (best.verified && !best.tempArchivePath.empty() && durableConfigId != best.configId) {
                    // Save every durable leader, including a leader in an unfinished group.
                    StageMetrics::Scope groupCheckpoint(&m_metrics, "GroupCheckpoint");
                    auto copyLease = acquireLease(m_run.sessionId + ":group-checkpoint:" + key, 16, 0, control.taskCancel);
                    output = GroupCheckpoint::publish(groupRoot, best.tempArchivePath,
                        Json{{"identity", identity}, {"configId", best.configId}, {"searchComplete", false},
                             {"contentVerified", best.contentVerified}, {"compressionSeconds", best.durationSeconds}});
                    groupCheckpoint.finish();
                    copyLease.reset();
                    durableConfigId = best.configId;
                    {
                        std::lock_guard lock(m_mutex);
                        m_retainedGroups = true;
                    }
                    checkpoint();
                }
                std::lock_guard lock(m_mutex);
                m_lastCandidateFinished = std::chrono::steady_clock::now();
            });
            if (fs::is_regular_file(marker) && fs::is_regular_file(output)) {
                try {
                    Json record; std::ifstream in(marker); in >> record;
                    if (record.value("identity", "") == identity &&
                        record.value("sha256", "") == fileChecksum(output, &m_cancel)) {
                        search.m_seedArchive = output;
                        search.m_seedConfig = record.value("configId", "");
                        if (fs::is_regular_file(groupRoot / "search-report.json")) {
                            Json previous; std::ifstream report(groupRoot / "search-report.json"); report >> previous;
                            for (const auto &candidate : previous.value("results", Json::array()))
                                search.m_seedResults.push_back(resultFromJson(candidate));
                        }
                    }
                } catch (...) { if (m_cancel) throw; }
            }
            const auto controlKey = std::string(similarity ? "similarity/" : "") + key;
            {
                std::lock_guard lock(m_mutex);
                m_groupEngines[controlKey] = &search;
            }
            auto unregister = std::shared_ptr<void>(nullptr, [&, this](void *) {
                std::lock_guard lock(m_mutex);
                m_groupEngines.erase(controlKey);
            });
            const auto childOutput = groupRoot / "published.7z";
            if (!search.start(input, childOutput))
                throw std::runtime_error("Cannot start group search");
            while (search.busy()) {
                auto snapshot = search.getSnapshot();
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto &detail = m_activeGroupKeys[number - 1];
                    detail = key;
                    for (const auto &name : snapshot.activeTaskNames) detail += " | " + name;
                }
                if (stopped())
                    search.cancel();
                else if (m_earlyAssembly && search.getBestLeader().taskId > 0)
                    search.finishAndAssembleBest();
                else if (m_pause)
                    search.pause();
                else
                    search.resume();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            search.wait();
            auto report = search.getRunReport();
            report["groupKey"] = key;
            report["cached"] = false;
            writeAtomically(groupRoot / "search-report.json", report.dump(2));
            if (stopped() || search.getState() == EngineState::Cancelled) {
                std::lock_guard lock(m_mutex);
                m_groupReports.push_back(report);
                m_searchInterrupted = true;
                return {};
            }
            if (search.getState() != EngineState::Completed)
                throw std::runtime_error("Group search failed: " + search.getLastError());
            auto childCapabilities = settings.customTempDir / "capabilities-v2-sha256.json";
            if (fs::is_regular_file(childCapabilities)) {
                std::lock_guard<std::mutex> lock(m_capabilityCacheMutex);
                Json merged = Json::object(), child;
                try { std::ifstream in(capabilities); in >> merged; } catch (...) {}
                std::ifstream in(childCapabilities);
                in >> child;
                if (!merged.is_object()) merged = Json::object();
                if (child.is_object()) merged.update(child);
                writeAtomically(capabilities, merged.dump(2));
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_groupReports.push_back(report);
                if (m_costModel)
                    m_costModel->record(manifest.summary, search.getResults());
            }
            counters.peakMemoryMB = search.activeGuard().getPeakMemoryMB();
            for (const auto &candidate : search.getResults()) {
                counters.readBytes += candidate.readBytes;
                counters.writeBytes += candidate.writeBytes;
            }
            auto best = search.getBestLeader();
            auto task = std::find_if(matrix.begin(), matrix.end(), [&](const auto &candidate) {
                return candidate.configId == best.configId;
            });
            if (task == matrix.end())
                throw std::runtime_error("Group winner missing from typed matrix");
            winner = *task;
            // Commit the actual final leader even if an earlier callback failed.
            StageMetrics::Scope groupCheckpoint(&m_metrics, "GroupCheckpoint");
            auto copyLease = acquireLease(m_run.sessionId + ":group-final-checkpoint:" + key, 16, 0, control.taskCancel);
            output = GroupCheckpoint::publish(groupRoot, childOutput,
                Json{{"identity", identity}, {"configId", winner.configId},
                     {"searchComplete", !search.m_baselineOnly && !report.value("searchInterrupted", false)},
                     {"contentVerified", true}, {"compressionSeconds", best.durationSeconds}});
            groupCheckpoint.finish();
            copyLease.reset();
            { std::error_code ignored; fs::remove(childOutput, ignored); }
            {
                std::lock_guard lock(m_mutex);
                m_retainedGroups = true;
            }
            checkpoint();
        }
        log("[Group ready] " + key + ": " + winner.name);
        bool searchComplete = true;
        double compressionSeconds = 0;
        try { Json saved; std::ifstream in(marker); in >> saved;
            searchComplete = saved.value("searchComplete", true);
            compressionSeconds = saved.value("compressionSeconds", 0.0);
        } catch (...) {}
        return {winner, counters, true, searchComplete, compressionSeconds};
    };
    struct Work {
        std::string key;
        const std::vector<FileTypeInfo> *files;
        OptimizerSettings settings;
        GroupOutcome outcome;
        double estimatedSeconds = 0;
    };
    auto cpu = coresForGrouping(m_run.settings);
    auto workers = m_run.settings.workerThreads;
    const auto slots = std::min({8u, workers, cpu});
    std::vector<Work> work;
    for (const auto &[key, files] : groups) {
        AnalysisSummary summary;
        summary.totalFiles = files.size();
        summary.files = files;
        for (const auto &file : files) {
            summary.totalBytes += file.sizeBytes;
            summary.categoryBytes[file.category] += file.sizeBytes;
            summary.hasExecutables |= file.category == FileCategory::Executable_PE;
            summary.hasUncompressedMedia |= file.category == FileCategory::Uncompressed_Media;
            summary.hasStructuredText |= file.category == FileCategory::Text_Database;
        }
        for (const auto &[category, bytes] : summary.categoryBytes)
            if (bytes >= summary.categoryBytes[summary.dominantCategory])
                summary.dominantCategory = category;
        auto settings = m_run.settings;
        settings.ramLimitMB -= m_inputRamReservedMB;
        settings.enableGrouping = false;
        settings.enableSimilarityGroups = false;
        settings.groupedSearchOnly = false;
        // Local concurrency is a soft ceiling. Only admitted leaf jobs consume
        // global resources; a group never reserves idle executor slots.
        settings.workerThreads = summary.totalBytes >= 64ULL * 1048576 ? std::min(2u, workers) : 1;
        settings.cpuThreads = std::min(cpu, settings.workerThreads * 2);
        settings.autoCpu = false; // Parent already resolved availability; preserve local worker ranges.
        auto matrix = TaskMatrixGenerator{}.generateMatrix(summary, settings);
        auto estimatedTasks = settings.searchMode == SearchMode::Exhaustive ? matrix
            : SearchPlanner::smallInputFinalists(matrix, settings.searchMode);
        double cost = 0;
        if (m_costModel)
            for (const auto &task : estimatedTasks)
                cost += m_costModel->estimateSeconds(summary, task);
        work.push_back({key, &files, settings, {}, std::max(0.01, cost)});
    }
    // Dispatch expensive groups first without changing final archive assembly order.
    std::vector<size_t> queue(work.size());
    for (size_t i = 0; i < queue.size(); ++i) queue[i] = i;
    std::stable_sort(queue.begin(), queue.end(), [&](size_t a, size_t b) {
        return work[a].estimatedSeconds > work[b].estimatedSeconds;
    });
    // Interleave expensive heads with cheap backfills. A bounded set of group
    // coordinators must not all wait behind heavy leaves while tiny groups wait.
    const auto sortedQueue = queue;
    queue.clear();
    for (size_t head = 0, tail = sortedQueue.size(); head < tail;) {
        queue.push_back(sortedQueue[head++]);
        if (head < tail) queue.push_back(sortedQueue[--tail]);
    }
    double totalCost = 0, completedCost = 0;
    for (const auto &job : work) totalCost += job.estimatedSeconds;
    struct Pending { size_t index; std::future<GroupOutcome> result; };
    std::vector<Pending> pending;
    struct AbortOnExit {
        std::atomic<bool> &flag;
        ~AbortOnExit() { flag = true; }
    } abortOnExit{abortGroups};
    std::vector<bool> dispatched(work.size(), false);
    std::exception_ptr failure;
    size_t completed = 0;
    while (completed < work.size() && !failure && !stopped()) {
        bool advanced = false;
        for (const auto i : queue) {
            if (pending.size() >= slots) break;
            const auto &job = work[i];
            if (dispatched[i]) continue;
            dispatched[i] = true;
            pending.push_back({i, std::async(std::launch::async, [&, i] {
                return searchOne(work[i].key, *work[i].files, i + 1, work[i].settings, 0);
            })});
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_groupsActive;
                m_activeGroupKeys[i] = job.key;
            }
            advanced = true;
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto &job = work[it->index];
            try { job.outcome = it->result.get(); } catch (...) {
                failure = std::current_exception();
                abortGroups = true;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_groupsActive;
                m_groupsCompleted += job.outcome.searchComplete;
                if (!job.outcome.searchComplete) {
                    m_searchInterrupted = true;
                    if (m_stopAction.empty()) {
                        m_stopAction = "IncompleteGroupSearch";
                        m_stopReason = "At least one group search remains incomplete; durable leaders retained";
                    }
                }
                m_activeGroupKeys.erase(it->index);
                if (job.outcome.complete) completedCost += job.estimatedSeconds;
                m_progress = std::max(m_progress, static_cast<float>(0.88 * completedCost / totalCost));
                m_eta = std::max(0.0, totalCost - completedCost) / slots;
            }
            it = pending.erase(it);
            ++completed;
            advanced = true;
        }
        if (!advanced)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Always join children before the parent workspace or shared job can be destroyed.
    for (auto &job : pending) {
        try { work[job.index].outcome = job.result.get(); } catch (...) {
            if (!failure) failure = std::current_exception();
            abortGroups = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groupsActive = 0;
        m_activeGroupKeys.clear();
    }
    if (failure)
        std::rethrow_exception(failure);
    {
        std::lock_guard lock(m_mutex);
        for (size_t i = 1; i < work.size(); ++i)
            m_groupAssemblyPredictedSeconds += work[i].outcome.compressionSeconds;
    }
    size_t number = 0;
    for (auto &job : work) {
        if (m_earlyAssembly && !stopped() && !job.outcome.complete) {
            log("[Assembly fallback] Complete coverage required for group " + job.key);
            job.outcome = searchOne(job.key, *job.files, static_cast<size_t>(&job - work.data()) + 1, job.settings, 0);
        }
        if (stopped() || !job.outcome.complete)
            return false;
        ++number;
        const auto &key = job.key;
        const auto &files = *job.files;
        const auto &winner = job.outcome.winner;
        const auto &settings = job.settings;
        auto groupRoot = directory / Hasher::text(key).substr(0, 16);
        auto output = GroupCheckpoint::artifact(groupRoot, GroupCheckpoint::read(groupRoot));
        accounting.peakMemoryMB = std::max(accounting.peakMemoryMB, job.outcome.counters.peakMemoryMB);
        accounting.readBytes += job.outcome.counters.readBytes;
        accounting.writeBytes += job.outcome.counters.writeBytes;
        if (number == 1) {
            const auto started = std::chrono::steady_clock::now();
            auto copyLease = acquireLease(m_run.sessionId + ":assembly-copy:" + key, 16, 0, control.taskCancel);
            fs::copy_file(output, archive, fs::copy_options::overwrite_existing);
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::lock_guard lock(m_mutex);
            m_groupAssemblySeconds += elapsed;
            m_assemblyEvidence.push_back({{"group", key}, {"method", "CopyFirstGroup"}, {"elapsedSeconds", elapsed}});
        } else {
            auto started = std::chrono::steady_clock::now();
            auto lease = acquireLease(m_run.sessionId + ":append:" + key,
                std::max(winner.estimatedRamMB, winner.spec.memoryMB(false)), winner.cpuTokens, control.taskCancel);
            auto list = groupRoot / "assembly.txt";
            std::ofstream names(list, std::ios::binary);
            for (const auto &file : files)
                if (RamInput::materialize(file)) m_materializedInputBytes += file.sizeBytes;
            for (const auto &file : files)
                names << pathUtf8(file.path.lexically_relative(root)) << '\n';
            names.close();
            if (!names)
                throw std::runtime_error("Cannot write grouped assembly list");
            std::vector<std::string> args = {"a", "-y", "-t7z", "-scsUTF-8", "-sccUTF-8", "-spd"};
            args.insert(args.end(), winner.flags.begin(), winner.flags.end());
            args.push_back(pathUtf8(archive));
            args.push_back("@" + pathUtf8(list));
            auto result = ProcessManager::execute(pathUtf8(ProcessManager::resolveExecutable("7z", settings.toolsBinDir)),
                args, root, &activeGuard(), worker, &m_cancel, nullptr, &control);
            if (!result.success)
                throw std::runtime_error("Grouped assembly failed: " + result.errOutput);
            accounting.peakMemoryMB = std::max(accounting.peakMemoryMB, result.peakMemoryMB);
            accounting.readBytes += result.readBytes;
            accounting.writeBytes += result.writeBytes;
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            {
                std::lock_guard lock(m_mutex);
                m_groupAssemblySeconds += elapsed;
                m_assemblyEvidence.push_back({{"group", key}, {"method", "ReencodeAppend"},
                    {"elapsedSeconds", elapsed}, {"compressionSeconds", result.durationSeconds},
                    {"waitAndPreparationSeconds", std::max(0.0, elapsed - result.durationSeconds)},
                    {"predictedSeconds", job.outcome.compressionSeconds}});
            }
            log("[Group assembly] Re-encoded " + key + " in " +
                std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()) +
                " seconds; compressed-block reuse unavailable for this backend path.");
        }
        log("[Group winner] " + key + ": " + winner.name + "; assembled " +
            std::to_string(fs::file_size(archive)) + " bytes");
    }
    return true;
}
OptimizerEngine::~OptimizerEngine() {
    cancel();
    wait();
}
bool OptimizerEngine::busy() const {
    auto s = m_state.load();
    return s != EngineState::Idle && s != EngineState::Completed && s != EngineState::Failed &&
           s != EngineState::Cancelled;
}
void OptimizerEngine::wait() {
    if (m_coordinator.joinable())
        m_coordinator.join();
}
void OptimizerEngine::setSettings(const OptimizerSettings &settings) {
    validateSettings(settings);
    std::lock_guard<std::mutex> l(m_mutex);
    if (busy())
        throw std::logic_error("Cannot change settings during a run");
    m_settings = normalizeSearchSettings(settings);
}
OptimizerSettings OptimizerEngine::getSettings() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_settings;
}
EngineState OptimizerEngine::getState() const {
    return m_state.load();
}
std::vector<CandidateResult> OptimizerEngine::getResults() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_results;
}
CandidateResult OptimizerEngine::getBestLeader() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_best;
}
AnalysisSummary OptimizerEngine::getAnalysisSummary() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_summary;
}
EngineSnapshot OptimizerEngine::getSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    EngineSnapshot s;
    s.state = m_state;
    s.totalFiles = m_summary.totalFiles;
    s.totalBytes = m_summary.totalBytes;
    s.sourceArchiveBytes = m_compatibleInputArchiveBytes;
    s.leaderBytes = m_best.outputBytes;
    s.leaderName = m_best.name;
    s.searchStage = m_searchStage;
    s.leaderContentVerified = m_best.contentVerified;
    s.groupAssemblySeconds = m_groupAssemblySeconds;
    s.groupAssemblyPredictedSeconds = m_groupAssemblyPredictedSeconds;
    s.analyzedFiles = m_analyzedFiles;
    s.analyzedBytes = m_analyzedBytes;
    s.reservedCpu = busy() ? m_scheduler.cpuReserved : 0;
    s.reservedRamMB = busy() ? m_reservedMemory + m_inputRamReservedMB : 0;
    s.activeTasks = static_cast<uint32_t>(m_active);
    s.effectiveRamLimitMB = activeGuard().getMemoryLimitMB();
    s.actualCommittedMB = activeGuard().getCurrentMemoryMB();
    s.memoryOverrideMB = s.effectiveRamLimitMB > m_run.settings.ramLimitMB ?
        s.effectiveRamLimitMB - m_run.settings.ramLimitMB : 0;
    if (m_broker && busy()) {
        auto resources = m_broker->snapshot();
        s.reservedCpu = resources.cpuReserved;
        s.reservedRamMB = resources.ramReserved + resources.persistent;
        s.activeTasks = resources.active;
        if (!resources.blockReason.empty()) s.blockedReason = resources.blockReason;
        else if (resources.ramReserved >= resources.ramLimit) s.blockedReason = "RAM reservation budget";
        else if (resources.cpuReserved >= resources.cpuLimit) s.blockedReason = "CPU token budget";
        else if (resources.active >= resources.executorLimit) s.blockedReason = "Concurrent executor limit";
        else if (resources.diskActive >= resources.diskLimit) s.blockedReason = "Disk slots occupied; RAM execution remains eligible";
    }
    s.groupsCompleted = m_groupsCompleted;
    s.groupsTotal = m_groupsTotal;
    s.groupsActive = m_groupsActive;
    s.completed = m_completed;
    s.total = static_cast<int>(m_results.size());
    s.stopReason = m_stopReason;
    s.phaseProgress = s.state == EngineState::AssemblingFinal     ? (m_progress - 0.9f) * 10
                      : s.state == EngineState::TestingCandidates ? m_progress / 0.9f
                                                                  : 0;
    s.progress = m_progress;
    s.approximateEtaSeconds = m_eta;
    s.secondsSinceCandidateFinished = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_lastCandidateFinished).count();
    for (const auto &result : m_results) {
        if (result.status == CandidateStatus::Running || result.durationSeconds > 0 || result.retryCount)
            ++s.startedCandidates;
        if (result.status == CandidateStatus::Deferred) ++s.deferredCandidates;
        else if (result.status != CandidateStatus::Queued && result.status != CandidateStatus::Running)
            ++s.finishedCandidates;
    }
    for (size_t i = 0; i < m_results.size(); ++i)
        if (m_results[i].status == CandidateStatus::Running) {
            s.activeTaskNames.push_back(m_tasks[i].name);
            if (!m_tasks[i].groupedSearch)
                s.activeCandidates.push_back({"", m_tasks[i].configId, m_tasks[i].name, m_results[i].stage});
        }
    for (const auto &[group, engine] : m_groupEngines) {
        const auto child = engine->getSnapshot();
        for (const auto &candidate : child.activeCandidates)
            s.activeCandidates.push_back({group, candidate.configId, candidate.name, candidate.stage});
        s.localGroups.push_back({group, child.leaderName,
            !child.searchStage.empty() ? child.searchStage : child.activeCandidates.empty()
                ? engineStateName(child.state) : child.activeCandidates.front().stage,
            child.leaderBytes, child.leaderContentVerified, child.startedCandidates, child.finishedCandidates,
            child.deferredCandidates, static_cast<uint32_t>(child.total)});
        if (!s.memoryOverrideEligible && child.memoryOverrideEligible) {
            s.memoryOverrideEligible = true;
            s.memoryOverrideOffer = child.memoryOverrideOffer;
            s.memoryOverrideOffer.group = group;
        }
        s.waitingCandidatesTotal += child.waitingCandidatesTotal;
        for (auto candidate : child.waitingCandidates) {
            if (s.waitingCandidates.size() >= 64) break;
            candidate.group = group;
            s.waitingCandidates.push_back(std::move(candidate));
        }
    }
    const auto resources = m_broker ? m_broker->snapshot() : AdmissionBroker::Snapshot{
        m_scheduler.ramLimit, m_scheduler.ramReserved, m_scheduler.cpuLimit,
        m_scheduler.cpuReserved, static_cast<uint32_t>(m_active), m_run.settings.workerThreads};
    if (!s.memoryOverrideEligible && m_broker && s.state == EngineState::TestingCandidates &&
        !m_pause && !m_cancel && !m_finish && m_searchStage != "Probe" && !resources.admissionsBlocked && !resources.draining &&
        resources.active < resources.executorLimit && resources.diskActive < resources.diskLimit && resources.cpuReserved <= resources.cpuLimit &&
        (!m_run.settings.fullAttemptLimit || m_fullAttemptsStarted < m_run.settings.fullAttemptLimit)) {
        for (size_t i = 0; i < m_tasks.size() && i < m_results.size(); ++i) {
            if (m_results[i].status != CandidateStatus::Deferred || m_tasks[i].groupedSearch) continue;
            const auto &task = m_tasks[i];
            if (task.estimatedRamMB > UINT64_MAX - resources.ramReserved ||
                task.cpuTokens > resources.cpuLimit - resources.cpuReserved) continue;
            const auto target = resources.ramReserved + task.estimatedRamMB;
            const auto base = m_broker->baselineRam();
            if (target <= base || target - base > 1024 || target > UINT64_MAX - m_broker->persistentMB()) continue;
            s.memoryOverrideEligible = true;
            s.memoryOverrideOffer = {"", task.configId, task.name, target - base, task.estimatedRamMB};
            break;
        }
    }
    for (size_t i = 0; i < m_results.size() && i < m_tasks.size(); ++i) {
        const auto &result = m_results[i];
        if (result.status != CandidateStatus::Queued && result.status != CandidateStatus::Deferred) continue;
        ++s.waitingCandidatesTotal;
        if (s.waitingCandidates.size() >= 64) continue;
        const auto &task = m_tasks[i];
        const auto ram = task.groupedSearch ? uint64_t{0} : task.estimatedRamMB;
        const auto cpu = task.groupedSearch ? 0u : result.retryCount ? resources.cpuLimit : task.cpuTokens;
        std::string reason;
        if (m_pause) reason = "Paused by user";
        else if (m_finish || m_cancel) reason = "Search is stopping";
        else if (resources.admissionsBlocked || resources.draining) reason = resources.blockReason;
        else if (!task.reuseInputArchive && !task.groupedSearch && m_run.settings.fullAttemptLimit &&
                 m_fullAttemptsStarted >= m_run.settings.fullAttemptLimit) reason = "Explicit full-attempt budget reached";
        else if (ram > resources.ramLimit) reason = "Required RAM exceeds current admission limit";
        else if (cpu > resources.cpuLimit) reason = "Required CPU tokens exceed available CPU budget";
        else if (resources.ramReserved > resources.ramLimit || ram > resources.ramLimit - resources.ramReserved)
            reason = "Waiting for RAM reservations to be released";
        else if (resources.cpuReserved > resources.cpuLimit || cpu > resources.cpuLimit - resources.cpuReserved)
            reason = "Waiting for CPU tokens to be released";
        else if (!task.groupedSearch && resources.active >= resources.executorLimit) reason = "Concurrent executor limit";
        else if (!task.groupedSearch && !task.ramCandidate && resources.diskActive >= resources.diskLimit) reason = "Concurrent disk task limit";
        else if (result.status == CandidateStatus::Deferred)
            reason = result.statusMessage.empty() ? "Deferred; explicit admission required" : result.statusMessage;
        else reason = task.groupedSearch ? "Waiting for group orchestration" : "Waiting for executor or fair admission";
        s.waitingCandidates.push_back({"", task.configId, task.name, redactFlags(task.flags),
                                       std::move(reason), ram, cpu, result.status == CandidateStatus::Deferred});
    }
    for (const auto &report : m_groupReports) {
        if (s.localGroups.size() >= 64) break;
        const auto group = report.value("groupKey", std::string{});
        if (group.empty() || std::any_of(s.localGroups.begin(), s.localGroups.end(), [&](const auto &entry) {
            return entry.group == group;
        })) continue;
        EngineSnapshot::LocalGroup entry;
        entry.group = group;
        entry.stage = report.value("stateName", std::string{"Saved"});
        if (report.contains("leader") && report["leader"].is_object()) {
            const auto &leader = report["leader"];
            entry.leaderName = leader.value("name", std::string{});
            entry.leaderBytes = leader.value("outputBytes", uint64_t{0});
            entry.contentVerified = leader.value("contentVerified", false);
        }
        if (report.contains("results") && report["results"].is_array()) {
            entry.total = static_cast<uint32_t>(report["results"].size());
            for (const auto &result : report["results"]) {
                const auto status = static_cast<CandidateStatus>(result.value("status", 0));
                if (status == CandidateStatus::Running || result.value("duration", 0.0) > 0)
                    ++entry.started;
                if (status == CandidateStatus::Deferred) ++entry.deferred;
                else if (status != CandidateStatus::Queued && status != CandidateStatus::Running) ++entry.finished;
            }
        }
        s.localGroups.push_back(std::move(entry));
    }
    for (const auto &[index, key] : m_activeGroupKeys)
        s.activeTaskNames.push_back("Group: " + key);
    if (s.state != EngineState::TestingCandidates || m_pause || m_cancel || m_finish || m_earlyAssembly)
        s.memoryOverrideEligible = false;
    s.error = m_error;
    return s;
}
bool OptimizerEngine::selectAndAssemble(const std::string &id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((m_state != EngineState::TestingCandidates && m_state != EngineState::Paused) || m_finish || m_earlyAssembly || m_cancel)
        return false;
    auto found = std::find_if(m_results.begin(), m_results.end(), [&](const auto &r) {
        return r.configId == id && r.verified && !r.tempArchivePath.empty();
    });
    if (found == m_results.end())
        return false;
    m_best = *found;
    m_manualWinner = id;
    m_stopReason = "User selected retained verified candidate";
    m_searchInterrupted = true;
    m_stopAction = "SelectAndAssemble";
    m_overrideLeases.clear();
    if (m_broker) for (const auto &task : m_tasks) m_broker->withdraw(m_run.sessionId + ":" + task.configId);
    m_finish = true; markSearchClosed();
    m_pause = false;
    for (auto &token : m_taskCancel)
        *token = true;
    m_cv.notify_all();
    return true;
}
bool OptimizerEngine::retest(const std::string &id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((m_state != EngineState::TestingCandidates && m_state != EngineState::Paused) || m_finish || m_earlyAssembly || m_cancel ||
        m_active == 0)
        return false;
    for (size_t i = 0; i < m_results.size(); ++i) {
        auto &r = m_results[i];
        if (r.configId == id &&
            (r.status == CandidateStatus::Failed || r.status == CandidateStatus::Skipped ||
             r.status == CandidateStatus::OOM_Terminated)) {
            r.status = CandidateStatus::Queued;
            r.statusMessage.clear();
            *m_taskCancel[i] = false;
            --m_completed;
            m_cv.notify_all();
            return true;
        }
    }
    return false;
}
std::vector<SessionInfo> OptimizerEngine::sessions(const fs::path &base) const {
    SessionManager manager;
    manager.setSessionDir(base / "sessions");
    return manager.getSessionHistory();
}
bool OptimizerEngine::discardSession(const SessionInfo &session) {
    if (busy() || !WorkspaceManager::owned(session.workspace, session.workspace.parent_path()))
        return false;
    std::error_code ec;
    fs::remove_all(session.workspace, ec);
    if (ec)
        return false;
    SessionManager manager;
    manager.setSessionDir(session.workspace.parent_path() / "sessions");
    manager.clearSession(session.sessionId);
    return true;
}
bool OptimizerEngine::exportSessionLeader(const SessionInfo &session, const fs::path &target,
                                          const std::string &password,
                                          const std::atomic<bool> *cancel) {
    if (busy() || !WorkspaceManager::owned(session.workspace, session.workspace.parent_path()) ||
        isWithin(target, session.workspace) || isWithin(target, session.inputPath))
        return false;
    auto found = std::find_if(session.results.begin(), session.results.end(), [&](const auto &r) {
        return r.taskId == session.leaderTaskId && r.verified;
    });
    if (found == session.results.end() || !isWithin(found->tempArchivePath, session.workspace))
        return false;
    try {
        std::string error;
        ResourceGuard guard;
        guard.initialize(session.settings.ramLimitMB);
        if (!ArchiveAssembler::verifyArchive(found->tempArchivePath, session.settings.toolsBinDir,
                                             error, password, cancel, &guard,
                                             session.settings.processTimeoutSeconds))
            return false;
        PublicationTransaction publication(target);
        auto staged = publication.stage() / "new" / target.filename();
        fs::copy_file(found->tempArchivePath, staged);
        publication.verified({staged});
        try {
            publication.publish(cancel);
        } catch (...) {
            publication.rollback();
            throw;
        }
        return true;
    } catch (...) {
        return false;
    }
}
bool OptimizerEngine::resumeSession(const SessionInfo &session, const std::string &password) {
    if (busy() || !WorkspaceManager::owned(session.workspace, session.workspace.parent_path()))
        return false;
    auto settings = session.settings;
    settings.password = password;
    settings.customTempDir = session.workspace.parent_path();
    try {
        setSettings(settings);
        m_requestedSession = session.sessionId;
        return start(session.inputPath, session.outputPath);
    } catch (...) {
        return false;
    }
}
Json OptimizerEngine::getRunReport() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (busy())
        return {{"schema", 1},
                {"state", static_cast<int>(m_state.load())},
                {"message", "Report available after run completion"}};
    Json results = Json::array(), outputs = Json::array(), changed = Json::array(),
         phases = Json::object();
    for (const auto &candidate : m_results)
        results.push_back(resultJson(candidate));
    uint64_t candidateFileBytes = 0, ramCandidates = 0, ramLosers = 0;
    for (const auto &candidate : m_results) {
        candidateFileBytes += candidate.temporaryFileBytes;
        if (candidate.storage == "ram") {
            ++ramCandidates;
            if (candidate.status == CandidateStatus::Verified && !candidate.temporaryFileBytes) ++ramLosers;
        }
    }
    for (const auto &path : m_outputs) {
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        outputs.push_back({{"path", pathUtf8(path)}, {"bytes", ec ? 0 : size}});
    }
    for (const auto &path : m_changedFiles)
        changed.push_back(pathUtf8(path));
    for (const auto &[phase, seconds] : m_phaseSeconds)
        phases[std::to_string(phase)] = seconds;
    const auto accounting = activeGuard().getAccounting();
    const auto admission = m_broker ? m_broker->snapshot() : AdmissionBroker::Snapshot{};
    uint64_t retries = 0, peakCandidateCapacity = 0;
    double candidateWait = 0;
    for (const auto &candidate : m_results) {
        retries += candidate.retryCount;
        candidateWait += candidate.waitSeconds;
        peakCandidateCapacity = std::max(peakCandidateCapacity, candidate.ramBufferBytes);
    }
    return {{"schema", 1},
            {"resources", {{"admission", {{"ramLimitMB", admission.ramLimit},
                {"persistentMB", m_broker ? m_broker->persistentMB() : 0},
                {"executorLimit", admission.executorLimit}, {"diskLimit", admission.diskLimit},
                {"peakPlannedTotalMB", admission.peakTotalRam}, {"peakExecutors", admission.peakActive},
                {"peakDiskTasks", admission.peakDisk}, {"remainingWaiters", admission.waiting},
                {"scope", "Shared broker maxima; never sum child reports. Planned RAM includes persistent input; commit and RAM capacity remain separate measurements."}}},
                {"jobAccountingAvailable", accounting.available},
                {"processes", accounting.processes}, {"logicalReadBytes", accounting.readBytes},
                {"logicalWriteBytes", accounting.writeBytes}, {"peakCommittedMB", activeGuard().getPeakMemoryMB()},
                {"maxCandidateRamCapacityBytes", peakCandidateCapacity},
                {"sharedRamBudgetPeakBytes", m_ramBudget ? m_ramBudget->peak() : 0},
                {"applicationLogicalIO", m_applicationIO},
                {"applicationIOScope", "Application process transfers between start and terminal state, including all concurrent engines, checkpoint/hash/copy and diagnostics; excludes child jobs. Shared RAM budget peak repeats in child reports. Never sum child reports."},
                {"retries", retries},
                {"candidateQueueWaitSeconds", candidateWait},
                {"scope", "Job cumulative transfers/processes include native workers and failures; shared job totals repeat in child reports, never sum them. Excludes parent file operations and separate input decoder (inputStorage.decoder). Logical transfers are not physical disk I/O. Candidate capacity is largest individual buffer, not simultaneous RAM peak. Queue wait includes pause and resource contention."}}},
            {"productVersion", "2.2.0-dev"},
            {"stateName", engineStateName(m_state)},
            {"state", static_cast<int>(m_state.load())},
            {"settings", settingsJson(m_run.settings)},
            {"requestedSettings", settingsJson(m_settings)},
            {"inputFingerprint", m_run.inputFingerprint},
            {"effectiveFingerprint", m_run.effectiveFingerprint},
            {"files", m_summary.totalFiles},
            {"inputBytes", m_summary.totalBytes},
            {"inputStorage", {{"ramBytes", m_inputRamBytes}, {"diskBytes", m_inputDiskBytes},
                {"reservedMB", m_inputRamReservedMB},
                {"decoder", m_inputDecodeMetrics},
                {"scope", "Hybrid initial archive extraction only; later fallback/assembly materialization is separate"}}},
            {"compatibleInputArchiveBytes", m_compatibleInputArchiveBytes},
            {"archiveOutcome", m_state != EngineState::Completed ? "Incomplete" :
                !m_compatibleInputArchiveBytes ? "Created/transformed archive" :
                m_best.outputBytes < m_compatibleInputArchiveBytes ? "Reduced archive size" :
                "No archive size reduction; preserved input compression"},
            {"generatedCandidates", m_generatedCount},
            {"scheduledCandidates", m_tasks.size()},
            {"searchInterrupted", m_searchInterrupted},
            {"searchCompletion", m_searchInterrupted ? "Interrupted" :
                m_state == EngineState::Completed ? "ScheduledCandidatesCompleted" : "Incomplete"},
            {"publicationOutcome", m_publicationOutcome.empty() ? "NotPublished" : m_publicationOutcome},
            {"stopAction", m_stopAction},
            {"groupsCompleted", m_groupsCompleted}, {"groupsTotal", m_groupsTotal},
            {"sessionRetained", m_retainedGroups || m_searchInterrupted || m_state == EngineState::Cancelled},
            {"elapsedSeconds", m_finishedElapsed >= 0 ? m_finishedElapsed : std::chrono::duration<double>(std::chrono::steady_clock::now() - m_run.start).count()},
            {"finalization", {
                {"startedAtSeconds", m_finalizationStartedElapsed < 0 ? Json(nullptr) : Json(m_finalizationStartedElapsed)},
                {"finishedAtSeconds", m_finalizationStartedElapsed < 0 || m_finishedElapsed < 0 ? Json(nullptr) : Json(m_finishedElapsed)},
                {"seconds", m_finalizationStartedElapsed < 0 ? Json(nullptr) :
                    Json(std::max(0.0, (m_finishedElapsed >= 0 ? m_finishedElapsed :
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_run.start).count()) -
                        m_finalizationStartedElapsed))},
                {"scope", "Terminal assembly, verification, publication and final checkpoint; group assembly during search is reported separately in groupAssembly. Null means not reached."}}},
            {"stopReason", m_stopReason.empty() ?
                (m_state == EngineState::Completed ? "Scheduled search complete" : engineStateName(m_state)) : m_stopReason},
            {"phaseSeconds", phases},
            {"operationTiming", m_metrics.report()},
            {"searchClosedAtSeconds", m_searchClosedElapsed < 0 ? Json(nullptr) : Json(m_searchClosedElapsed)},
            {"completionContract", "Search budget stops new attempts. Active work, checkpoint, content verification and transactional publication may continue. Process timeout applies per process; no unverified publication on expiry."},
            {"groupAssembly", {{"elapsedSeconds", m_groupAssemblySeconds},
                {"predictedReencodeSeconds", m_groupAssemblyPredictedSeconds},
                {"predictionBasis", "Previously measured winning compression time; excludes new admission wait; zero means unavailable"},
                {"blockReuse", "First group copied; subsequent groups re-encoded"}, {"groups", m_assemblyEvidence}}},
            {"scratchPeakObservedBytes", m_peakScratchBytes},
            {"guardedChildCpuSeconds", (m_sharedGuard ? m_sharedGuard->getCpuSeconds() : m_guard.getCpuSeconds()) + m_inputDecoderCpuSeconds},
            {"guardedChildCpuScope", m_sharedGuard ? "Parent shared job, cumulative; not per-group" : "Main job plus separate hybrid decoder job; excludes application CPU"},
            {"tools", m_toolIdentities},
            {"changedResources", changed},
            {"leader", resultJson(m_best)},
            {"results", results},
            {"temporaryIO", {{"candidateFileBytes", candidateFileBytes},
                {"ramCandidates", ramCandidates}, {"ramDiscardedWithoutFile", ramLosers},
                {"ramProbes", m_ramProbes.load()}, {"probeFileBytes", m_probeFileWriteBytes.load()},
                {"probeInputFileBytes", m_probeInputWriteBytes.load()},
                {"ramInputMaterializedBytes", m_materializedInputBytes.load()},
                {"scope", "Logical candidate/probe artifact contents only; excludes input extraction, metadata, checkpoint, IPC, final output, OS cache and paging. Child group reports have separate counters; not physical SSD writes."}}},
            {"groups", m_groupReports},
            {"probes", m_probeEvidence},
            {"groupCounterScope", "Shared child job peak committed memory; sum of candidate and append process transfer counters, excludes probes and final verification"},
            {"outputs", outputs},
            {"log", pathUtf8(m_logPath)},
            {"error",
             {{"message", m_error},
              {"phase", static_cast<int>(m_structuredError.phase)},
              {"category", static_cast<int>(m_structuredError.category)},
              {"tool", m_structuredError.tool},
              {"exitCode", m_structuredError.exitCode},
              {"win32Error", m_structuredError.win32Error}}}};
}
bool OptimizerEngine::saveRunReport(const fs::path &path) const {
    if (busy())
        return false;
    return writeAtomically(path, getRunReport().dump(2));
}
fs::path OptimizerEngine::getLogPath() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_logPath;
}
float OptimizerEngine::getProgress() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_progress;
}
double OptimizerEngine::getEstimatedTimeRemaining() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_eta;
}
int OptimizerEngine::getCompletedCount() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_completed;
}
int OptimizerEngine::getTotalCount() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return static_cast<int>(m_results.size());
}
std::string OptimizerEngine::getCurrentTaskName() const {
    std::lock_guard<std::mutex> l(m_mutex);
    for (const auto &r : m_results)
        if (r.status == CandidateStatus::Running)
            return r.name;
    return {};
}
std::string OptimizerEngine::getLastError() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_error;
}
std::vector<fs::path> OptimizerEngine::getOutputFiles() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_outputs;
}
void OptimizerEngine::setLogCallback(std::function<void(const std::string &)> cb) {
    std::lock_guard<std::mutex> l(m_callbackMutex);
    m_logCallback = std::move(cb);
}
void OptimizerEngine::setCandidateCallback(std::function<void(const CandidateResult &)> cb) {
    std::lock_guard<std::mutex> l(m_callbackMutex);
    m_candidateCallback = std::move(cb);
}
void OptimizerEngine::setStateCallback(std::function<void(EngineState)> cb) {
    std::lock_guard<std::mutex> l(m_callbackMutex);
    m_stateCallback = std::move(cb);
}
void OptimizerEngine::setProgressCallback(std::function<void(float, int, int)> cb) {
    std::lock_guard<std::mutex> l(m_callbackMutex);
    m_progressCallback = std::move(cb);
}
void OptimizerEngine::log(const std::string &raw) {
    auto msg = Diagnostics::redact(raw, m_run.settings.password);
    {
        std::lock_guard<std::mutex> lock(m_logFileMutex);
        if (!m_logPath.empty()) {
            rotateDiagnostic(m_logPath);
            std::ofstream out(m_logPath, std::ios::app | std::ios::binary);
            auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &time);
#else
            gmtime_r(&time, &utc);
#endif
            out << std::put_time(&utc, "%FT%TZ") << " +" <<
                std::chrono::duration<double>(std::chrono::steady_clock::now() - m_run.start).count() << "s " << msg << '\n';
        }
    }
    std::function<void(const std::string &)> cb;
    {
        std::lock_guard<std::mutex> l(m_callbackMutex);
        cb = m_logCallback;
    }
    if (cb)
        try {
            cb(msg);
        } catch (...) {
        }
}
void OptimizerEngine::event(const std::string &stage, const std::string &configId, uint32_t pid,
                            const std::string &action, const Json &details) {
    const auto now = std::chrono::system_clock::now();
    Json record = {{"utcUnixMilliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()},
        {"elapsedSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - m_run.start).count()},
        {"sessionId", m_run.sessionId}, {"configId", configId}, {"pid", pid}, {"stage", stage}, {"action", action}, {"details", details}};
    if (m_broker) {
        auto resources = m_broker->snapshot();
        record["reservedRamMB"] = resources.ramReserved;
        record["reservedCpu"] = resources.cpuReserved;
        record["activeExecutors"] = resources.active;
        record["activeDiskTasks"] = resources.diskActive;
        record["executorLimit"] = resources.executorLimit;
        record["diskLimit"] = resources.diskLimit;
    }
    std::lock_guard lock(m_logFileMutex);
    if (m_logPath.empty()) return;
    auto path = m_logPath; path.replace_extension("events.jsonl");
    rotateDiagnostic(path);
    std::ofstream out(path, std::ios::app | std::ios::binary);
    out << record.dump() << '\n';
}
void OptimizerEngine::markSearchClosed() {
    if (m_searchClosedElapsed < 0)
        m_searchClosedElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_run.start).count();
}
void OptimizerEngine::changeState(EngineState s) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::steady_clock::now();
        if (m_phaseStart.time_since_epoch().count()) {
            const auto seconds = std::chrono::duration<double>(now - m_phaseStart).count();
            m_phaseSeconds[static_cast<int>(m_state.load())] += seconds;
            if (m_state == EngineState::Analyzing || m_state == EngineState::Extracting ||
                m_state == EngineState::Preprocessing)
                m_metrics.add(engineStateName(m_state), seconds);
        }
        m_phaseStart = now;
        m_state = s;
        if (s == EngineState::AssemblingFinal) markSearchClosed();
        if (s == EngineState::AssemblingFinal && m_finalizationStartedElapsed < 0)
            m_finalizationStartedElapsed = std::chrono::duration<double>(now - m_run.start).count();
        if (s == EngineState::Completed || s == EngineState::Failed || s == EngineState::Cancelled) {
            m_finishedElapsed = std::chrono::duration<double>(now - m_run.start).count();
            auto currentIO = applicationIO();
            if (!currentIO.is_null() && !m_applicationIOStart.is_null()) {
                for (const auto *key : {"readBytes", "writeBytes"})
                    currentIO[key] = currentIO[key].get<uint64_t>() - m_applicationIOStart[key].get<uint64_t>();
                m_applicationIO = std::move(currentIO);
            }
        }
        if (s != EngineState::Paused)
            m_pause = false;
    }
    event(engineStateName(s), "", 0, "Phase");
    notifyState(s);
}
void OptimizerEngine::notifyState(EngineState s) {
    std::function<void(EngineState)> cb;
    {
        std::lock_guard<std::mutex> l(m_callbackMutex);
        cb = m_stateCallback;
    }
    if (cb)
        try {
            cb(s);
        } catch (...) {
        }
}
void OptimizerEngine::pause() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != EngineState::TestingCandidates || m_finish || m_earlyAssembly || m_cancel)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (m_phaseStart.time_since_epoch().count())
            m_phaseSeconds[static_cast<int>(m_state.load())] += std::chrono::duration<double>(now - m_phaseStart).count();
        m_phaseStart = now;
        m_pause = true;
        m_state = EngineState::Paused;
    }
    event("Paused", "", 0, "Phase");
    notifyState(EngineState::Paused);
    log("[Engine] Queue paused; running candidates will finish.");
}
void OptimizerEngine::resume() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != EngineState::Paused || m_finish || m_earlyAssembly || m_cancel)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (m_phaseStart.time_since_epoch().count())
            m_phaseSeconds[static_cast<int>(m_state.load())] += std::chrono::duration<double>(now - m_phaseStart).count();
        m_phaseStart = now;
        m_pause = false;
        m_state = EngineState::TestingCandidates;
    }
    event("Testing", "", 0, "Resume");
    notifyState(EngineState::TestingCandidates);
    m_cv.notify_all();
}
void OptimizerEngine::skipCurrent() {
    auto snapshot = getSnapshot();
    if (!snapshot.activeCandidates.empty())
        skipCandidate(snapshot.activeCandidates.front().group, snapshot.activeCandidates.front().configId);
}
bool OptimizerEngine::skipCandidate(const std::string &group, const std::string &id) {
    std::lock_guard<std::mutex> l(m_mutex);
    if (!busy() || m_cancel || m_finish || m_earlyAssembly) return false;
    if (!group.empty()) {
        auto found = m_groupEngines.find(group);
        if (found == m_groupEngines.end() || !found->second->skipCandidate("", id)) return false;
        m_searchInterrupted = true;
        m_stopAction = "SkipCandidate";
        m_stopReason = "User skipped candidate in group " + group + ": " + id;
        return true;
    }
    for (size_t i = 0; i < m_results.size(); ++i)
        if (m_results[i].status == CandidateStatus::Running && !m_tasks[i].groupedSearch &&
            m_tasks[i].configId == id) {
            *m_taskCancel[i] = true;
            m_searchInterrupted = true;
            m_stopAction = "SkipCandidate";
            m_stopReason = "User skipped candidate " + id;
            m_cv.notify_all();
            return true;
        }
    return false;
}
bool OptimizerEngine::skipGroup(const std::string &group) {
    std::lock_guard lock(m_mutex);
    if (!busy() || m_cancel || m_finish || m_earlyAssembly) return false;
    auto found = m_groupEngines.find(group);
    if (found == m_groupEngines.end()) return false;
    found->second->cancel();
    m_searchInterrupted = true;
    m_stopAction = "SkipGroup";
    m_stopReason = "User skipped group " + group;
    return true;
}
bool OptimizerEngine::startNextCandidate() {
    std::lock_guard lock(m_mutex);
    if (m_state != EngineState::TestingCandidates || m_pause || m_cancel || m_finish || m_earlyAssembly || m_startNextPending || m_searchStage == "Probe" ||
        (m_run.settings.fullAttemptLimit && m_fullAttemptsStarted >= m_run.settings.fullAttemptLimit))
        return false;
    for (const auto &[key, engine] : m_groupEngines)
        if (engine->startNextCandidate()) return true;
    auto resources = m_broker ? m_broker->snapshot() : AdmissionBroker::Snapshot{};
    if (resources.admissionsBlocked || resources.draining) return false;
    for (size_t i = 0; i < m_tasks.size(); ++i)
        if (m_results[i].status == CandidateStatus::Queued && !m_tasks[i].groupedSearch &&
            m_scheduler.fits(m_tasks[i].estimatedRamMB, m_tasks[i].cpuTokens) &&
            resources.ramReserved <= resources.ramLimit && resources.cpuReserved <= resources.cpuLimit &&
            m_tasks[i].estimatedRamMB <= resources.ramLimit - resources.ramReserved &&
            m_tasks[i].cpuTokens <= resources.cpuLimit - resources.cpuReserved && resources.active < resources.executorLimit &&
            (m_tasks[i].ramCandidate || resources.diskActive < resources.diskLimit)) {
            m_startNextPending = true;
            m_cv.notify_all();
            return true;
        }
    return false;
}
bool OptimizerEngine::requestMemoryOverride() {
    const auto preview = getSnapshot();
    if (!preview.memoryOverrideEligible) return false;
    return requestMemoryOverride(preview.memoryOverrideOffer.group, preview.memoryOverrideOffer.configId,
                                 preview.memoryOverrideOffer.extraMiB);
}
bool OptimizerEngine::requestMemoryOverride(const std::string &group, const std::string &configId,
                                            uint64_t maxExtraMiB) {
    std::lock_guard lock(m_mutex);
    if (m_state != EngineState::TestingCandidates || m_pause || m_cancel || m_finish || m_earlyAssembly || !m_broker ||
        !maxExtraMiB || maxExtraMiB > 1024 || m_searchStage == "Probe")
        return false;
    if (!group.empty()) {
        const auto found = m_groupEngines.find(group);
        return found != m_groupEngines.end() && found->second->requestMemoryOverride("", configId, maxExtraMiB);
    }
    if (m_run.settings.fullAttemptLimit && m_fullAttemptsStarted >= m_run.settings.fullAttemptLimit) return false;
    const auto current = m_broker->snapshot();
    for (size_t i = 0; i < m_tasks.size(); ++i) {
        if (m_results[i].status != CandidateStatus::Deferred || m_tasks[i].configId != configId) continue;
        auto required = m_tasks[i].estimatedRamMB;
        if (required > UINT64_MAX - current.ramReserved) continue;
        const auto target = current.ramReserved + required;
        const auto base = m_broker->baselineRam();
        if (target <= base || target - base > maxExtraMiB || target > UINT64_MAX - m_broker->persistentMB() ||
            !ResourceGuard::hasMemoryHeadroom(required)) continue;
        const auto key = m_run.sessionId + ":" + m_tasks[i].configId;
        auto lease = m_broker->raiseAndAcquire(key, required, m_tasks[i].cpuTokens, target, [&] {
            return ResourceGuard::hasMemoryHeadroom(required) &&
                activeGuard().updateMemoryLimits(target + m_broker->persistentMB());
        });
        if (!lease) return false;
        m_overrideLeases[key] = std::move(lease);
        m_scheduler.ramLimit = target;
        m_results[i].status = CandidateStatus::Queued;
        m_results[i].statusMessage = "Explicit bounded temporary RAM override";
        m_startNextPending = true;
        m_cv.notify_all();
        return true;
    }
    return false;
}
void OptimizerEngine::finishAndAssembleBest() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if ((m_state != EngineState::TestingCandidates && m_state != EngineState::Paused) ||
            m_best.taskId <= 0 || m_finish || m_earlyAssembly || m_cancel)
            return;
        m_stopReason = "User requested assembly of current leader";
        m_searchInterrupted = true;
        m_stopAction = "AssembleBest";
        m_overrideLeases.clear();
        const bool groupedPending = std::any_of(m_tasks.begin(), m_tasks.end(), [&](const auto &task) {
            auto index = static_cast<size_t>(&task - m_tasks.data());
            return task.groupedSearch && (m_results[index].status == CandidateStatus::Running ||
                                         m_results[index].status == CandidateStatus::Queued);
        });
        if (groupedPending) {
            m_earlyAssembly = true; markSearchClosed();
            m_pause = false;
            for (size_t i = 0; i < m_results.size(); ++i) {
                if (m_tasks[i].groupedSearch) continue;
                if (m_results[i].status == CandidateStatus::Running) *m_taskCancel[i] = true;
                if (m_results[i].status == CandidateStatus::Queued) {
                    m_results[i].status = CandidateStatus::Skipped;
                    m_results[i].statusMessage = "Early complete grouped assembly requested";
                    ++m_completed;
                }
                if (m_broker) m_broker->withdraw(m_run.sessionId + ":" + m_tasks[i].configId);
            }
            m_cv.notify_all();
            return;
        }
        m_finish = true; markSearchClosed();
        m_pause = false;
        for (auto &t : m_taskCancel)
            if (t)
                *t = true;
        if (m_broker)
            for (const auto &task : m_tasks) m_broker->withdraw(m_run.sessionId + ":" + task.configId);
    }
    m_cv.notify_all();
}
void OptimizerEngine::cancel() {
    {
        std::lock_guard lock(m_mutex);
        if (!busy() || m_cancel) return;
        m_cancel = true; markSearchClosed();
        m_searchInterrupted = true;
        m_stopAction = "Cancel";
        m_overrideLeases.clear();
        m_stopReason = "User cancelled search; durable results retained";
        if (m_broker)
            for (const auto &task : m_tasks) m_broker->withdraw(m_run.sessionId + ":" + task.configId);
    }
    m_pause = false;
    m_cv.notify_all();
}
void OptimizerEngine::cleanupWorkspace() {
    if (!m_run.workspace.empty()) {
        std::error_code ec;
        fs::remove_all(m_run.workspace, ec);
    }
}
bool OptimizerEngine::start(const fs::path &input, const fs::path &output) {
    if (busy())
        return false;
    wait();
    try {
        auto settings = getSettings();
        validateSettings(settings);
        EmbeddedTools::verifyToolManifest(settings.toolsBinDir);
        auto in = fs::weakly_canonical(fs::absolute(input));
        auto out = fs::weakly_canonical(fs::absolute(output));
        if (!fs::exists(in) || isLinkOrReparse(input))
            throw std::invalid_argument("Input is missing or is a symbolic link");
        if (isWithin(out, in))
            throw std::invalid_argument("Output must be outside the input tree");
        auto temporaryBase = settings.customTempDir.empty() ? in.parent_path() / ".ultraarch_tmp"
                                                            : settings.customTempDir;
        if (fs::is_directory(in) && isWithin(temporaryBase, in))
            throw std::invalid_argument("Temporary directory must be outside the input tree");
        auto exe = ProcessManager::resolveExecutable("7z", settings.toolsBinDir);
        if (exe.empty())
            throw std::runtime_error("7-Zip executable was not found");
        settings.toolsBinDir = exe.parent_path();
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_run = {};
            m_run.input = in;
            m_run.output = out;
            m_run.settings = settings;
            if (settings.autoCpu) {
                m_run.settings.cpuThreads = ResourceGuard::availableCpuThreads(static_cast<int>(settings.affinityMode));
                m_run.settings.workerThreads = std::clamp(m_run.settings.cpuThreads / 2, 1u, 64u);
            }
            m_run.start = std::chrono::steady_clock::now();
            m_finishedElapsed = -1;
            m_finalizationStartedElapsed = -1;
            m_fullAttemptsStarted = 0;
            m_results.clear();
            m_groupsCompleted = m_groupsTotal = m_groupsActive = 0;
            m_tasks.clear();
            m_taskCancel.clear();
            m_best = {};
            m_hasDurableCheckpoint = false;
            m_summary = {};
            m_outputs.clear();
            m_error.clear();
            m_reservedMemory = 0;
            m_analyzedFiles = m_analyzedBytes = 0;
            m_scheduler = {};
            m_admissionLimit = SIZE_MAX;
            m_peakScratchBytes = 0;
            m_compatibleInputArchiveBytes = 0;
            m_stopReason.clear();
            m_stopAction.clear();
            m_searchStage.clear();
            m_publicationOutcome.clear();
            m_retainedGroups = m_searchInterrupted = false;
            m_startNextPending = false;
            m_earlyAssembly = false;
            m_diagnosticBytes = 0;
            m_groupAssemblySeconds = m_groupAssemblyPredictedSeconds = 0;
            m_assemblyEvidence = Json::array();
            m_overrideLeases.clear();
            if (!m_sharedGuard) m_broker.reset();
            m_smallestBytes = UINT64_MAX;
            m_manualWinner.clear();
            m_structuredError = {};
            m_metrics.clear();
            m_applicationIOStart = applicationIO();
            m_applicationIO = nullptr;
            m_searchClosedElapsed = -1;
            m_phaseSeconds.clear();
            m_changedFiles.clear();
            m_toolIdentities = Json::array();
            m_groupReports = Json::array();
            m_probeEvidence = Json::array();
            m_phaseStart = std::chrono::steady_clock::now();
            m_logPath = Diagnostics::logDirectory() / ("run-" + uniqueId() + ".log");
            m_completed = 0;
            m_active = 0;
            m_durationSum = 0;
            m_eta = 0;
            m_costModel.reset();
            m_predictedSeconds.clear();
            m_taskStarts.clear();
            m_activeGroupKeys.clear();
            m_lastCandidateFinished = std::chrono::steady_clock::now();
            m_progress = 0;
        }
        m_cancel = false;
        m_pause = false;
        m_finish = false;
        changeState(EngineState::Analyzing);
        m_coordinator = std::thread(&OptimizerEngine::coordinatorLoop, this);
        return true;
    } catch (const std::exception &e) {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_error = Diagnostics::redact(e.what(), m_run.settings.password);
            m_structuredError.phase = m_state;
            m_structuredError.category = Diagnostics::classify(e);
            m_structuredError.message = m_error;
            if (m_best.taskId <= 0 && !m_results.empty()) {
                const auto failed =
                    std::find_if(m_results.rbegin(), m_results.rend(), [](const auto &result) {
                        return result.status == CandidateStatus::Failed ||
                               result.status == CandidateStatus::OOM_Terminated;
                    });
                if (failed != m_results.rend()) {
                    m_structuredError.category = failed->errorCode;
                    m_structuredError.exitCode = failed->toolExitCode;
                    m_structuredError.win32Error = failed->win32Error;
                    m_structuredError.tool = pathUtf8(
                        ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir));
                }
            }
        }
        log(std::string("[ERROR] ") + e.what());
        changeState(EngineState::Failed);
        return false;
    }
}
std::string OptimizerEngine::fingerprintInput(const fs::path &input) const {
    return manifestForInput(input).fingerprint;
}
InputManifest OptimizerEngine::manifestForInput(const fs::path &input) const {
    auto manifest = InputManifest::scan(input, &m_cancel, {}, m_run.settings.enableSimilarityGroups);
    if (!fs::is_directory(input) || m_memoryFiles.empty()) return manifest;
    std::map<std::string, FileTypeInfo> files;
    for (const auto &file : manifest.summary.files)
        files.emplace(archivePathKey(pathUtf8(file.path.lexically_relative(input))), file);
    for (const auto &file : m_memoryFiles) {
        auto relative = file.path.lexically_relative(input);
        if (relative.empty() || *relative.begin() == "..") continue;
        auto key = archivePathKey(pathUtf8(relative));
        auto verified = ContentAnalyzer::inspectMemory(file.path, file.memoryContent, &m_cancel,
            m_run.settings.enableSimilarityGroups);
        if (verified.contentChecksum != file.contentChecksum)
            throw std::runtime_error("RAM input changed during optimization");
        auto existing = files.find(key);
        if (existing != files.end() && existing->second.contentChecksum != file.contentChecksum)
            throw std::runtime_error("Disk fallback input changed during optimization");
        files[key] = std::move(verified);
    }
    std::vector<FileTypeInfo> combined;
    for (auto &[key, file] : files) combined.push_back(std::move(file));
    auto empty = manifest.summary.emptyDirectories;
    empty.erase(std::remove_if(empty.begin(), empty.end(), [&](const auto &directory) {
        return std::any_of(combined.begin(), combined.end(), [&](const auto &file) {
            auto relative = file.path.lexically_relative(directory);
            return !relative.empty() && *relative.begin() != "..";
        });
    }), empty.end());
    return InputManifest::fromEntries(input, std::move(combined), std::move(empty));
}
bool OptimizerEngine::checkpoint() {
    StageMetrics::Scope timing(&m_metrics, "Checkpoint");
    // Serialize snapshots and writes together so an older worker cannot overwrite a newer state.
    std::lock_guard<std::mutex> checkpointLock(m_checkpointMutex);
    SessionInfo s;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        // A verified finishing worker may race with an unverified Disk winner.
        // Only replace the journal when its actual leader can be restored.
        std::error_code ec;
        if (m_best.taskId <= 0 || !m_best.verified || m_best.tempArchivePath.empty() ||
            !isWithin(m_best.tempArchivePath, m_run.workspace) ||
            !fs::is_regular_file(m_best.tempArchivePath, ec) || ec)
            return false;
        const auto leaderTask = std::find_if(m_tasks.begin(), m_tasks.end(), [&](const auto &task) {
            return task.taskId == m_best.taskId && task.configId == m_best.configId &&
                   redactFlags(task.flags) == m_best.flagsStr;
        });
        if (leaderTask == m_tasks.end()) return false;
        s.sessionId = m_run.sessionId;
        s.inputPath = m_run.input;
        s.outputPath = m_run.output;
        s.settings = m_run.settings;
        s.workspace = m_run.workspace;
        s.settings.customTempDir = m_run.workspace.parent_path();
        s.fingerprint = m_run.fingerprint;
        s.results = m_results;
        // A resumed task may be retried while its old complete artifact remains the fallback.
        auto durable = std::find_if(s.results.begin(), s.results.end(), [&](const auto &r) {
            return r.taskId == m_best.taskId;
        });
        if (durable != s.results.end() && (!durable->verified || durable->tempArchivePath != m_best.tempArchivePath)) {
            const auto status = durable->status;
            *durable = m_best;
            if (status == CandidateStatus::Queued || status == CandidateStatus::Running)
                durable->status = CandidateStatus::Queued;
        }
        s.leaderTaskId = m_best.taskId > 0 ? m_best.taskId : -1;
        for (const auto &r : m_results)
            if (r.status == CandidateStatus::Verified || r.status == CandidateStatus::Passed)
                s.completedTaskIds.push_back(r.taskId);
    }
    const bool saved = !s.sessionId.empty() && m_sessions.saveCheckpoint(s);
    if (saved) {
        std::lock_guard<std::mutex> l(m_mutex);
        m_hasDurableCheckpoint = true;
    } else
        log("[WARNING] Could not save session checkpoint.");
    return saved;
}
void OptimizerEngine::runQuickProbe(std::vector<TaskConfig> &tasks, uint32_t ordinal) {
    StageMetrics::Scope timing(&m_metrics, "ProbeSelection");
    { std::lock_guard lock(m_mutex); m_searchStage = "Probe"; }
    auto clearStage = std::shared_ptr<void>(nullptr, [this](void *) {
        std::lock_guard lock(m_mutex); m_searchStage.clear();
    });
    auto summary = getAnalysisSummary();
    // Avoid a second compression pass on tiny groups. Seed a whole-data tournament;
    // the verified leader still receives bounded numerical refinement afterwards.
    if (summary.totalBytes <= 2ULL * 1048576) {
        auto kept = SearchPlanner::smallInputFinalists(tasks, m_run.settings.searchMode);
        log("[Probe] Small-input whole-data tournament: " + std::to_string(kept.size()) +
            " of " + std::to_string(tasks.size()) + " candidates.");
        tasks = std::move(kept);
        return;
    }
    auto sample = m_run.workspace / ("sample-" + std::to_string(ordinal));
    fs::create_directories(sample);
    // The visible MiB setting is authoritative; legacy percentages cannot silently shrink it.
    const uint64_t limit = std::min<uint64_t>(summary.totalBytes,
        uint64_t(m_run.settings.probeSampleMB) * RamPolicy::MiB);
    const auto fragments = ProbePlanner::plan(summary.files, limit, 64, ordinal);
    const bool trustedWholePe = ProbePlanner::wholePeCovered(summary.files, fragments);
    if (fragments.empty()) {
        log("[Probe] No representative whole PE sample fits; retaining the complete matrix.");
        return;
    }
    uint64_t sampleBytes = 0;
    uint64_t plannedSampleBytes = 0;
    for (const auto &fragment : fragments) plannedSampleBytes += fragment.bytes;
    std::shared_ptr<AdmissionBroker::Lease> sampleLease;
    std::shared_ptr<RamArtifact> sampleStorage;
    const auto sampleReservedMB = (plannedSampleBytes + RamPolicy::MiB - 1) / RamPolicy::MiB;
    if (m_run.settings.enableRamCandidates && plannedSampleBytes &&
        plannedSampleBytes <= m_run.settings.ramGroupLimitMB * RamPolicy::MiB &&
        sampleReservedMB + m_inputRamReservedMB + 256 < m_run.settings.ramLimitMB)
    {
        if (m_broker) sampleLease = m_broker->tryAcquire(m_run.sessionId + ":sample:" + std::to_string(ordinal), sampleReservedMB, 0, false, false);
        if (!m_broker || sampleLease)
            sampleStorage = RamArtifact::allocate(m_ramBudget, sampleReservedMB * RamPolicy::MiB, plannedSampleBytes);
        if (!sampleStorage) {
            sampleLease.reset();
            if (m_broker) m_broker->withdraw(m_run.sessionId + ":sample:" + std::to_string(ordinal));
        }
    }
    std::vector<FileTypeInfo> sampleEntries;
    std::set<size_t> sampleFiles;
    auto sampleCopyLease = acquireLease(m_run.sessionId + ":sample-copy:" + std::to_string(ordinal), 16, 0);
    for (const auto &fragment : fragments) {
        const auto &f = summary.files[fragment.fileIndex];
        std::ifstream in;
        if (!f.memoryContent) {
            in.open(f.path, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(fragment.offset));
        }
        auto samplePath = sample / (std::to_string(fragment.fileIndex) + fragment.suffix +
                                    pathUtf8(f.path.extension()));
        std::ofstream out;
        if (!sampleStorage) out.open(samplePath, std::ios::binary);
        auto length = fragment.bytes;
        std::array<char, 65536> data{};
        while (length) {
            if (m_cancel || m_finish)
                return;
            auto count = std::min<uint64_t>(length, data.size());
            if (f.memoryContent) {
                auto offset = fragment.offset + fragment.bytes - length;
                if (offset > f.memoryContent->size || count > f.memoryContent->size - offset)
                    throw std::runtime_error("RAM probe fragment outside content source");
                if (sampleStorage) sampleStorage->write(sampleBytes + fragment.bytes - length, f.memoryContent->data + offset, count);
                else out.write(reinterpret_cast<const char *>(f.memoryContent->data + offset), static_cast<std::streamsize>(count));
            } else {
                if (!in.read(data.data(), static_cast<std::streamsize>(count)))
                    throw std::runtime_error("Input changed during sampling");
                if (sampleStorage) sampleStorage->write(sampleBytes + fragment.bytes - length, data.data(), count);
                else out.write(data.data(), static_cast<std::streamsize>(count));
            }
            length -= count;
        }
        if (sampleStorage) {
            auto memory = std::make_shared<MemoryContent>();
            memory->owner = sampleStorage;
            memory->data = sampleStorage->data() + sampleBytes;
            memory->mappingName = sampleStorage->mappingName();
            memory->offset = sampleBytes; memory->size = fragment.bytes;
            memory->capacity = sampleStorage->capacity;
            // Identical metadata if a probe switches from RAM to CLI on disk.
            memory->mtime = memory->ctime = memory->atime = 125911584000000000ULL;
            sampleEntries.push_back(ContentAnalyzer::inspectMemory(samplePath, memory, &m_cancel));
        } else {
            out.close();
            if (!out) throw std::runtime_error("Cannot create probe sample");
#ifdef _WIN32
            HANDLE metadata = CreateFileW(samplePath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (metadata == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot set probe metadata");
            constexpr uint64_t ticks = 125911584000000000ULL;
            const FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
            const bool restored = SetFileTime(metadata, &time, &time, &time) != FALSE;
            CloseHandle(metadata);
            if (!restored) throw std::runtime_error("Cannot restore deterministic probe metadata");
#endif
            m_probeInputWriteBytes += fragment.bytes;
        }
        sampleBytes += fragment.bytes;
        sampleFiles.insert(fragment.fileIndex);
    }
    sampleCopyLease.reset();
    log("[Probe] Size/type strata: " + std::to_string(sampleFiles.size()) + " files, " +
        std::to_string(sampleBytes) + " bytes; separate start/middle/end fragments, whole PE.");
    Json sampleIdentity = Json::array();
    for (const auto &fragment : fragments) {
        const auto &file = summary.files[fragment.fileIndex];
        sampleIdentity.push_back({{"path", pathUtf8(file.path.lexically_relative(m_run.effectiveInput))},
            {"sourceSha256", file.contentChecksum}, {"offset", fragment.offset}, {"bytes", fragment.bytes}});
    }
    const auto sampleId = digestText(sampleIdentity.dump());
    std::shared_ptr<AnalysisSummary> ramSample;
    if (m_run.settings.enableRamCandidates)
        ramSample = std::make_shared<AnalysisSummary>(sampleStorage
            ? InputManifest::fromEntries(sample, std::move(sampleEntries), {}).summary
            : InputManifest::scan(sample, &m_cancel).summary);
    std::vector<ProbeResult> sizes(tasks.size());
    for (auto &outcome : sizes) {
        outcome.sampleId = sampleId;
        outcome.coveredBytes = sampleBytes; outcome.totalBytes = summary.totalBytes;
        outcome.coverageComplete = trustedWholePe;
    }
    uint64_t best = UINT64_MAX;
    struct ProbeJob {
        CompressionSpec spec;
        std::vector<size_t> indices;
    };
    std::vector<ProbeJob> jobs;
    std::map<std::string, size_t> probeCache;
    const auto probeDictionary = std::max<uint64_t>(1048576, (sampleBytes + 1023) / 1024 * 1024);
    for (size_t i = 0; i < tasks.size(); ++i) {
        auto family = tasks[i].spec.family();
        // These candidates always receive full-input tests. Sampling cannot prune
        // them and would consume the deadline needed by the remaining families.
        if (tasks[i].reuseInputArchive || tasks[i].groupedSearch ||
            (!trustedWholePe && (family == "BCJ" || family == "BCJ2")))
            continue;
        auto probeSpec = tasks[i].spec;
        for (auto &method : probeSpec.methods)
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2 || method.codec == Codec::PPMd)
                method.dictionary.value =
                    std::min(method.dictionary.value, probeDictionary);
        auto probeIdentity = probeSpec.id();
        auto [cached, inserted] = probeCache.emplace(probeIdentity, jobs.size());
        if (inserted)
            jobs.push_back({std::move(probeSpec), {i}});
        else
            jobs[cached->second].indices.push_back(i);
    }
    std::sort(jobs.begin(), jobs.end(), [](const auto &a, const auto &b) { return a.spec.id() < b.spec.id(); });
    const auto passes = m_run.settings.searchMode == SearchMode::Balanced ? m_run.settings.probePasses : 1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(
        std::max(1u, m_run.settings.probeBudgetSeconds / passes));
    auto stop = [&] {
        return m_cancel || m_finish || std::chrono::steady_clock::now() >= deadline;
    };
    ResourceScheduler budget{m_run.settings.ramLimitMB - m_inputRamReservedMB -
                                 (sampleStorage ? sampleReservedMB : 0), 0,
                             m_run.settings.cpuThreads
                                 ? m_run.settings.cpuThreads
                                 : coresForGrouping(m_run.settings),
                             0};
    struct PendingProbe {
        size_t job;
        uint64_t ram;
        uint32_t cpu;
        std::future<ProbeResult> size;
    };
    std::deque<PendingProbe> pending;
    auto collect = [&] {
        auto ready = pending.begin();
        while (!pending.empty()) {
            ready = std::find_if(pending.begin(), pending.end(), [](auto &probe) {
                return probe.size.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            });
            if (ready != pending.end())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        auto &probe = *ready;
        auto result = probe.size.get();
        for (auto index : jobs[probe.job].indices)
            sizes[index] = result;
        if (result.state == ProbeState::Success)
            best = std::min(best, result.bytes);
        budget.release(probe.ram, probe.cpu);
        {
            std::lock_guard lock(m_mutex);
            m_reservedMemory = budget.ramReserved + (sampleStorage ? sampleReservedMB : 0);
            m_scheduler.cpuReserved = budget.cpuReserved;
        }
        pending.erase(ready);
    };
    auto workers = std::max(1u, m_run.settings.workerThreads);
    for (size_t i = 0; i < jobs.size() && !stop(); ++i) {
        const auto &job = jobs[i];
        auto ram = job.spec.memoryMB();
        if (ramSample && RamPolicy::eligible(m_run.settings, ramSample->totalBytes, ramSample->totalFiles)) {
            TaskConfig probe; probe.spec = job.spec;
            auto fullReservation = RamPolicy::reservation(probe, ramSample->totalBytes, ramSample->totalFiles);
            if (fullReservation <= m_run.settings.ramLimitMB) ram = fullReservation;
        }
        auto cpu = job.spec.threads;
        while (!pending.empty() &&
               (pending.size() >= workers || !budget.fits(ram, cpu)))
            collect();
        if (stop())
            break;
        if (!budget.fits(ram, cpu))
            continue; // An untested probe remains eligible for full-input testing.
        budget.reserve(ram, cpu);
        {
            std::lock_guard lock(m_mutex);
            m_reservedMemory = budget.ramReserved + (sampleStorage ? sampleReservedMB : 0);
            m_scheduler.cpuReserved = budget.cpuReserved;
        }
        pending.push_back({i, ram, cpu,
                          std::async(std::launch::async, [this, spec = job.spec,
                                                         index = job.indices.front(), sample,
                                                         deadline, i, workers, ramSample, ram, cpu, sampleBytes, trustedWholePe, ordinal, sampleId] {
            ProbeResult outcome;
            outcome.sampleId = sampleId;
            outcome.aliasId = spec.id();
            outcome.coverageComplete = trustedWholePe;
            outcome.coveredBytes = sampleBytes; outcome.totalBytes = m_summary.totalBytes;
            const auto admissionKey = m_run.sessionId + ":probe:" + std::to_string(i);
            std::shared_ptr<AdmissionBroker::Lease> lease;
            while (m_broker && !lease && !m_cancel && !m_finish && std::chrono::steady_clock::now() < deadline) {
                lease = m_broker->tryAcquire(admissionKey, ram, cpu, !ramSample);
                if (!lease) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (m_broker && !lease) {
                m_broker->withdraw(admissionKey);
                outcome.state = ProbeState::ResourceRejected;
                return outcome;
            }
            auto archive = m_run.workspace / ("probe_" + std::to_string(index) + ".7z");
            struct Cleanup {
                fs::path archive;
                ~Cleanup() {
                    std::error_code ec;
                    fs::remove(archive, ec);
                }
            } cleanup{archive};
            std::vector<std::string> args = {"a", "-y", "-t7z",
                                             "-w" + pathUtf8(m_run.workspace)};
            auto probeFlags = spec.compile();
            args.insert(args.end(), probeFlags.begin(), probeFlags.end());
            if (!m_run.settings.password.empty())
                args.push_back("-p" + m_run.settings.password);
            if (m_run.settings.encryptHeaders)
                args.push_back("-mhe=on");
            args.push_back(pathUtf8(archive));
            args.push_back(pathUtf8(sample / "*"));
            ProcessControl control{nullptr, m_run.settings.processTimeoutSeconds
                                                ? std::min(m_run.settings.probeTimeoutSeconds, m_run.settings.processTimeoutSeconds)
                                                : m_run.settings.probeTimeoutSeconds};
            control.lifecycleCallback = [this, id = spec.id()](uint32_t pid, const char *action) {
                event("Probe", id, pid, action);
            };
            control.resourceStop = [this, deadline] {
                return m_finish || std::chrono::steady_clock::now() >= deadline;
            };
            if (ramSample) {
                TaskConfig probe; probe.spec = spec;
                auto reservation = RamPolicy::reservation(probe, ramSample->totalBytes, ramSample->totalFiles);
                if (reservation <= m_run.settings.ramLimitMB) {
                    auto ram = RamCandidate::run(probe, *ramSample, sample, m_run.settings, m_ramBudget,
                        &activeGuard(), lease ? lease->workerIndex : static_cast<uint32_t>(i % workers) + m_workerOffset, &m_cancel, &control);
                    if (ram.artifact) {
                        ++m_ramProbes; outcome.state = ProbeState::Success;
                        outcome.bytes = ram.artifact->size; outcome.seconds = ram.process.durationSeconds;
                        return outcome;
                    }
                    if (ram.fallbackReason.empty()) {
                        outcome.state = ram.process.timedOut ? ProbeState::Timeout : ProbeState::Error;
                        outcome.seconds = ram.process.durationSeconds; return outcome;
                    }
                }
            }
            if (!acquireDisk(lease, nullptr, control.resourceStop)) {
                outcome.state = ProbeState::ResourceRejected;
                return outcome;
            }
            if (ramSample) {
                std::lock_guard lock(m_materializeMutex);
                for (const auto &file : ramSample->files)
                    if (RamInput::materialize(file)) m_probeInputWriteBytes += file.sizeBytes;
            }
            auto result = ProcessManager::execute(
                pathUtf8(ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir)), args,
                sample, &activeGuard(), lease ? lease->workerIndex : static_cast<uint32_t>(i % workers) + m_workerOffset, &m_cancel, nullptr, &control);
            if (result.success) {
                auto bytes = fs::file_size(archive);
                m_probeFileWriteBytes += bytes;
                outcome.state = ProbeState::Success; outcome.bytes = bytes;
                outcome.seconds = result.durationSeconds;
                return outcome;
            }
            outcome.state = result.timedOut ? ProbeState::Timeout : ProbeState::Error;
            outcome.seconds = result.durationSeconds;
            return outcome;
        })});
    }
    while (!pending.empty())
        collect();
    {
        std::lock_guard lock(m_mutex);
        m_reservedMemory = 0;
        m_scheduler.cpuReserved = 0;
    }
    {
        auto mode = m_run.settings.searchMode;
        auto kept = SearchPlanner::finalists(tasks, sizes, mode, m_run.settings.pruningThreshold,
                                            trustedWholePe);
        for (size_t i = 0; i < tasks.size(); ++i) {
            const auto &probe = sizes[i];
            std::lock_guard lock(m_mutex);
            m_probeEvidence.push_back({{"configId", tasks[i].configId}, {"state", static_cast<int>(probe.state)},
                {"bytes", probe.bytes}, {"seconds", probe.seconds}, {"sampleId", probe.sampleId},
                {"aliasId", probe.aliasId}, {"coveredBytes", probe.coveredBytes}, {"totalBytes", probe.totalBytes},
                {"promoted", std::any_of(kept.begin(), kept.end(), [&](const auto &task) { return task.configId == tasks[i].configId; })}});
        }
        log("[Probe] Kept " + std::to_string(kept.size()) + " of " + std::to_string(tasks.size()) +
            " candidates; sampling is heuristic.");
        tasks = std::move(kept);
    }
    std::error_code ec;
    fs::remove_all(sample, ec);
    ramSample.reset(); sampleEntries.clear(); sampleStorage.reset();
    sampleLease.reset();
    timing.finish(); // Recursive passes are separate intervals, not nested effort.
    if (!m_cancel && !m_finish && ordinal + 1 < passes && sampleBytes < summary.totalBytes)
        runQuickProbe(tasks, ordinal + 1);
}
void OptimizerEngine::coordinatorLoop() {
    // Declared before local manifests, so their mapping owners die before this cleanup.
    auto releaseRamInput = std::shared_ptr<void>(nullptr, [this](void *) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_memoryFiles.clear();
        m_injectedMemoryFiles.clear();
        for (auto &file : m_summary.files) file.memoryContent.reset();
    });
    bool success = false;
    try {
        m_probeFileWriteBytes = 0;
        m_ramProbes = 0;
        m_probeInputWriteBytes = 0;
        m_materializedInputBytes = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_inputRamReservedMB = m_inputRamBytes = m_inputDiskBytes = 0;
            m_inputDecoderCpuSeconds = 0;
            m_inputDecodeMetrics = Json::object();
        }
        m_memoryFiles = m_injectedMemoryFiles;
        if (!m_sharedGuard)
            m_ramBudget = std::make_shared<RamBudget>(std::min(m_run.settings.ramTemporaryBudgetMB,
                m_run.settings.ramLimitMB) * RamPolicy::MiB);
        auto base = m_run.settings.customTempDir.empty()
                        ? m_run.input.parent_path() / ".ultraarch_tmp"
                        : fs::absolute(m_run.settings.customTempDir);
        fs::create_directories(base);
        m_sessions.setSessionDir(base / "sessions");
        m_run.sessionId = uniqueId();
        m_run.workspace = base / ("run_" + m_run.sessionId);
        fs::create_directories(m_run.workspace);
        WorkspaceManager::mark(m_run.workspace, m_run.sessionId);
        if (!m_sharedGuard && !m_guard.initialize(m_run.settings.ramLimitMB))
            throw std::runtime_error("Cannot initialize memory protection");
        if (!m_sharedGuard && m_run.settings.cpuLimitPercent && !m_guard.setCpuRate(m_run.settings.cpuLimitPercent))
            throw std::runtime_error("Cannot enforce requested CPU hard cap");
        m_run.settings.customTempDir = m_run.workspace;
        if (!m_sharedGuard) m_guard.setScheduling(static_cast<int>(m_run.settings.affinityMode),
                              m_run.settings.backgroundPriority);
        if (!m_broker)
            m_broker = std::make_shared<AdmissionBroker>(m_run.settings.ramLimitMB,
                coresForGrouping(m_run.settings), m_run.settings.diskTaskLimit ? m_run.settings.diskTaskLimit : m_run.settings.workerThreads,
                0, m_run.settings.workerThreads);
        if (!m_sharedGuard) m_broker->setAdmissionCheck([this](uint64_t planned, uint32_t cpu) -> std::string {
#ifdef _WIN32
            // Job usage already consumes system commit. Reserve only the remaining planned peak.
            const auto committed = activeGuard().getCurrentMemoryMB();
            const auto ephemeral = committed > m_inputRamReservedMB ? committed - m_inputRamReservedMB : 0;
            const auto additional = planned > ephemeral ? planned - ephemeral : 0;
            if (!ResourceGuard::hasMemoryHeadroom(additional, 128))
                return "System memory/commit pressure; waiting for planned peak headroom";
            if ((m_run.settings.autoCpu || m_run.settings.backgroundPriority) && cpu &&
                ResourceGuard::systemCpuUsage() >= 0.90)
                return "System CPU pressure; Auto/background admission paused";
#endif
            return {};
        });
        auto originalManifest = !m_memoryFiles.empty() ? manifestForInput(m_run.input) :
            InputManifest::scan(m_run.input, &m_cancel, [this](uint64_t files, uint64_t bytes) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_analyzedFiles = files;
                m_analyzedBytes = bytes;
            }, m_run.settings.enableSimilarityGroups);
        m_run.inputFingerprint = originalManifest.fingerprint;
        m_run.effectiveInput = m_run.input;
        std::optional<InputManifest> ramInputManifest;
        if (ArchiveExtractor::isArchive(m_run.input)) {
            auto extractionLease = acquireLease(m_run.sessionId + ":extract",
                m_broker->baselineRam(), coresForGrouping(m_run.settings));
            changeState(EngineState::Extracting);
            m_run.effectiveInput = m_run.workspace / "extracted";
            bool extracted = false;
            if (m_run.settings.enableRamCandidates && !m_run.settings.enablePreprocessors &&
                m_run.settings.contentPolicy == ContentPolicy::ExactBytes) {
                auto ram = RamInput::extract(m_run.input, m_run.effectiveInput, m_run.settings,
                    m_ramBudget, &activeGuard(), &m_cancel);
                {
                    std::lock_guard lock(m_mutex);
                    m_inputDecoderCpuSeconds = ram.cpuSeconds;
                    m_inputDecodeMetrics = {{"cpuSeconds", ram.cpuSeconds},
                        {"wallSeconds", ram.process.durationSeconds}, {"peakCommittedMB", ram.process.peakMemoryMB},
                        {"readTransferBytes", ram.process.readBytes}, {"writeTransferBytes", ram.process.writeBytes},
                        {"scope", "Hybrid decoder process only; transfer includes IPC, memory excludes parent mapping"}};
                }
                if (ram.success) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_inputRamReservedMB = ram.reservedMB;
                        m_inputRamBytes = ram.ramBytes;
                        m_inputDiskBytes = ram.diskBytes;
                    }
                    for (const auto &file : ram.manifest.summary.files)
                        if (file.memoryContent) m_memoryFiles.push_back(file);
                    ramInputManifest = std::move(ram.manifest);
                    extracted = true;
                    log("[RAM input] " + std::to_string(m_inputRamBytes) + " bytes in RAM; " +
                        std::to_string(m_inputDiskBytes) + " bytes on disk");
                } else if (ram.fallbackReason.empty())
                    throw std::runtime_error("RAM input extraction failed: " + ram.process.errOutput);
                else log("[RAM input fallback] " + ram.fallbackReason);
            }
            if (!extracted && !ArchiveExtractor::extract(
                    m_run.input, m_run.effectiveInput, m_run.settings.toolsBinDir, &m_cancel,
                    [this](const auto &m) { log(m); }, m_run.settings.password, &activeGuard(),
                    m_run.settings.processTimeoutSeconds,
                    {m_run.settings.extractionLimitMB * 1048576,
                     m_run.settings.extractionFileLimitMB * 1048576,
                     m_run.settings.extractionFileCount}))
                throw std::runtime_error("Input extraction failed");
        }
        if (!m_sharedGuard && !m_broker->setPersistent(m_inputRamReservedMB))
            throw std::runtime_error("Cannot account persistent RAM input while operations are active");
        if (m_run.settings.enablePreprocessors ||
            m_run.settings.contentPolicy == ContentPolicy::OptimizeResources) {
            if (m_run.settings.enablePreprocessors &&
                m_run.settings.profile != OptimizationProfile::Extreme_RePack)
                throw std::runtime_error("Legacy --preprocess requires Extreme; use --content "
                                         "optimize for independent policy");
            auto preprocessingLease = acquireLease(m_run.sessionId + ":preprocess",
                m_broker->baselineRam(), coresForGrouping(m_run.settings));
            changeState(EngineState::Preprocessing);
            PreprocessorPipeline pipeline;
            pipeline.detectAvailableTools(m_run.settings.toolsBinDir);
            if (!pipeline.hasAvailableTools())
                throw std::runtime_error("No supported preprocessors found (Leanify/ECT)");
            auto result = pipeline.execute(
                m_run.effectiveInput, m_run.workspace / "preprocessed", m_run.settings.toolsBinDir,
                &m_cancel, [this](const auto &m) { log(m); }, &activeGuard(),
                m_run.settings.processTimeoutSeconds);
            m_run.effectiveInput = result.outputPath;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_changedFiles = result.changedFiles;
            }
            for (const auto &step : result.steps)
                log("[Preprocess] " + step.stepName + ": " + std::to_string(step.inputBytes) +
                    " -> " + std::to_string(step.outputBytes) +
                    (step.success ? "" : " (failed; changes discarded)"));
        }
        if (m_cancel)
            throw std::runtime_error("Cancelled");
        changeState(EngineState::Analyzing);
        auto effectiveManifest = ramInputManifest ? std::move(*ramInputManifest) : m_run.effectiveInput == m_run.input
                                     ? std::move(originalManifest)
                                     : InputManifest::scan(m_run.effectiveInput, &m_cancel, {},
                                                           m_run.settings.enableSimilarityGroups);
        auto summary = std::move(effectiveManifest.summary);
        if (!summary.totalFiles)
            throw std::runtime_error("Input contains no readable regular files");

        {
            std::lock_guard<std::mutex> l(m_mutex);
            if (ArchiveExtractor::isArchive(m_run.input) && !ramInputManifest)
                m_inputDiskBytes = summary.totalBytes;
            m_summary = summary;
        }
        m_run.effectiveFingerprint = m_run.effectiveInput == m_run.input
                                         ? m_run.inputFingerprint
                                         : effectiveManifest.fingerprint;
        auto canonicalSettings = m_run.settings;
        canonicalSettings.customTempDir = base;
        Json tools = Json::array();
        std::vector<fs::path> relevantTools = {
            ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir),
            m_run.settings.toolsBinDir / "7z.dll"};
#ifdef _WIN32
        wchar_t module[32768]{};
        if (GetModuleFileNameW(nullptr, module, 32768)) {
            auto executable = fs::path(module);
            relevantTools.push_back(executable);
            auto app = executable.parent_path();
            for (const auto &name : {"verify-worker.exe", "encode-worker.exe", "extract-worker.exe"}) {
                if (!m_run.settings.enableRamCandidates && std::string(name) != "verify-worker.exe") continue;
                for (const auto &candidate : {app / name, app.parent_path() / name,
                     app.parent_path().parent_path() / app.filename() / name,
                     m_run.settings.toolsBinDir.parent_path() / name})
                    if (fs::is_regular_file(candidate)) { relevantTools.push_back(candidate); break; }
            }
        }
#endif
        if (m_run.settings.sfxOutput)
            relevantTools.push_back(m_run.settings.toolsBinDir / "7z.sfx");
        if (m_run.settings.enablePreprocessors ||
            m_run.settings.contentPolicy == ContentPolicy::OptimizeResources) {
            relevantTools.push_back(
                ProcessManager::resolveExecutable("leanify", m_run.settings.toolsBinDir));
            relevantTools.push_back(
                ProcessManager::resolveExecutable("ect", m_run.settings.toolsBinDir));
        }
        for (const auto &file : relevantTools)
            if (!file.empty() && fs::is_regular_file(file))
                tools.push_back({pathUtf8(file.filename()), fileChecksum(file, &m_cancel)});
        m_run.fingerprint =
            digestText("engine-schema-13-sha256" + m_run.inputFingerprint +
                       m_run.effectiveFingerprint + settingsJson(canonicalSettings).dump() +
                       tools.dump() + (!m_run.settings.password.empty() ? "encrypted" : "plain"));
        TaskMatrixGenerator generator;
        auto candidateSettings = m_run.settings;
        candidateSettings.ramLimitMB -= m_inputRamReservedMB;
        m_tasks = generator.generateMatrix(summary, candidateSettings);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_toolIdentities = {{"resolved", tools}};
            auto pinned = m_run.settings.toolsBinDir / "tool-manifest.json";
            if (fs::is_regular_file(pinned)) {
                std::ifstream in(pinned);
                Json manifest;
                in >> manifest;
                m_toolIdentities["pinnedManifest"] = manifest;
            }
            m_generatedCount = static_cast<uint32_t>(m_tasks.size());
        }
        auto groupKeys = GroupPlanner::plan(summary);
        bool similarityChanged = false;
        if (m_run.settings.enableSimilarityGroups) {
            const auto alternative = GroupPlanner::plan(summary, true);
            similarityChanged = alternative.size() != groupKeys.size();
            auto a = alternative.cbegin(), b = groupKeys.cbegin();
            while (!similarityChanged && a != alternative.end()) {
                similarityChanged = a->first != b->first;
                ++a; ++b;
            }
        }
        bool hasGroups = m_run.settings.enableGrouping &&
                         m_run.settings.profile == OptimizationProfile::Extreme_RePack &&
                         (groupKeys.size() > 1 || similarityChanged) &&
                         summary.totalBytes > 2ULL * 1048576;
        if (hasGroups && m_run.settings.groupedSearchOnly) {
            m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [](const auto &task) {
                return task.spec.family() != "Store";
            }), m_tasks.end());
        }
        if (!m_broker)
            m_broker = std::make_shared<AdmissionBroker>(m_run.settings.ramLimitMB - m_inputRamReservedMB,
                coresForGrouping(m_run.settings), m_run.settings.diskTaskLimit ? m_run.settings.diskTaskLimit : m_run.settings.workerThreads,
                m_inputRamReservedMB, m_run.settings.workerThreads);
        auto capabilityLease = acquireLease(m_run.sessionId + ":capabilities",
            std::min<uint64_t>(256, m_run.settings.ramLimitMB - m_inputRamReservedMB), 1);
        m_tasks = ToolCapabilities::check(
            m_tasks, ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir),
            m_run.workspace, &activeGuard(), &m_cancel, [this](const auto &line) { log(line); });
        capabilityLease.reset();
        auto memoryTool = ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir);
        auto memoryIdentity = fileChecksum(memoryTool, &m_cancel);
        if (fs::is_regular_file(memoryTool.parent_path() / "7z.dll"))
            memoryIdentity = Hasher::text(
                memoryIdentity + fileChecksum(memoryTool.parent_path() / "7z.dll", &m_cancel));
        MemoryModel memoryModel(base / "memory-model-sha256.json", memoryIdentity,
                                summary.dominantCategory, summary.totalBytes);
        m_costModel = std::make_unique<CostModel>(base / "duration-model-sha256.json", memoryIdentity);
        memoryModel.reserve(m_tasks, m_run.settings.ramLimitMB);
        if (hasGroups) {
            TaskConfig grouped;
            grouped.groupedSearch = true;
            grouped.taskId = 1;
            for (const auto &task : m_tasks)
                grouped.taskId = std::max(grouped.taskId, task.taskId + 1);
            grouped.name = "Grouped methods (verified assembly)";
            grouped.configId = Hasher::text("grouped-search-8:" + m_run.effectiveFingerprint +
                                          settingsJson(canonicalSettings).dump());
            grouped.spec.methods = {CodecSpec{Codec::Store}};
            grouped.estimatedRamMB = 0; // Children lease their actual candidates globally.
            grouped.cpuTokens = 0;
            m_tasks.push_back(grouped);
            ++m_generatedCount;
            if (similarityChanged) {
                ++grouped.taskId;
                grouped.similarityPartition = true;
                grouped.name = "Content similarity groups (verified competitor)";
                grouped.configId = Hasher::text("similarity-groups-1:" + m_run.effectiveFingerprint +
                                               settingsJson(canonicalSettings).dump());
                m_tasks.push_back(grouped);
                ++m_generatedCount;
            }
        }
        auto extension = pathUtf8(m_run.input.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".7z" && fs::is_regular_file(m_run.input) &&
            ArchiveExtractor::detectFormat(m_run.input) == "7z" &&
            m_run.settings.contentPolicy == ContentPolicy::ExactBytes &&
            !m_run.settings.enablePreprocessors && m_run.settings.password.empty() &&
            !m_run.settings.encryptHeaders && !m_run.settings.sfxOutput &&
            !m_run.settings.volumeSizeMB && !m_run.settings.maxLzmaDictionaryMB &&
            m_run.settings.solidBlockSize.empty()) {
            TaskConfig original;
            original.reuseInputArchive = true;
            m_compatibleInputArchiveBytes = fs::file_size(m_run.input);
            original.taskId = 1;
            for (const auto &task : m_tasks)
                original.taskId = std::max(original.taskId, task.taskId + 1);
            original.name = "Original archive (preserve compression)";
            original.configId = Hasher::text("original-archive-1:" + m_run.inputFingerprint);
            original.spec.methods = {CodecSpec{Codec::Store}};
            original.estimatedRamMB = m_run.settings.ramLimitMB - m_inputRamReservedMB;
            original.cpuTokens = 1;
            // Run before compression so a finish/time budget can preserve the input.
            m_tasks.insert(m_tasks.begin(), original);
            ++m_generatedCount;
        }
        if (!m_broker)
            m_broker = std::make_shared<AdmissionBroker>(m_run.settings.ramLimitMB - m_inputRamReservedMB,
                coresForGrouping(m_run.settings), m_run.settings.diskTaskLimit ? m_run.settings.diskTaskLimit : m_run.settings.workerThreads,
                m_inputRamReservedMB, m_run.settings.workerThreads);
        if (m_compatibleInputArchiveBytes && m_compatibleInputArchiveBytes < summary.totalBytes &&
            m_run.settings.selectionGoal == SelectionGoal::MinimumSize && m_run.settings.immediateCleanInferior) {
            std::erase_if(m_tasks, [](const auto &task) {
                return !task.reuseInputArchive && !task.groupedSearch && !task.spec.methods.empty() &&
                       task.spec.methods.front().codec == Codec::Store;
            });
            log("[Baseline] Store deferred: verified original is smaller than uncompressed content.");
        }
        const auto eligibleMatrix = m_tasks;
        if (m_baselineOnly) {
            // Coverage-only fallback deliberately omits method search even if
            // this tiny child finishes before the parent can request assembly.
            {
                std::lock_guard lock(m_mutex);
                m_searchInterrupted = true;
                m_stopAction = "EarlyCoverageBaseline";
                m_stopReason = "Early assembly requested; complete group coverage only";
            }
            const bool haveSeed = std::any_of(m_tasks.begin(), m_tasks.end(), [&](const auto &task) { return task.configId == m_seedConfig; });
            std::erase_if(m_tasks, [&](const auto &task) {
                return haveSeed ? task.configId != m_seedConfig : task.spec.methods.front().codec != Codec::Store;
            });
        }
        for (const auto &task : eligibleMatrix)
            m_predictedSeconds[task.taskId] = m_costModel->estimateSeconds(summary, task);
        if (m_cancel)
            throw std::runtime_error("Cancelled");
        m_run.list = m_run.workspace / "input.txt";
        auto root = fs::is_directory(m_run.effectiveInput) ? m_run.effectiveInput
                                                           : m_run.effectiveInput.parent_path();
        std::ofstream list(m_run.list, std::ios::binary);
        for (const auto &f : summary.files) {
            auto rel = pathUtf8(f.path.lexically_relative(root));
            if (rel.find_first_of("\r\n") != std::string::npos)
                throw std::runtime_error("Newlines in filenames are not supported");
            list << rel << '\n';
        }
        for (const auto &dir : summary.emptyDirectories)
            list << pathUtf8(dir.lexically_relative(root)) << '\n';
        list.close();
        if (!list)
            throw std::runtime_error("Cannot write candidate manifest");
        auto space = fs::space(m_run.workspace);
        uint64_t multiplier = uint64_t(m_run.settings.workerThreads) + 1;
        if (summary.totalBytes && space.available / summary.totalBytes < multiplier)
            throw std::runtime_error("Insufficient temporary disk space");
        SessionInfo resumed;
        CandidateResult restoredLeader;
        std::set<std::string> unfinishedGroupConfigs;
        bool canResume = false;
        if (m_sessions.tryResumeSession(m_run.input, canonicalSettings, resumed, m_run.fingerprint,
                                        m_requestedSession)) {
            if (!resumed.workspace.empty() &&
                WorkspaceManager::owned(resumed.workspace, fs::absolute(base))) {
                auto leader =
                    std::find_if(resumed.results.begin(), resumed.results.end(),
                                 [&](const auto &r) { return r.taskId == resumed.leaderTaskId; });
                bool configurationPresent =
                    leader != resumed.results.end() &&
                    std::any_of(m_tasks.begin(), m_tasks.end(), [&](const auto &task) {
                        return task.configId == leader->configId &&
                               redactFlags(task.flags) == leader->flagsStr;
                    });
                if (configurationPresent && leader->verified && !leader->tempArchivePath.empty() &&
                    isWithin(leader->tempArchivePath, resumed.workspace) &&
                    fs::is_regular_file(leader->tempArchivePath)) {
                    auto resumeLease = acquireLease(m_run.sessionId + ":resume-verify", m_broker->baselineRam(), 1);
                    std::string error;
                    canResume = ArchiveAssembler::verifyArchive(
                        leader->tempArchivePath, m_run.settings.toolsBinDir, error,
                        m_run.settings.password, &m_cancel, &activeGuard(),
                        m_run.settings.processTimeoutSeconds);
                    if (canResume) {
                        auto copy =
                            m_run.workspace / ("restored_" + std::to_string(leader->taskId) + ".7z");
                        fs::copy_file(leader->tempArchivePath, copy);
                        leader->tempArchivePath = copy;
                        restoredLeader = *leader;
                        auto currentTask =
                            std::find_if(m_tasks.begin(), m_tasks.end(), [&](const auto &t) {
                                return t.configId == leader->configId;
                            });
                        restoredLeader.taskId = currentTask->taskId;
                    }
                }
            }
        }
        if (canResume) {
            // A published, fully covered grouped archive can still be the result
            // of early assembly. Its validity does not prove that every group's
            // method search completed. Resume those searches without overwriting
            // the independent durable complete-archive fallback.
            for (const auto &task : m_tasks) {
                if (!task.groupedSearch) continue;
                const auto expected = GroupPlanner::plan(m_summary, task.similarityPartition);
                const auto directory = resumed.workspace /
                    (task.similarityPartition ? "similarity-groups" : "groups");
                bool complete = !expected.empty();
                for (const auto &[key, files] : expected) {
                    const auto group = directory / Hasher::text(key).substr(0, 16);
                    const auto marker = group / "winner.json";
                    auto winner = group / "winner.7z";
                    try { winner = GroupCheckpoint::artifact(group, GroupCheckpoint::read(group)); } catch (...) {}
                    try {
                        Json record;
                        if (!fs::is_regular_file(marker) || !fs::is_regular_file(winner) ||
                            isLinkOrReparse(marker) || isLinkOrReparse(winner)) {
                            complete = false;
                            break;
                        }
                        std::ifstream saved(marker); saved >> record;
                        if (!record.value("searchComplete", false)) {
                            complete = false;
                            break;
                        }
                    } catch (...) {
                        complete = false;
                        break;
                    }
                }
                if (!complete) unfinishedGroupConfigs.insert(task.configId);
            }
        }
        {
            std::lock_guard<std::mutex> l(m_mutex);
            if (canResume)
                m_best = restoredLeader;
            for (const auto &t : m_tasks) {
                CandidateResult r;
                r.taskId = t.taskId;
                r.configId = t.configId;
                r.name = t.name;
                r.flagsStr = redactFlags(t.flags);
                r.originalBytes = summary.totalBytes;
                if (t.resourceBlocked) {
                    r.status = CandidateStatus::Deferred;
                    r.statusMessage = "RAM budget blocked; explicit temporary override required";
                }
                if (canResume) {
                    auto old = std::find_if(
                        resumed.results.begin(), resumed.results.end(), [&](const auto &previous) {
                            return previous.configId == t.configId &&
                                   previous.flagsStr == r.flagsStr &&
                                   (previous.status == CandidateStatus::Verified ||
                                    previous.status == CandidateStatus::Passed ||
                                    (previous.verified && previous.taskId == resumed.leaderTaskId));
                        });
                    if (old != resumed.results.end()) {
                        r = *old;
                        r.taskId = t.taskId;
                        r.tempArchivePath =
                            r.taskId == m_best.taskId ? m_best.tempArchivePath : fs::path{};
                        if (unfinishedGroupConfigs.count(t.configId)) {
                            r.status = CandidateStatus::Queued;
                            r.statusMessage = "Durable grouped archive retained; unfinished group searches queued";
                            m_retainedGroups = true;
                        } else ++m_completed;
                    }
                }
                m_results.push_back(r);
                m_taskCancel.push_back(std::make_shared<std::atomic<bool>>(false));
            }
        }
        if (canResume) {
            m_smallestBytes = m_best.outputBytes;
            log("[Session] Resumed " + std::to_string(m_completed) + " candidates.");
            std::error_code ec;
            for (const auto *groupDirectory : {"groups", "similarity-groups"}) {
            auto savedGroups = resumed.workspace / groupDirectory;
            if (fs::is_directory(savedGroups)) {
                // Child publication journals contain absolute paths. Never relocate an
                // unfinished transaction or its backups. Only completed group artifacts
                // cross to the new workspace; input overlays are rebuilt from the manifest.
                for (const auto &entry : fs::directory_iterator(savedGroups)) {
                    if (!entry.is_directory() || isLinkOrReparse(entry.path()))
                        continue;
                    auto marker = entry.path() / "winner.json";
                    auto winner = entry.path() / "winner.7z";
                    try { winner = GroupCheckpoint::artifact(entry.path(), GroupCheckpoint::read(entry.path())); } catch (...) {}
                    if (!fs::is_regular_file(marker) || !fs::is_regular_file(winner) ||
                        isLinkOrReparse(marker) || isLinkOrReparse(winner))
                        continue;
                    auto destination = m_run.workspace / groupDirectory / entry.path().filename();
                    fs::create_directories(destination);
                    auto copyLease = acquireLease(m_run.sessionId + ":resume-group-copy", 16, 0);
                    GroupCheckpoint::copyTo(entry.path(), destination);
                    {
                        std::lock_guard lock(m_mutex);
                        m_retainedGroups = true;
                    }
                    if (fs::is_regular_file(entry.path() / "search-report.json"))
                        fs::copy_file(entry.path() / "search-report.json", destination / "search-report.json");
                }
            }
            }
            // Do not destroy the old resumable session until its copied leader
            // and new workspace are referenced by a durable replacement journal.
            if (!checkpoint())
                throw std::runtime_error("Could not persist restored session checkpoint");
            m_sessions.clearSession(resumed.sessionId);
            fs::remove_all(resumed.workspace, ec);
        }
        if (!canResume && !m_seedArchive.empty() && fs::is_regular_file(m_seedArchive)) {
            auto seeded = std::find_if(m_tasks.begin(), m_tasks.end(), [&](const auto &task) {
                return task.configId == m_seedConfig;
            });
            if (seeded != m_tasks.end()) {
                auto seedLease = acquireLease(m_run.sessionId + ":seed-verify",
                    std::max<uint64_t>(64, seeded->spec.memoryMB(false)), 1);
                std::string error;
                if (ArchiveAssembler::verifyArchive(m_seedArchive, m_run.settings.toolsBinDir, error,
                    m_run.settings.password, &m_cancel, &activeGuard(), m_run.settings.processTimeoutSeconds)) {
                    auto index = static_cast<size_t>(seeded - m_tasks.begin());
                    auto restored = m_results[index];
                    restored.tempArchivePath = m_run.workspace / ("cand_" + std::to_string(seeded->taskId) + ".7z");
                    fs::copy_file(m_seedArchive, restored.tempArchivePath);
                    restored.outputBytes = fs::file_size(restored.tempArchivePath);
                    restored.temporaryFileBytes = restored.outputBytes;
                    restored.status = CandidateStatus::Verified; restored.verified = restored.isLeader = true;
                    restored.statusMessage = "Durable group leader restored; unfinished search continues";
                    {
                        std::lock_guard lock(m_mutex);
                        m_results[index] = m_best = restored;
                        m_smallestBytes = restored.outputBytes;
                        ++m_completed;
                    }
                    if (!checkpoint()) throw std::runtime_error("Cannot checkpoint restored group leader");
                    log("[Session] Restored unfinished group's durable leader.");
                    std::lock_guard lock(m_mutex);
                    for (size_t i = 0; i < m_tasks.size(); ++i) {
                        if (m_tasks[i].configId == m_seedConfig) continue;
                        auto old = std::find_if(m_seedResults.begin(), m_seedResults.end(), [&](const auto &result) {
                            return result.configId == m_tasks[i].configId &&
                                (result.status == CandidateStatus::Verified || result.status == CandidateStatus::DiscardedBySize);
                        });
                        if (old == m_seedResults.end()) continue;
                        m_results[i] = *old;
                        m_results[i].taskId = m_tasks[i].taskId;
                        m_results[i].tempArchivePath.clear(); m_results[i].isLeader = false;
                        ++m_completed;
                    }
                }
            }
        }
        if (m_tasks.empty())
            throw std::runtime_error("No candidates fit the configured memory limit");
        log("[Engine] Testing " + std::to_string(m_tasks.size()) + " candidates.");
        changeState(EngineState::TestingCandidates);
        uint32_t count =
            std::min<uint32_t>(m_run.settings.workerThreads, static_cast<uint32_t>(m_tasks.size()));
        m_executorTarget = count;
        if (!m_sharedGuard) ResourceGuard::setWorkerCount(m_run.settings.workerThreads);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_scheduler = {m_run.settings.ramLimitMB - m_inputRamReservedMB, 0,
                           m_run.settings.cpuThreads
                               ? m_run.settings.cpuThreads
                               : coresForGrouping(m_run.settings),
                           0};
        }
        if (!m_run.settings.groupedSearchOnly && (m_run.settings.enableAdaptivePruning ||
            m_run.settings.searchMode != SearchMode::Exhaustive)) {
            log("[Baseline] Testing Store and the smallest LZMA2 configuration on the full input.");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_admissionLimit = std::min<size_t>(!m_tasks.empty() && m_tasks.front().reuseInputArchive ? 3 : 2,
                                                   m_tasks.size());
            }
            workerFunc(0);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_admissionLimit = SIZE_MAX;
            }
            if (!m_cancel && !m_finish && (!m_run.settings.fullAttemptLimit ||
                m_fullAttemptsStarted < m_run.settings.fullAttemptLimit)) {
                auto finalists = m_tasks;
                runQuickProbe(finalists);
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_best.taskId > 0 &&
                    std::none_of(finalists.begin(), finalists.end(), [&](const auto &task) {
                        return task.configId == m_best.configId;
                    })) {
                    auto leader =
                        std::find_if(m_tasks.begin(), m_tasks.end(), [&](const auto &task) {
                            return task.configId == m_best.configId;
                        });
                    if (leader != m_tasks.end())
                        finalists.push_back(*leader);
                }
                std::vector<CandidateResult> retained;
                std::vector<std::shared_ptr<std::atomic<bool>>> tokens;
                for (const auto &task : finalists) {
                    auto old =
                        std::find_if(m_results.begin(), m_results.end(), [&](const auto &result) {
                            return result.configId == task.configId;
                        });
                    auto index = static_cast<size_t>(old - m_results.begin());
                    retained.push_back(*old);
                    tokens.push_back(m_taskCancel[index]);
                }
                m_tasks = std::move(finalists);
                m_results = std::move(retained);
                m_taskCancel = std::move(tokens);
                m_completed = static_cast<int>(
                    std::count_if(m_results.begin(), m_results.end(), [](const auto &result) {
                        return result.status != CandidateStatus::Queued &&
                               result.status != CandidateStatus::Running;
                    }));
            }
        }
        for (uint32_t i = 0; i < std::min(64u, count + 1); ++i)
            m_workers.emplace_back(&OptimizerEngine::workerFunc, this, i);
        for (auto &worker : m_workers)
            worker.join();
        m_workers.clear();
        for (unsigned round = 0; round < 2 && !m_cancel && !m_finish &&
             m_run.settings.searchMode != SearchMode::Exhaustive && m_best.taskId > 0 &&
             (!m_run.settings.fullAttemptLimit || m_fullAttemptsStarted < m_run.settings.fullAttemptLimit); ++round) {
            const auto previousLeader = m_best.configId;
            auto extra = SearchPlanner::refinement(eligibleMatrix, m_tasks, m_best.configId);
            if (extra.empty())
                break;
            if (!extra.empty()) {
                log("[Refinement] Testing " + std::to_string(extra.size()) +
                    " full-input neighbours of the leader.");
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto &task : extra) {
                        m_tasks.push_back(task);
                        CandidateResult result;
                        result.taskId = task.taskId;
                        result.configId = task.configId;
                        result.name = task.name;
                        result.flagsStr = redactFlags(task.flags);
                        result.originalBytes = summary.totalBytes;
                        m_results.push_back(result);
                        m_taskCancel.push_back(std::make_shared<std::atomic<bool>>(false));
                    }
                }
                for (uint32_t i = 0; i < std::min(64u, count + 1); ++i)
                    m_workers.emplace_back(&OptimizerEngine::workerFunc, this, i);
                for (auto &worker : m_workers)
                    worker.join();
                m_workers.clear();
            }
            if (m_best.configId == previousLeader)
                break;
        }
        {
            std::lock_guard lock(m_mutex);
            if (std::any_of(m_results.begin(), m_results.end(), [](const auto &r) {
                    return r.status == CandidateStatus::Deferred;
                })) {
                m_searchInterrupted = true;
                if (m_stopAction.empty()) {
                    m_stopAction = "ResourceDeferred";
                    m_stopReason = "Valid candidates remain deferred by the explicit resource budget";
                }
            }
        }
        {
            std::lock_guard lock(m_mutex);
            m_overrideLeases.clear();
            if (m_broker) for (const auto &task : m_tasks)
                m_broker->withdraw(m_run.sessionId + ":" + task.configId);
        }
        memoryModel.save(getResults());
        m_costModel->record(summary, getResults());
        if (!m_costModel->save()) log("[WARNING] Could not save duration model.");
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto &result : m_results)
                if (result.status == CandidateStatus::Queued) {
                    result.status = CandidateStatus::Skipped;
                    result.statusMessage =
                        m_cancel ? "Cancelled before execution" : "Search stopped";
                    ++m_completed;
                }
        }
        if (m_cancel)
            throw std::runtime_error("Cancelled");
        if (m_manualWinner.empty() &&
            m_run.settings.selectionGoal == SelectionGoal::FastWithinTolerance) {
            std::lock_guard<std::mutex> lock(m_mutex);
            double allowed = static_cast<double>(m_smallestBytes) *
                             (1 + m_run.settings.sizeTolerancePercent / 100);
            for (const auto &candidate : m_results)
                if ((candidate.status == CandidateStatus::Verified ||
                     candidate.status == CandidateStatus::Passed) &&
                    candidate.outputBytes <= allowed && !candidate.tempArchivePath.empty() &&
                    (m_best.outputBytes > allowed ||
                     candidate.durationSeconds < m_best.durationSeconds))
                    m_best = candidate;
            for (auto &candidate : m_results)
                candidate.isLeader = candidate.configId == m_best.configId;
        }
        log("[Search] " +
            (m_stopReason.empty()
                 ? std::string("Scheduled candidates completed; heuristic exclusions: ") +
                       std::to_string(m_generatedCount - m_tasks.size())
                 : m_stopReason));
        if (m_best.taskId <= 0)
            throw std::runtime_error("No successful compression candidates");
        if (fingerprintInput(m_run.input) != m_run.inputFingerprint ||
            (m_run.effectiveInput != m_run.input &&
             fingerprintInput(m_run.effectiveInput) != m_run.effectiveFingerprint))
            throw std::runtime_error("Input changed during optimization; output was not published");
        changeState(EngineState::AssemblingFinal);
        auto win = std::find_if(m_tasks.begin(), m_tasks.end(),
                                [&](const auto &t) { return t.taskId == m_best.taskId; });
        if (win == m_tasks.end())
            throw std::runtime_error("Winning configuration was not found");
        AssemblyPlan plan;
        plan.targetOutputPath = m_run.output;
        plan.inputSourcePath = m_run.effectiveInput;
        plan.winningConfig = *win;
        plan.smartCategorized = false;
        plan.createSfx = m_run.settings.sfxOutput;
        plan.bestCandidateArchive = m_best.tempArchivePath;
        plan.reuseCandidate = true; // Wrapping/splitting preserves the selected method graph.
        plan.password = m_run.settings.password;
        plan.encryptHeaders = m_run.settings.encryptHeaders;
        plan.volumeSizeMB = m_run.settings.volumeSizeMB;
        plan.solidBlockSize = m_run.settings.solidBlockSize;
        std::vector<fs::path> outputs;
        const auto finalMemory = win->groupedSearch || win->reuseInputArchive ?
            m_run.settings.ramLimitMB - m_inputRamReservedMB : win->spec.memoryMB(false);
        if (m_best.memoryOverride && finalMemory > m_broker->snapshot().ramLimit &&
            finalMemory > m_broker->baselineRam() && finalMemory - m_broker->baselineRam() <= 1024) {
            m_broker->beginDrain();
            struct EndDrain { std::shared_ptr<AdmissionBroker> broker; ~EndDrain() { broker->endDrain(); } } drain{m_broker};
            while (!m_cancel && m_broker->snapshot().active)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!ResourceGuard::hasMemoryHeadroom(finalMemory) || !m_broker->raiseLimit(finalMemory, [&] {
                return activeGuard().updateMemoryLimits(finalMemory + m_broker->persistentMB());
            })) throw std::runtime_error("Cannot safely restore candidate's explicit RAM allowance for verification");
        }
        auto finalLease = acquireLease(m_run.sessionId + ":finalize", finalMemory, 1);
        success = ArchiveAssembler::assembleFinalArchive(
            plan, summary, m_run.settings, &activeGuard(), &m_cancel, [this](const auto &s) { log(s); },
            [this](float p) {
                std::lock_guard<std::mutex> l(m_mutex);
                m_progress = 0.9f + p * 0.1f;
            },
            &outputs, &m_metrics);
        finalLease.reset();
        if (m_broker) {
            const auto base = m_broker->baselineRam();
            m_broker->restoreLimit(base, [&] {
                return activeGuard().updateMemoryLimits(base + m_broker->persistentMB());
            }, activeGuard().getCurrentMemoryMB());
            if (m_broker->snapshot().admissionsBlocked)
                log("[WARNING] " + m_broker->snapshot().blockReason);
        }
        if (!success)
            throw std::runtime_error("Final assembly failed; see the log");
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_outputs = outputs;
            m_progress = 1;
            m_eta = 0;
        }
        if (m_retainedGroups || m_searchInterrupted) {
            if (!checkpoint()) log("[WARNING] Published output; session checkpoint failed. Workspace retained.");
            log("[Session] Published output separately; durable search results retained for resume.");
        } else {
            m_sessions.clearSession(m_run.sessionId);
            cleanupWorkspace();
        }
        m_requestedSession.clear();
        m_publicationOutcome = win->reuseInputArchive ? "OriginalPublished" : "CandidatePublished";
        changeState(EngineState::Completed);
        saveRunReport(Diagnostics::logDirectory() / ("report-" + m_run.sessionId + ".json"));
    } catch (const std::exception &e) {
        m_cv.notify_all();
        for (auto &worker : m_workers)
            if (worker.joinable()) {
                m_cancel = true;
                worker.join();
            }
        m_workers.clear();
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_error = Diagnostics::redact(e.what(), m_run.settings.password);
            m_structuredError.phase = m_state;
            m_structuredError.category = Diagnostics::classify(e);
            m_structuredError.message = m_error;
            if (m_best.taskId <= 0 && !m_results.empty()) {
                const auto failed =
                    std::find_if(m_results.rbegin(), m_results.rend(), [](const auto &result) {
                        return result.status == CandidateStatus::Failed ||
                               result.status == CandidateStatus::OOM_Terminated;
                    });
                if (failed != m_results.rend()) {
                    m_structuredError.category = failed->errorCode;
                    m_structuredError.exitCode = failed->toolExitCode;
                    m_structuredError.win32Error = failed->win32Error;
                    m_structuredError.tool = pathUtf8(
                        ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir));
                }
            }
        }
        log(std::string("[ERROR] ") + e.what());
        bool verifiedCurrentLeader;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            verifiedCurrentLeader = m_best.taskId > 0 && m_best.verified;
        }
        if (verifiedCurrentLeader)
            checkpoint();
        bool preserveWorkspace;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            // Keep a verified current artifact even if this checkpoint write failed,
            // and retain any earlier durable journal when Disk fallback won later.
            preserveWorkspace = verifiedCurrentLeader || m_hasDurableCheckpoint || m_retainedGroups;
        }
        if (!preserveWorkspace) {
            m_sessions.clearSession(m_run.sessionId);
            cleanupWorkspace();
        }
        changeState(m_cancel ? EngineState::Cancelled : EngineState::Failed);
        if (!m_run.sessionId.empty())
            saveRunReport(Diagnostics::logDirectory() / ("report-" + m_run.sessionId + ".json"));
    }
}
void OptimizerEngine::workerFunc(uint32_t index) {
    index += m_workerOffset;
    while (true) {
        std::shared_ptr<AdmissionBroker::Lease> lease;
        std::string admissionKey;
        const auto waitingSince = std::chrono::steady_clock::now();
        size_t selected = m_tasks.size();
        {
            std::unique_lock<std::mutex> l(m_mutex);
            auto select = [&] {
                if (m_active >= static_cast<int>(m_executorTarget) && !m_startNextPending) {
                    selected = m_tasks.size(); return false;
                }
                const auto limit = std::min(m_results.size(), m_admissionLimit);
                const auto admit = [&](size_t candidate) {
                    if (m_run.settings.fullAttemptLimit && m_fullAttemptsStarted >= m_run.settings.fullAttemptLimit &&
                        !m_tasks[candidate].reuseInputArchive && !m_tasks[candidate].groupedSearch) return false;
                    selected = candidate;
                    if (m_tasks[candidate].groupedSearch || !m_broker) return true;
                    admissionKey = m_run.sessionId + ":" + m_tasks[candidate].configId;
                    auto pending = m_overrideLeases.find(admissionKey);
                    if (pending != m_overrideLeases.end()) {
                        lease = std::move(pending->second);
                        m_overrideLeases.erase(pending);
                    } else {
                        const auto tokens = m_results[candidate].retryCount ? m_broker->snapshot().cpuLimit : m_tasks[candidate].cpuTokens;
                        lease = m_broker->tryAcquire(admissionKey, m_tasks[candidate].estimatedRamMB, tokens,
                            !m_tasks[candidate].ramCandidate);
                    }
                    if (lease) return true;
                    selected = m_tasks.size();
                    return false;
                };
                // An explicitly admitted override owns resources already. Dispatch its owner first.
                for (size_t i = 0; i < limit; ++i)
                    if (m_results[i].status == CandidateStatus::Queued &&
                        m_overrideLeases.contains(m_run.sessionId + ":" + m_tasks[i].configId) &&
                        m_scheduler.fits(m_tasks[i].estimatedRamMB, m_tasks[i].cpuTokens))
                        return admit(i);
                const auto preferred = m_scheduler.select(m_tasks, m_results, limit);
                if (preferred != m_tasks.size() && admit(preferred)) return true;
                // Global pressure can reject a locally fitting head. The broker bounds backfill.
                for (size_t i = 0; i < limit; ++i)
                    if (i != preferred && m_results[i].status == CandidateStatus::Queued &&
                        m_scheduler.fits(m_tasks[i].estimatedRamMB, m_tasks[i].cpuTokens) && admit(i))
                        return true;
                return selected != m_tasks.size();
            };
            m_cv.wait_for(l, std::chrono::milliseconds(100), [&] {
                if (!m_cancel && !m_finish && m_best.taskId > 0 && m_run.settings.timeBudgetSeconds &&
                    std::chrono::steady_clock::now() - m_run.start >=
                        std::chrono::seconds(m_run.settings.timeBudgetSeconds)) {
                    m_stopReason = "Time budget reached after a restorable leader";
                    m_searchInterrupted = true;
                    m_stopAction = "TimeBudget";
                    m_finish = true; markSearchClosed();
                    for (auto &token : m_taskCancel)
                        *token = true;
                }
                if (!m_cancel && !m_finish && m_run.settings.fullAttemptLimit &&
                    m_fullAttemptsStarted >= m_run.settings.fullAttemptLimit) {
                    m_searchInterrupted = true;
                    m_stopAction = "FullAttemptBudget";
                    m_stopReason = "Explicit full-attempt budget reached; active attempts may finish";
                    // Originals and orchestration do not consume a full-data attempt.
                    // A spent leaf budget must not suppress a still-queued group search.
                    if (m_cancel || m_finish) return true;
                    if (m_pause) return false;
                    select();
                    return true;
                }
                if (!m_cancel && !m_finish && m_broker && m_broker->snapshot().admissionsBlocked) {
                    m_searchInterrupted = true;
                    m_stopAction = "ResourceGuardFailure";
                    m_stopReason = m_broker->snapshot().blockReason;
                    return true;
                }
                if (m_cancel || m_finish)
                    return true;
                bool queued =
                    std::any_of(m_results.begin(),
                                m_results.begin() + std::min(m_results.size(), m_admissionLimit),
                                [](const auto &r) { return r.status == CandidateStatus::Queued; });
                if (!queued)
                    return true;
                return !m_pause && select();
            });
            if (m_cancel || m_finish) {
                m_overrideLeases.clear();
                if (m_broker)
                    for (const auto &task : m_tasks) m_broker->withdraw(m_run.sessionId + ":" + task.configId);
                return;
            }
            if (selected == m_tasks.size()) {
                if (m_broker && m_broker->snapshot().admissionsBlocked) { m_overrideLeases.clear(); return; }
                if (m_run.settings.fullAttemptLimit && m_fullAttemptsStarted >= m_run.settings.fullAttemptLimit) {
                    m_overrideLeases.clear();
                    return;
                }
                if (std::any_of(m_results.begin(),
                                m_results.begin() + std::min(m_results.size(), m_admissionLimit),
                                [](const auto &r) { return r.status == CandidateStatus::Queued; }))
                    continue;
                return;
            }
            m_scheduler.reserve(m_tasks[selected].estimatedRamMB, m_tasks[selected].cpuTokens);
            if (m_active >= static_cast<int>(m_executorTarget)) m_startNextPending = false;
            m_reservedMemory = m_scheduler.ramReserved;
            ++m_active;
            if (!m_tasks[selected].reuseInputArchive && !m_tasks[selected].groupedSearch) ++m_fullAttemptsStarted;
            m_results[selected].status = CandidateStatus::Running;
            m_results[selected].stage = m_tasks[selected].groupedSearch ? "GroupedSearch" : "Compress";
            m_taskStarts[m_tasks[selected].taskId] = std::chrono::steady_clock::now();
        }
        const auto &task = m_tasks[selected];
        const auto admittedRam = lease ? lease->ram : uint64_t(0);
        const auto admittedCpu = lease ? lease->cpu : uint32_t(0);
        CandidateResult result;
        result.taskId = task.taskId;
        result.configId = task.configId;
        result.name = task.name;
        result.flagsStr = redactFlags(task.flags);
        result.originalBytes = m_summary.totalBytes;
        result.retryCount = m_results[selected].retryCount;
        result.memoryOverride = m_broker && task.estimatedRamMB > m_broker->baselineRam();
        result.admissionOrigin = result.memoryOverride ? "ExplicitRamAllowance" : result.retryCount ? "IsolatedAllocationRetry" : "Automatic";
        for (auto evidence = m_probeEvidence.rbegin(); evidence != m_probeEvidence.rend(); ++evidence)
            if (evidence->value("configId", "") == result.configId) {
                result.sampleCoveredBytes = evidence->value("coveredBytes", uint64_t(0));
                result.sampleTotalBytes = evidence->value("totalBytes", uint64_t(0));
                break;
            }
        result.admissionOrigin = result.memoryOverride ? "ExplicitRamAllowance" : result.retryCount ? "IsolatedAllocationRetry" : "Automatic";
        for (auto evidence = m_probeEvidence.rbegin(); evidence != m_probeEvidence.rend(); ++evidence)
            if (evidence->value("configId", "") == result.configId) {
                result.sampleCoveredBytes = evidence->value("coveredBytes", uint64_t(0));
                result.sampleTotalBytes = evidence->value("totalBytes", uint64_t(0));
                break;
            }
        result.waitSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - waitingSince).count();
        auto archive = m_run.workspace / ("cand_" + std::to_string(task.taskId) + ".7z");
        std::error_code ec;
        std::shared_ptr<RamArtifact> ramArtifact;
        try {
            std::vector<std::string> args = {"a",
                                             "-y",
                                             "-t7z",
                                             "-scsUTF-8",
                                             "-sccUTF-8",
                                             "-spd",
                                             "-w" + pathUtf8(m_run.workspace)};
            if (task.reuseInputArchive) {
                fs::copy_file(m_run.input, archive);
                args = {"t", "-t7z", "-y", "-mmt1", "-sccUTF-8", pathUtf8(archive)};
            } else {
                args.insert(args.end(), task.flags.begin(), task.flags.end());
                args.push_back(pathUtf8(archive));
                args.push_back("@" + pathUtf8(m_run.list));
            }
            auto root = fs::is_directory(m_run.effectiveInput) ? m_run.effectiveInput
                                                               : m_run.effectiveInput.parent_path();
            ProcessControl control{m_taskCancel[selected].get(),
                                   m_run.settings.processTimeoutSeconds};
            control.lifecycleCallback = [this, id = task.configId](uint32_t pid, const char *action) {
                event("Compress", id, pid, action);
            };
            log("[Candidate start] " + task.name + " / " + task.configId);
            StageMetrics::Scope encode(&m_metrics, task.groupedSearch ? "GroupSearchAndAssembly" :
                task.reuseInputArchive ? "OriginalArchiveTest" : "CandidateEncoding");
            ProcessResult r;
            if (task.groupedSearch) {
                auto started = std::chrono::steady_clock::now();
                r.success = runGroupedSearch(archive, index, control, r, task.similarityPartition);
                r.exitCode = r.success ? 0 : 1;
                r.cancelled = m_cancel || m_taskCancel[selected]->load();
                r.durationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            } else {
                bool disk = true;
                if (task.ramCandidate) {
                    auto ram = RamCandidate::run(task, m_summary, root, m_run.settings,
                        m_ramBudget, &activeGuard(), lease ? lease->workerIndex : index, &m_cancel, &control);
                    if (ram.fallbackReason.empty()) {
                        r = std::move(ram.process);
                        ramArtifact = std::move(ram.artifact);
                        disk = false;
                        result.storage = "ram";
                        result.contentVerified = r.success;
                        if (ramArtifact) result.ramBufferBytes = ramArtifact->capacity;
                    } else result.storageFallback = ram.fallbackReason;
                } else if (m_run.settings.enableRamCandidates)
                    result.storageFallback = "Group or candidate reservation exceeds RAM policy";
                if (disk) {
                    if (!acquireDisk(lease, m_taskCancel[selected].get()))
                        throw std::runtime_error("Cancelled or blocked while admitting disk fallback");
                    std::lock_guard materializeLock(m_materializeMutex);
                    for (const auto &file : m_summary.files)
                        if (RamInput::materialize(file)) m_materializedInputBytes += file.sizeBytes;
                }
                if (disk) {
                    r = ProcessManager::execute(
                    pathUtf8(ProcessManager::resolveExecutable("7z", m_run.settings.toolsBinDir)), args,
                    root, &activeGuard(), lease ? lease->workerIndex : index, &m_cancel, nullptr, &control);
                }
            }
            const auto capture = [&](const char *stream, const std::string &output) {
                constexpr size_t maxBytes = 1024 * 1024;
                auto text = Diagnostics::redact(output, m_run.settings.password);
                const bool truncated = text.size() > maxBytes || (std::string(stream) == "stdout" ? r.outputTruncated : r.stderrTruncated);
                if (truncated) text.resize(maxBytes);
                if (truncated) text += "\n[output truncated at 1 MiB]\n";
                const auto directory = m_logPath.parent_path() / (pathUtf8(m_logPath.stem()) + "-artifacts");
                fs::create_directories(directory);
                {
                    std::lock_guard lock(m_logFileMutex);
                    if (m_diagnosticBytes + text.size() > 64ULL * 1024 * 1024)
                        return; // The event log records completion even when stream storage reaches its bound.
                    m_diagnosticBytes += text.size();
                }
                if (!writeAtomically(directory /
                    (task.configId + ".retry-" + std::to_string(result.retryCount) + "." + stream + ".txt"), text))
                    log("[WARNING] Cannot persist candidate process output");
            };
            capture("stdout", r.stdOutput); capture("stderr", r.errOutput);
            encode.finish();
            event("Compress", task.configId, r.pid, r.success ? "Succeeded" : "Failed");
            result.toolExitCode = r.exitCode;
            result.win32Error = r.win32Error;
            result.durationSeconds = r.durationSeconds;
            result.peakRamMB = r.peakMemoryMB;
            result.readBytes = r.readBytes;
            result.writeBytes = r.writeBytes;
            if (r.cancelled) {
                result.status = CandidateStatus::Skipped;
                result.statusMessage = "Cancelled/skipped";
            } else if (r.killedByOOM) {
                result.status = CandidateStatus::OOM_Terminated;
                result.errorCode = ErrorCode::OutOfMemory;
                result.statusMessage = "Allocation failure; cause not established. Job committed " +
                    std::to_string(activeGuard().getCurrentMemoryMB()) + " MiB; Job limit " +
                    std::to_string(activeGuard().getMemoryLimitMB()) + " MiB; candidate reservation " +
                    std::to_string(task.estimatedRamMB) + " MiB; system headroom " +
                    (ResourceGuard::hasMemoryHeadroom(task.estimatedRamMB) ? "available" : "insufficient or unknown");
            } else if (!r.success) {
                result.status = CandidateStatus::Failed;
                result.errorCode = r.timedOut            ? ErrorCode::Timeout
                                   : r.win32Error == 5   ? ErrorCode::PermissionDenied
                                   : r.win32Error == 2   ? ErrorCode::ToolNotFound
                                   : r.win32Error == 112 ? ErrorCode::DiskFull
                                                         : ErrorCode::ProcessCrash;
                result.statusMessage = r.errOutput;
            } else {
                result.outputBytes = ramArtifact ? ramArtifact->size : fs::file_size(archive);
                if (!ramArtifact) {
                    result.tempArchivePath = archive;
                    result.temporaryFileBytes = result.outputBytes;
                }
                result.compressionRatio =
                    result.originalBytes ? 100.0 * result.outputBytes / result.originalBytes : 0;
                std::string error;
                bool discardBySize = false;
                {
                    std::lock_guard lock(m_mutex);
                    discardBySize = !ramArtifact && m_run.settings.immediateCleanInferior &&
                        m_run.settings.selectionGoal == SelectionGoal::MinimumSize && m_best.verified &&
                        !m_best.tempArchivePath.empty() && result.outputBytes > m_best.outputBytes;
                }
                { std::lock_guard lock(m_mutex); m_results[selected].stage = "Verify"; }
                StageMetrics::Scope verifyTiming(&m_metrics, "CandidateVerification");
                auto verificationStart = std::chrono::steady_clock::now();
                control.lifecycleCallback = [this, id = task.configId](uint32_t pid, const char *action) {
                    event("Verify", id, pid, action);
                };
                if (discardBySize) {
                    result.status = CandidateStatus::DiscardedBySize;
                    result.statusMessage = "Larger than durable verified leader; not verified or restorable";
                // The preserved-original execution already ran 7z t successfully.
                // Avoid repeating that identical CRC pass; full SHA verification below remains mandatory.
                } else if (!ramArtifact && !task.reuseInputArchive && m_run.settings.verifyArchives &&
                    !ArchiveAssembler::verifyArchive(
                        archive, m_run.settings.toolsBinDir, error, m_run.settings.password,
                        &m_cancel, &activeGuard(), m_run.settings.processTimeoutSeconds, &control)) {
                    result.status = (m_cancel || m_taskCancel[selected]->load())
                                        ? CandidateStatus::Skipped
                                        : CandidateStatus::Failed;
                    result.errorCode = ErrorCode::InvalidArchive;
                    result.statusMessage = error;
                } else {
                    result.verified = ramArtifact || m_run.settings.verifyArchives;
                    result.status =
                        result.verified ? CandidateStatus::Verified : CandidateStatus::Passed;
                    if (!ramArtifact && result.verified) {
                        AssemblyPlan verification;
                        verification.inputSourcePath = m_run.effectiveInput;
                        verification.password = m_run.settings.password;
                        const auto content = StreamingVerifier::verify(archive, verification, m_summary,
                            m_run.settings, &activeGuard(), &m_cancel, error, &control);
                        result.contentVerified = content == StreamingVerifier::Result::Verified;
                        if (content == StreamingVerifier::Result::Failed) {
                            result.status = (m_cancel || m_taskCancel[selected]->load()) ?
                                CandidateStatus::Skipped : CandidateStatus::Failed;
                            result.verified = false;
                            result.errorCode = ErrorCode::InvalidArchive;
                            result.statusMessage = "Content verification: " + error;
                        } else if (content == StreamingVerifier::Result::Unavailable)
                            result.statusMessage = "CRC-tested only; full manifest verification required before publication";
                    }
                }
                result.verificationSeconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - verificationStart).count();
            }
        } catch (const std::exception &e) {
            result.status = (m_cancel || m_taskCancel[selected]->load()) ? CandidateStatus::Skipped : CandidateStatus::Failed;
            result.errorCode = Diagnostics::classify(e);
            result.statusMessage = e.what();
        }
        result.statusMessage = Diagnostics::redact(result.statusMessage, m_run.settings.password);

        uint64_t scratchBytes = 0;
        try {
            for (const auto &entry : fs::recursive_directory_iterator(m_run.workspace))
                if (entry.is_regular_file())
                    scratchBytes += entry.file_size();
        } catch (...) {
        }
        // RAM encoding uses no disk execution slot. Obtain one before any winner/keep-all materialization.
        if (ramArtifact && !acquireDisk(lease, m_taskCancel[selected].get())) {
            result.status = (m_cancel || m_taskCancel[selected]->load()) ? CandidateStatus::Skipped : CandidateStatus::Failed;
            result.verified = false;
            result.statusMessage = "RAM materialization admission cancelled or blocked";
        }
        bool newLeader = false;
        fs::path obsoleteLeader;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_peakScratchBytes = std::max(m_peakScratchBytes, scratchBytes);
            bool passed = result.status == CandidateStatus::Verified ||
                          result.status == CandidateStatus::Passed;
            bool chooseSpeed = m_run.settings.selectionGoal == SelectionGoal::FastWithinTolerance;
            auto prospectiveSmallest = passed ? std::min(m_smallestBytes, result.outputBytes) : m_smallestBytes;
            double allowed = static_cast<double>(prospectiveSmallest) *
                             (1.0 + m_run.settings.sizeTolerancePercent / 100.0);
            bool speedLeader = chooseSpeed && result.outputBytes <= allowed &&
                               (m_best.taskId <= 0 || m_best.outputBytes > allowed ||
                                result.durationSeconds < m_best.durationSeconds);
            bool wouldLead = passed && m_manualWinner.empty() &&
                (speedLeader || (!chooseSpeed && (m_best.taskId <= 0 || result.outputBytes < m_best.outputBytes ||
                (result.outputBytes == m_best.outputBytes && result.durationSeconds < m_best.durationSeconds))));
            // Retained/manual/speed candidates need a durable artifact too. Normal
            // MinimumSize losers are discarded straight from RAM without a file.
            if (ramArtifact && passed && (wouldLead || chooseSpeed || !m_run.settings.immediateCleanInferior)) {
                try {
                    ramArtifact->materialize(archive);
                    result.tempArchivePath = archive;
                    result.temporaryFileBytes = result.outputBytes;
                } catch (const std::exception &e) {
                    passed = false;
                    result.status = CandidateStatus::Failed;
                    result.errorCode = Diagnostics::classify(e);
                    result.statusMessage = Diagnostics::redact(e.what(), m_run.settings.password);
                }
            }
            if (passed) m_smallestBytes = prospectiveSmallest;
            if (passed && m_manualWinner.empty() &&
                (speedLeader ||
                 (!chooseSpeed && (m_best.taskId <= 0 || result.outputBytes < m_best.outputBytes ||
                                   (result.outputBytes == m_best.outputBytes &&
                                    result.durationSeconds < m_best.durationSeconds))))) {
                if (m_best.taskId > 0) {
                    for (auto &old : m_results)
                        if (old.taskId == m_best.taskId) {
                            old.isLeader = false;
                            if ((!old.contentVerified || result.contentVerified) && m_run.settings.immediateCleanInferior &&
                                m_run.settings.selectionGoal == SelectionGoal::MinimumSize) {
                                obsoleteLeader = old.tempArchivePath;
                                old.tempArchivePath.clear();
                            }
                        }
                }
                result.leaderGainBytes = m_best.outputBytes > result.outputBytes ? m_best.outputBytes - result.outputBytes : 0;
                result.leaderGainBytes = m_best.outputBytes > result.outputBytes ? m_best.outputBytes - result.outputBytes : 0;
                result.isLeader = true;
                m_best = result;
                newLeader = true;
            } else if (!passed || (m_run.settings.immediateCleanInferior &&
                                   m_run.settings.selectionGoal == SelectionGoal::MinimumSize)) {
                fs::remove(archive, ec);
                result.tempArchivePath.clear();
            }
            ramArtifact.reset(); // Release buffers before releasing admission reservation.
            lease.reset();
            if (m_broker) {
                const auto base = m_broker->baselineRam();
                m_broker->restoreLimit(base, [&] {
                    return activeGuard().updateMemoryLimits(base + m_broker->persistentMB());
                }, activeGuard().getCurrentMemoryMB());
                m_scheduler.ramLimit = std::max(m_broker->snapshot().ramLimit, m_scheduler.ramReserved);
            }
            result.stage = "Finished";
            m_results[selected] = result;
            m_lastCandidateFinished = std::chrono::steady_clock::now();
            m_taskStarts.erase(task.taskId);
            m_scheduler.release(task.estimatedRamMB, task.cpuTokens);
            m_reservedMemory = m_scheduler.ramReserved;
            --m_active;
            ++m_completed;
            if (result.status == CandidateStatus::OOM_Terminated && !result.retryCount && !m_cancel && !m_finish) {
                auto increased = task.estimatedRamMB + task.estimatedRamMB / 4 + 32;
                if (increased <= m_scheduler.ramLimit && ResourceGuard::hasMemoryHeadroom(increased)) {
                    m_tasks[selected].estimatedRamMB = increased;
                    m_results[selected].status = CandidateStatus::Queued;
                    m_results[selected].retryCount = 1;
                    m_results[selected].statusMessage += "; one isolated retry queued with increased reservation";
                    --m_completed;
                }
            }
            m_durationSum += result.durationSeconds;
            m_progress = std::max(m_progress, 0.9f * m_completed / m_results.size());
            m_eta = 0;
            for (size_t i = 0; i < m_results.size(); ++i) {
                if (m_results[i].status != CandidateStatus::Queued &&
                    m_results[i].status != CandidateStatus::Running) continue;
                double cost = m_predictedSeconds[m_tasks[i].taskId];
                auto started = m_taskStarts.find(m_tasks[i].taskId);
                if (started != m_taskStarts.end())
                    cost = std::max(0.0, cost - std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started->second).count());
                m_eta += cost;
            }
            m_eta /= std::max(1u, m_run.settings.workerThreads);
        }
        event("Candidate", task.configId, 0, "Finished",
            {{"status", static_cast<int>(result.status)}, {"spec", result.flagsStr},
             {"storage", result.storage}, {"reservedRamMB", admittedRam},
             {"cpuTokens", admittedCpu}, {"admission", result.admissionOrigin},
             {"retry", result.retryCount}, {"gainBytes", result.leaderGainBytes},
             {"error", result.statusMessage}});
        m_cv.notify_all();
        if (newLeader)
            log("[Leader] " + result.name + ": " + std::to_string(result.outputBytes) + " bytes");
        std::function<void(const CandidateResult &)> candidateCb;
        std::function<void(float, int, int)> progressCb;
        {
            std::lock_guard<std::mutex> l(m_callbackMutex);
            candidateCb = m_candidateCallback;
            progressCb = m_progressCallback;
        }
        if (candidateCb)
            try {
                candidateCb(result);
            } catch (...) {
            }
        if (progressCb)
            try {
                progressCb(getProgress(), getCompletedCount(), getTotalCount());
            } catch (...) {
            }
        // Persist only restorable leaders. An unchecked candidate remains usable for final
        // assembly.
        const bool durableCheckpoint = (m_run.settings.verifyArchives || result.verified) && checkpoint();
        if (!obsoleteLeader.empty() && durableCheckpoint)
            fs::remove(obsoleteLeader, ec);
    }
}
} // namespace UltraArch
