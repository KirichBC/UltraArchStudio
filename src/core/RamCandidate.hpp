#pragma once
#include "ContentAnalyzer.hpp"
#include "ProcessManager.hpp"
#include "RamPolicy.hpp"
#include <memory>
#include <mutex>
namespace UltraArch {
class RamBudget {
    uint64_t limit_, used_ = 0, peak_ = 0;
    mutable std::mutex mutex_;
  public:
    explicit RamBudget(uint64_t bytes) : limit_(bytes) {}
    bool acquire(uint64_t bytes) {
        std::lock_guard lock(mutex_);
        if (bytes > limit_ - used_) return false;
        used_ += bytes;
        if (used_ > peak_) peak_ = used_;
        return true;
    }
    void release(uint64_t bytes) {
        std::lock_guard lock(mutex_);
        used_ -= bytes;
    }
    uint64_t peak() const { std::lock_guard lock(mutex_); return peak_; }
    uint64_t used() const { std::lock_guard lock(mutex_); return used_; }
};
class RamArtifact {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    uint64_t size = 0, capacity = 0;
    RamArtifact();
    ~RamArtifact();
    RamArtifact(const RamArtifact &) = delete;
    RamArtifact &operator=(const RamArtifact &) = delete;
    void materialize(const std::filesystem::path &path) const;
    static std::shared_ptr<RamArtifact> allocate(const std::shared_ptr<RamBudget> &budget,
                                                uint64_t reserved, uint64_t capacity);
    const uint8_t *data() const;
    void write(uint64_t offset, const void *source, uint64_t bytes);
    const std::string &mappingName() const;
    friend class RamCandidate;
};
struct RamCandidateResult {
    ProcessResult process;
    std::shared_ptr<RamArtifact> artifact;
    std::string fallbackReason;
};
class RamCandidate {
  public:
    static RamCandidateResult run(const TaskConfig &task, const AnalysisSummary &summary,
                                  const std::filesystem::path &root,
                                  const OptimizerSettings &settings,
                                  const std::shared_ptr<RamBudget> &budget,
                                  ResourceGuard *guard, uint32_t worker,
                                  const std::atomic<bool> *cancel,
                                  const ProcessControl *control);
};
} // namespace UltraArch
