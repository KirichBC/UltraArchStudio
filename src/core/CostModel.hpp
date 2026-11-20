#pragma once
#include "SettingsJson.hpp"
#include "ContentAnalyzer.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
namespace UltraArch {
// Scheduling estimates only: observations never decide candidate eligibility.
// configId is the canonical typed graph identity, including dictionaries,
// contexts, solid/order and thread count. Backend hash includes its DLL.
class CostModel {
    Json records_ = Json::object();
    std::filesystem::path path_;
    std::string backend_;
    static constexpr double maximumSeconds = 7.0 * 24 * 3600;
    static constexpr uint64_t maximumBytes = uint64_t(1) << 44;
    static unsigned bucket(uint64_t bytes) {
        return bytes ? 64 - std::countl_zero(bytes) : 0;
    }
    std::string prefix(FileCategory category, const std::string &configuration) const {
        return backend_ + ":" + std::to_string(static_cast<int>(category)) + ":" + configuration + ":";
    }
    static bool validRecord(const Json &record) {
        if (!record.is_object() || !record.contains("bytes") || !record["bytes"].is_number_unsigned() ||
            !record.contains("seconds") || !record["seconds"].is_number())
            return false;
        try {
            const auto bytes = record["bytes"].get<uint64_t>();
            const auto seconds = record["seconds"].get<double>();
            return bytes && bytes <= maximumBytes && std::isfinite(seconds) && seconds > 0 &&
                   seconds <= maximumSeconds;
        } catch (...) { return false; }
    }
    static double fallback(const AnalysisSummary &summary, const TaskConfig &task) {
        const double mib = static_cast<double>(summary.totalBytes) / 1048576;
        if (task.reuseInputArchive) return std::clamp(0.05 + mib / 160, 0.01, maximumSeconds);
        if (task.groupedSearch) return std::clamp(2.0 + mib / 4, 0.01, maximumSeconds);
        double secondsPerMiB = 1.0 / 64;
        double allocation = 0;
        for (const auto &method : task.spec.methods) {
            if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2) {
                secondsPerMiB = std::max(secondsPerMiB, 1.0 / 12);
                allocation += static_cast<double>(method.dictionary.value) / (256.0 * 1048576);
            } else if (method.codec == Codec::PPMd)
                secondsPerMiB = std::max(secondsPerMiB, (1.0 + method.order / 16.0) / 8);
            else if (method.codec == Codec::BZip2)
                secondsPerMiB = std::max(secondsPerMiB, 1.0 / 10);
        }
        if (!task.spec.methods.empty() && task.spec.methods.front().codec == Codec::BCJ2)
            secondsPerMiB *= 1.6;
        // Sublinear CPU scaling avoids predicting ideal speedup from thread count.
        const auto threads = std::max(1u, std::min(task.spec.threads, 8u));
        return std::clamp(0.1 + allocation + mib * secondsPerMiB / std::sqrt(threads), 0.01, maximumSeconds);
    }
  public:
    CostModel(const std::filesystem::path &path, std::string backendHash)
        : path_(path), backend_(std::move(backendHash)) {
        try {
            if (std::filesystem::file_size(path_) > 4 * 1048576) return;
            std::ifstream in(path_); Json data; in >> data;
            if (!data.is_object() || data.value("schema", 0) != 1 ||
                !data.contains("records") || !data["records"].is_object()) return;
            for (auto it = data["records"].begin(); it != data["records"].end(); ++it)
                if (it.key().size() <= 1024 && validRecord(it.value()) && records_.size() < 8192)
                    records_[it.key()] = it.value();
        } catch (...) { records_ = Json::object(); }
    }
    double estimateSeconds(const AnalysisSummary &summary, const TaskConfig &task) const {
        if (!summary.totalBytes || summary.totalBytes > maximumBytes || task.configId.empty())
            return fallback(summary, task);
        const auto keyPrefix = prefix(summary.dominantCategory, task.configId);
        const auto inputBucket = bucket(summary.totalBytes);
        const Json *nearest = nullptr;
        unsigned distance = std::numeric_limits<unsigned>::max();
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            if (it.key().rfind(keyPrefix, 0) != 0) continue;
            const auto measuredBytes = it.value()["bytes"].get<uint64_t>();
            const auto measuredBucket = bucket(measuredBytes);
            const unsigned candidateDistance = inputBucket > measuredBucket
                ? inputBucket - measuredBucket : measuredBucket - inputBucket;
            if (candidateDistance > 2 || candidateDistance >= distance) continue;
            const double ratio = static_cast<double>(summary.totalBytes) / measuredBytes;
            if (ratio < 0.25 || ratio > 4.0) continue;
            distance = candidateDistance; nearest = &it.value();
        }
        if (!nearest) return fallback(summary, task);
        return std::clamp(nearest->at("seconds").get<double>() *
                          (static_cast<double>(summary.totalBytes) / nearest->at("bytes").get<uint64_t>()),
                          0.01, maximumSeconds);
    }
    void record(const AnalysisSummary &summary, const std::vector<CandidateResult> &results) {
        for (const auto &result : results) {
            if ((result.status != CandidateStatus::Verified && result.status != CandidateStatus::Passed) ||
                result.configId.empty() || result.configId.size() > 512 ||
                !std::isfinite(result.durationSeconds) || result.durationSeconds <= 0 ||
                result.durationSeconds > maximumSeconds)
                continue;
            const auto bytes = result.originalBytes ? result.originalBytes : summary.totalBytes;
            if (!bytes || bytes > maximumBytes) continue;
            const auto key = prefix(summary.dominantCategory, result.configId) + std::to_string(bucket(bytes));
            double seconds = result.durationSeconds;
            if (records_.contains(key)) {
                const auto &old = records_[key];
                const auto oldSeconds = old["seconds"].get<double>() *
                    (static_cast<double>(bytes) / old["bytes"].get<uint64_t>());
                seconds = std::clamp(oldSeconds * 0.5 + seconds * 0.5, 0.01, maximumSeconds);
            }
            if (!records_.contains(key) && records_.size() >= 8192) continue;
            records_[key] = Json{{"bytes", bytes}, {"seconds", seconds}};
        }
    }
    bool save() const {
        return writeAtomically(path_, Json{{"schema", 1}, {"records", records_}}.dump(2));
    }
};
} // namespace UltraArch
