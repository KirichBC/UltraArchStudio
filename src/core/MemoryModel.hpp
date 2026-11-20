#pragma once
#include "SettingsJson.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
namespace UltraArch {
class MemoryModel {
    Json data_ = Json::object();
    std::filesystem::path path_;
    std::string prefix_;

  public:
    MemoryModel(const std::filesystem::path &path, std::string toolHash, FileCategory category,
                uint64_t bytes)
        : path_(path) {
        prefix_ = std::move(toolHash) + ":" + std::to_string(static_cast<int>(category)) + ":" +
                  std::to_string(bytes ? 64 - std::countl_zero(bytes) : 0) + ":";
        try {
            if (std::filesystem::file_size(path) < 4 * 1048576) {
                std::ifstream in(path);
                in >> data_;
            }
        } catch (...) {
        }
        if (!data_.is_object())
            data_ = Json::object();
    }
    void reserve(std::vector<TaskConfig> &tasks, uint64_t ramLimit) const {
        for (auto &task : tasks) {
            auto key = prefix_ + task.configId;
            if (data_.contains(key) && data_[key].is_number_unsigned()) {
                auto observed = data_[key].get<uint64_t>();
                task.estimatedRamMB = std::max(task.estimatedRamMB, observed > UINT64_MAX - observed / 4 - 32 ? UINT64_MAX : observed + observed / 4 + 32);
            }
        }
        for (auto &task : tasks) task.resourceBlocked = task.estimatedRamMB > ramLimit;
    }
    void save(const std::vector<CandidateResult> &results) {
        for (const auto &result : results)
            if (result.peakRamMB && result.status != CandidateStatus::OOM_Terminated) {
                auto key = prefix_ + result.configId;
                uint64_t previous = 0;
                if (data_.contains(key) && data_[key].is_number_unsigned())
                    previous = data_[key].get<uint64_t>();
                data_[key] = std::max(previous, result.peakRamMB);
            }
        writeAtomically(path_, data_.dump(2));
    }
};
} // namespace UltraArch
