#pragma once
#include "ContentAnalyzer.hpp"
#include "Types.hpp"
#include "ResourceGuard.hpp"
#include <algorithm>
#include <stdexcept>
namespace UltraArch {
// Reservations, not a promise of measured consumption or physically resident pages.
struct RamEstimate {
    uint64_t codecMB = 0, temporaryMB = 0, perTaskMB = 0, concurrentMB = 0;
    uint32_t slots = 0;
    bool fits = false;
};
struct RamPolicy {
    static constexpr uint64_t MiB = 1048576;
    static uint64_t outputCapacity(uint64_t bytes, uint64_t files) {
        uint64_t overhead = 16 * MiB + std::min<uint64_t>(files, 65536) * 1024;
        if (bytes > UINT64_MAX - overhead) throw std::overflow_error("RAM output capacity overflow");
        return bytes + overhead;
    }
    static uint64_t temporaryMB(uint64_t bytes, uint64_t files) {
        // One immutable input and one bounded output. Hash sinks do not retain extraction.
        auto output = outputCapacity(bytes, files);
        if (output > UINT64_MAX - (MiB - 1) || bytes > UINT64_MAX - output - (MiB - 1))
            throw std::overflow_error("RAM reservation overflow");
        return (bytes + output + MiB - 1) / MiB;
    }
    static uint64_t reservation(const TaskConfig &task, uint64_t bytes, uint64_t files) {
        return std::max(task.spec.memoryMB(), task.spec.memoryMB(false)) +
               temporaryMB(bytes, files) + 128;
    }
    static bool eligible(const OptimizerSettings &s, uint64_t bytes, uint64_t files) {
        return s.enableRamCandidates && bytes <= s.ramGroupLimitMB * MiB && files <= 65536 &&
               temporaryMB(bytes, files) <= std::min(s.ramTemporaryBudgetMB, s.ramLimitMB);
    }
    static RamEstimate estimate(const OptimizerSettings &s, uint64_t bytes) {
        RamEstimate e;
        uint64_t groupMB = std::max<uint64_t>(1, (bytes + MiB - 1) / MiB);
        uint64_t dictionary = std::min(groupMB, s.maxLzmaDictionaryMB ? s.maxLzmaDictionaryMB : 1536);
        uint32_t cores = s.cpuThreads ? s.cpuThreads : std::max(1u, ResourceGuard::detectCpuTopology().physicalCores);
        uint32_t threads = std::min(4u, std::max(1u, cores / std::max(1u, s.workerThreads)));
        // Worst graph can contain several LZMA branches. This preview deliberately
        // overestimates; actual admission uses each concrete CompressionSpec.
        e.codecMB = 64 + dictionary * 12 * std::max(1u, (threads + 1) / 2) *
                          (s.profile == OptimizationProfile::Extreme_RePack ? 3 : 1);
        uint64_t ppmd = 256;
        if (s.profile != OptimizationProfile::WinRAR_Safe)
            for (uint64_t value : {512ULL, 1024ULL, 1536ULL})
                if (value <= std::max<uint64_t>(64, groupMB * 2)) ppmd = value;
        e.codecMB = std::max(e.codecMB, ppmd + 64);
        // Preview assumes 1024 entries; actual reservation uses manifest entry count.
        e.temporaryMB = temporaryMB(bytes, 1024);
        e.perTaskMB = e.codecMB + e.temporaryMB + 128;
        uint64_t buffers = std::min(s.ramTemporaryBudgetMB, s.ramLimitMB) / e.temporaryMB;
        e.slots = static_cast<uint32_t>(std::min<uint64_t>({s.workerThreads, buffers, s.ramLimitMB / e.perTaskMB}));
        e.concurrentMB = e.perTaskMB * e.slots;
        e.fits = e.slots > 0;
        return e;
    }
};
} // namespace UltraArch
