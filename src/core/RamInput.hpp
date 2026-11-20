#pragma once
#include "RamCandidate.hpp"
#include "InputManifest.hpp"
namespace UltraArch {
struct RamInputResult {
    InputManifest manifest;
    ProcessResult process;
    uint64_t ramBytes = 0, diskBytes = 0, reservedMB = 0;
    double cpuSeconds = 0;
    bool success = false;
    std::string fallbackReason;
};
class RamInput {
  public:
    static RamInputResult extract(const std::filesystem::path &archive,
        const std::filesystem::path &root, const OptimizerSettings &settings,
        const std::shared_ptr<RamBudget> &budget, ResourceGuard *guard,
        const std::atomic<bool> *cancel);
    static bool materialize(const FileTypeInfo &file);
};
} // namespace UltraArch
