#pragma once
#include "Types.hpp"
#include <cmath>
#include <nlohmann/json.hpp>

namespace UltraArch {
using Json = nlohmann::json;
inline OptimizerSettings normalizeSearchSettings(OptimizerSettings settings) {
    // Migrate the legacy adaptive toggle into the explicit search mode.
    // Explicit Exhaustive is authoritative. Legacy toggles cannot silently prune it.
    settings.enableAdaptivePruning = false;
    return settings;
}
inline void validateSettings(const OptimizerSettings &s, bool requirePassword = true) {
    if (static_cast<int>(s.verificationMode) < 0 || static_cast<int>(s.verificationMode) > 2 ||
        static_cast<int>(s.searchMode) < 0 || static_cast<int>(s.searchMode) > 2 ||
        static_cast<int>(s.selectionGoal) < 0 || static_cast<int>(s.selectionGoal) > 1 ||
        static_cast<int>(s.contentPolicy) < 0 || static_cast<int>(s.contentPolicy) > 1 ||
        static_cast<int>(s.affinityMode) < 0 || static_cast<int>(s.affinityMode) > 2 ||
        !std::isfinite(s.sizeTolerancePercent) || s.sizeTolerancePercent < 0 ||
        s.sizeTolerancePercent > 10 || s.extractionLimitMB < 1 || s.extractionLimitMB > 1048576 ||
        s.extractionFileLimitMB < 1 || s.extractionFileLimitMB > s.extractionLimitMB ||
        s.extractionFileCount < 1)
        throw std::invalid_argument("Invalid search/selection/extraction policy");
    if (static_cast<int>(s.profile) < 0 || static_cast<int>(s.profile) > 2)
        throw std::invalid_argument("Unknown optimization profile");
    if (s.workerThreads < 1 || s.workerThreads > 64 || s.cpuThreads > 256)
        throw std::invalid_argument("Workers must be 1..64; CPU threads must be 0..256");
    if (!s.probePasses || s.probePasses > 4 || s.fullAttemptLimit > 10000 ||
        !s.probeSampleMB || s.probeSampleMB > 256 || !s.probeBudgetSeconds || s.probeBudgetSeconds > 3600 ||
        !s.probeTimeoutSeconds || s.probeTimeoutSeconds > 600)
        throw std::invalid_argument("Probe sample must be 1..256 MiB; budget 1..3600 s; timeout 1..600 s");
    if (s.cpuLimitPercent > 100 || s.diskTaskLimit > 64)
        throw std::invalid_argument("CPU cap must be 0..100; disk tasks must be 0..64");
    if (s.groupedSearchOnly && (!s.enableGrouping || s.profile != OptimizationProfile::Extreme_RePack))
        throw std::invalid_argument("Group-only search requires Extreme grouping");
    if (s.ramLimitMB < 64 || s.ramLimitMB > 1048576)
        throw std::invalid_argument("RAM limit must be 64..1048576 MiB");
    if (s.maxLzmaDictionaryMB > 1536)
        throw std::invalid_argument("LZMA dictionary ceiling must be 0..1536 MiB");
    if (s.ramGroupLimitMB < 1 || s.ramGroupLimitMB > 256 ||
        s.ramTemporaryBudgetMB < 32 || s.ramTemporaryBudgetMB > 4096)
        throw std::invalid_argument("RAM group must be 1..256 MiB; RAM temporary budget 32..4096 MiB");
    if (s.ramLimitMB < uint64_t(s.workerThreads) * 32)
        throw std::invalid_argument("RAM limit must provide at least 32 MiB per worker");
    if (!std::isfinite(s.pruningThreshold) || s.pruningThreshold < 1.0 ||
        s.pruningThreshold > 10.0 || !std::isfinite(s.quickProbePercent) ||
        s.quickProbePercent <= 0 || s.quickProbePercent > 100)
        throw std::invalid_argument("Invalid sampling/pruning settings");
    if (!s.solidBlockSize.empty() && !validSolid(s.solidBlockSize))
        throw std::invalid_argument("Invalid solid block size");
    if (s.language != "en" && s.language != "ru")
        throw std::invalid_argument("Unknown language");
    if (s.password.find('\0') != std::string::npos ||
        s.password.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("Password cannot contain NUL or line breaks");
    if (requirePassword && s.encryptHeaders && s.password.empty())
        throw std::invalid_argument("Header encryption requires a password");
    if (s.sfxOutput && s.volumeSizeMB)
        throw std::invalid_argument("SFX and split volumes cannot be combined");
    if (s.volumeSizeMB > 1048576)
        throw std::invalid_argument("Volume size is too large");
}
inline Json settingsJson(const OptimizerSettings &s) {
    // Secrets are intentionally excluded from presets and checkpoints.
    return {{"enableGrouping", s.enableGrouping},
            {"enableRamCandidates", s.enableRamCandidates},
            {"ramGroupLimitMB", s.ramGroupLimitMB},
            {"ramTemporaryBudgetMB", s.ramTemporaryBudgetMB},
            {"enableSimilarityGroups", s.enableSimilarityGroups},
            {"groupedSearchOnly", s.groupedSearchOnly},
            {"sortByType", s.sortByType},
            {"cpuLimitPercent", s.cpuLimitPercent}, {"diskTaskLimit", s.diskTaskLimit},
            {"verificationMode", static_cast<int>(s.verificationMode)},
            {"searchMode", static_cast<int>(s.searchMode)},
            {"selectionGoal", static_cast<int>(s.selectionGoal)},
            {"contentPolicy", static_cast<int>(s.contentPolicy)},
            {"affinityMode", static_cast<int>(s.affinityMode)},
            {"backgroundPriority", s.backgroundPriority},
            {"sizeTolerancePercent", s.sizeTolerancePercent},
            {"extractionLimitMB", s.extractionLimitMB},
            {"extractionFileLimitMB", s.extractionFileLimitMB},
            {"extractionFileCount", s.extractionFileCount},
            {"profile", static_cast<int>(s.profile)},
            {"workerThreads", s.workerThreads},
            {"autoCpu", s.autoCpu},
            {"ramLimitMB", s.ramLimitMB},
            {"maxLzmaDictionaryMB", s.maxLzmaDictionaryMB},
            {"verifyArchives", s.verifyArchives},
            {"immediateCleanInferior", s.immediateCleanInferior},
            {"sfxOutput", s.sfxOutput},
            {"toolsBinDir", pathUtf8(s.toolsBinDir)},
            {"customTempDir", pathUtf8(s.customTempDir)},
            {"encryptHeaders", s.encryptHeaders},
            {"volumeSizeMB", s.volumeSizeMB},
            {"solidBlockSize", s.solidBlockSize},
            {"enablePreprocessors", s.enablePreprocessors},
            {"enableAdaptivePruning", s.enableAdaptivePruning},
            {"pruningThreshold", s.pruningThreshold},
            {"quickProbePercent", s.quickProbePercent},
            {"probeSampleMB", s.probeSampleMB}, {"probeBudgetSeconds", s.probeBudgetSeconds},
            {"probeTimeoutSeconds", s.probeTimeoutSeconds},
            {"probePasses", s.probePasses}, {"fullAttemptLimit", s.fullAttemptLimit},
            {"portableMode", s.portableMode},
            {"autoShutdown", s.autoShutdown},
            {"language", s.language},
            {"cpuThreads", s.cpuThreads},
            {"processTimeoutSeconds", s.processTimeoutSeconds},
            {"timeBudgetSeconds", s.timeBudgetSeconds}};
}
inline OptimizerSettings settingsFromJson(const Json &j) {
    if (!j.is_object())
        throw std::invalid_argument("Settings must be an object");
    // Grouping policy participates in the session fingerprint.
    OptimizerSettings s;
    s.profile = static_cast<OptimizationProfile>(j.value("profile", static_cast<int>(s.profile)));
    auto unsignedField = [&](const char *key, uint64_t fallback) {
        if (!j.contains(key))
            return fallback;
        if (!j.at(key).is_number_integer() ||
            (j.at(key).is_number_integer() && !j.at(key).is_number_unsigned() &&
             j.at(key).get<int64_t>() < 0))
            throw std::invalid_argument(std::string("Invalid nonnegative integer: ") + key);
        return j.at(key).get<uint64_t>();
    };
    auto workers = unsignedField("workerThreads", s.workerThreads);
    auto cpu = unsignedField("cpuThreads", s.cpuThreads);
    auto timeout = unsignedField("processTimeoutSeconds", 0);
    auto budget = unsignedField("timeBudgetSeconds", 0);
    if (workers > 64 || cpu > 256 || timeout > UINT32_MAX || budget > UINT32_MAX)
        throw std::invalid_argument("Settings integer out of range");
    s.workerThreads = static_cast<uint32_t>(workers);
    s.autoCpu = j.value("autoCpu", false);
    s.cpuThreads = static_cast<uint32_t>(cpu);
    s.processTimeoutSeconds = static_cast<uint32_t>(timeout);
    s.timeBudgetSeconds = static_cast<uint32_t>(budget);
    s.verificationMode = static_cast<VerificationMode>(j.value("verificationMode", 0));
    // Presets predating the explicit mode preserve their adaptive intent. An explicit mode wins.
    s.searchMode = static_cast<SearchMode>(j.value("searchMode",
        j.value("enableAdaptivePruning", false) ? 1 : 2));
    s.selectionGoal = static_cast<SelectionGoal>(j.value("selectionGoal", 0));
    s.contentPolicy = static_cast<ContentPolicy>(j.value("contentPolicy", 0));
    s.affinityMode = static_cast<AffinityMode>(j.value("affinityMode", 0));
    s.backgroundPriority = j.value("backgroundPriority", false);
    s.sizeTolerancePercent = j.value("sizeTolerancePercent", 1.0);
    s.extractionLimitMB = unsignedField("extractionLimitMB", s.extractionLimitMB);
    s.extractionFileLimitMB = unsignedField("extractionFileLimitMB", s.extractionFileLimitMB);
    auto count = unsignedField("extractionFileCount", s.extractionFileCount);
    if (count > UINT32_MAX)
        throw std::invalid_argument("Extraction count out of range");
    s.extractionFileCount = static_cast<uint32_t>(count);
    s.ramLimitMB = unsignedField("ramLimitMB", s.ramLimitMB);
    s.maxLzmaDictionaryMB = unsignedField("maxLzmaDictionaryMB", 0);
    s.enableRamCandidates = j.value("enableRamCandidates", false);
    s.ramGroupLimitMB = unsignedField("ramGroupLimitMB", 40);
    s.ramTemporaryBudgetMB = unsignedField("ramTemporaryBudgetMB", 1024);
    s.volumeSizeMB = unsignedField("volumeSizeMB", 0);
    s.enableGrouping = j.value("enableGrouping", s.enableGrouping);
    s.enableSimilarityGroups = j.value("enableSimilarityGroups", false);
    s.groupedSearchOnly = j.value("groupedSearchOnly", false);
    s.sortByType = j.value("sortByType", false);
    auto cpuLimit = unsignedField("cpuLimitPercent", 0);
    auto diskLimit = unsignedField("diskTaskLimit", 0);
    if (cpuLimit > 100 || diskLimit > 64)
        throw std::invalid_argument("Invalid background limits");
    s.cpuLimitPercent = static_cast<uint32_t>(cpuLimit);
    s.diskTaskLimit = static_cast<uint32_t>(diskLimit);
    s.verifyArchives = j.value("verifyArchives", s.verifyArchives);
    s.immediateCleanInferior = j.value("immediateCleanInferior", s.immediateCleanInferior);
    s.sfxOutput = j.value("sfxOutput", s.sfxOutput);
    s.toolsBinDir = utf8Path(j.value("toolsBinDir", pathUtf8(s.toolsBinDir)));
    s.customTempDir = utf8Path(j.value("customTempDir", std::string{}));
    s.encryptHeaders = j.value("encryptHeaders", s.encryptHeaders);
    s.solidBlockSize = j.value("solidBlockSize", s.solidBlockSize);
    s.enablePreprocessors = j.value("enablePreprocessors", s.enablePreprocessors);
    s.enableAdaptivePruning = j.value("enableAdaptivePruning", s.enableAdaptivePruning);
    s.pruningThreshold = j.value("pruningThreshold", s.pruningThreshold);
    s.quickProbePercent = j.value("quickProbePercent", s.quickProbePercent);
    auto sampleMB = unsignedField("probeSampleMB", 25), probeBudget = unsignedField("probeBudgetSeconds", 60),
         probeTimeout = unsignedField("probeTimeoutSeconds", 30);
    if (sampleMB > 256 || probeBudget > 3600 || probeTimeout > 600)
        throw std::invalid_argument("Probe settings out of range");
    s.probeSampleMB = static_cast<uint32_t>(sampleMB);
    s.probeBudgetSeconds = static_cast<uint32_t>(probeBudget);
    s.probeTimeoutSeconds = static_cast<uint32_t>(probeTimeout);
    auto passes = unsignedField("probePasses", 2), attempts = unsignedField("fullAttemptLimit", 0);
    if (passes > 4 || attempts > 10000) throw std::invalid_argument("Probe passes/attempt limit out of range");
    s.probePasses = static_cast<uint32_t>(passes); s.fullAttemptLimit = static_cast<uint32_t>(attempts);
    s.portableMode = j.value("portableMode", s.portableMode);
    s.autoShutdown = j.value("autoShutdown", s.autoShutdown);
    s.language = j.value("language", s.language);
    validateSettings(s, false);
    return s;
}
inline Json resultJson(const CandidateResult &r) {
    return {{"configId", r.configId},
            {"storage", r.storage}, {"storageFallback", r.storageFallback},
            {"temporaryFileBytes", r.temporaryFileBytes}, {"ramBufferBytes", r.ramBufferBytes},
            {"taskId", r.taskId},
            {"name", r.name}, {"stage", r.stage}, {"admissionOrigin", r.admissionOrigin},
            {"sampleCoveredBytes", r.sampleCoveredBytes}, {"sampleTotalBytes", r.sampleTotalBytes},
            {"leaderGainBytes", r.leaderGainBytes},
            {"flags", r.flagsStr},
            {"outputBytes", r.outputBytes},
            {"originalBytes", r.originalBytes},
            {"ratio", r.compressionRatio},
            {"duration", r.durationSeconds},
            {"verificationSeconds", r.verificationSeconds}, {"waitSeconds", r.waitSeconds},
            {"contentVerified", r.contentVerified},
            {"retryCount", r.retryCount},
            {"memoryOverride", r.memoryOverride},
            {"peakRamMB", r.peakRamMB},
            {"readBytes", r.readBytes},
            {"writeBytes", r.writeBytes},
            {"toolExitCode", r.toolExitCode},
            {"win32Error", r.win32Error},
            {"status", static_cast<int>(r.status)},
            {"error", static_cast<int>(r.errorCode)},
            {"message", r.statusMessage},
            {"verified", r.verified},
            {"isLeader", r.isLeader},
            {"archive", pathUtf8(r.tempArchivePath)}};
}
inline CandidateResult resultFromJson(const Json &j) {
    CandidateResult r;
    r.storage = j.value("storage", std::string("disk"));
    r.storageFallback = j.value("storageFallback", std::string{});
    r.temporaryFileBytes = j.value("temporaryFileBytes", uint64_t(0));
    r.ramBufferBytes = j.value("ramBufferBytes", uint64_t(0));
    r.configId = j.value("configId", std::string{});
    r.taskId = j.at("taskId").get<int>();
    r.name = j.at("name").get<std::string>();
    r.stage = j.value("stage", std::string{"Queued"});
    r.admissionOrigin = j.value("admissionOrigin", std::string{"Automatic"});
    r.sampleCoveredBytes = j.value("sampleCoveredBytes", uint64_t(0));
    r.sampleTotalBytes = j.value("sampleTotalBytes", uint64_t(0));
    r.leaderGainBytes = j.value("leaderGainBytes", uint64_t(0));
    r.flagsStr = j.at("flags").get<std::string>();
    r.outputBytes = j.at("outputBytes").get<uint64_t>();
    r.originalBytes = j.at("originalBytes").get<uint64_t>();
    r.compressionRatio = j.at("ratio").get<double>();
    r.durationSeconds = j.at("duration").get<double>();
    r.verificationSeconds = j.value("verificationSeconds", 0.0);
    r.waitSeconds = j.value("waitSeconds", 0.0);
    r.contentVerified = j.value("contentVerified", false);
    r.retryCount = j.value("retryCount", 0u);
    r.memoryOverride = j.value("memoryOverride", false);
    r.peakRamMB = j.at("peakRamMB").get<uint64_t>();
    r.readBytes = j.value("readBytes", uint64_t(0));
    r.writeBytes = j.value("writeBytes", uint64_t(0));
    r.toolExitCode = j.value("toolExitCode", 0);
    r.win32Error = j.value("win32Error", uint32_t(0));
    int status = j.at("status").get<int>();
    if (status < 0 || status > static_cast<int>(CandidateStatus::Deferred))
        throw std::invalid_argument("Invalid candidate status");
    r.status = static_cast<CandidateStatus>(status);
    int error = j.value("error", 0);
    if (error < 0 || error > static_cast<int>(ErrorCode::Unknown))
        throw std::invalid_argument("Invalid candidate error code");
    r.errorCode = static_cast<ErrorCode>(error);
    r.statusMessage = j.value("message", std::string{});
    r.verified = j.at("verified").get<bool>();
    r.isLeader = j.at("isLeader").get<bool>();
    r.tempArchivePath = utf8Path(j.at("archive").get<std::string>());
    return r;
}
} // namespace UltraArch
