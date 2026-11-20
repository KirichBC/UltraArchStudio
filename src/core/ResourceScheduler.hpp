#pragma once
#include <cstdint>
#include "Types.hpp"
namespace UltraArch {
// Called under the engine mutex. Bounded backfill fills spare resources, then drains
// reservations for the blocked head so a large candidate cannot starve indefinitely.
struct ResourceScheduler {
    uint64_t ramLimit = 0, ramReserved = 0;
    uint32_t cpuLimit = 1, cpuReserved = 0;
    size_t blockedHead = SIZE_MAX;
    uint32_t bypasses = 0;
    bool fits(uint64_t ram, uint32_t cpu) const {
        return ramReserved <= ramLimit && cpuReserved <= cpuLimit &&
               ram <= ramLimit - ramReserved && cpu <= cpuLimit - cpuReserved;
    }
    void reserve(uint64_t ram, uint32_t cpu) {
        if (!fits(ram, cpu)) throw std::logic_error("Resource reservation exceeds budget");
        ramReserved += ram;
        cpuReserved += cpu;
    }
    void release(uint64_t ram, uint32_t cpu) {
        if (ram > ramReserved || cpu > cpuReserved)
            throw std::logic_error("Resource reservation released twice");
        ramReserved -= ram;
        cpuReserved -= cpu;
    }
    size_t select(const std::vector<TaskConfig> &tasks,
                  const std::vector<CandidateResult> &results, size_t limit) {
        size_t head = 0;
        while (head < limit && results[head].status != CandidateStatus::Queued)
            ++head;
        if (head == limit)
            return tasks.size();
        if (blockedHead != head) {
            blockedHead = head;
            bypasses = 0;
        }
        if (fits(tasks[head].estimatedRamMB, tasks[head].cpuTokens)) {
            blockedHead = SIZE_MAX;
            bypasses = 0;
            return head;
        }
        if (bypasses < 32)
            for (size_t i = head + 1; i < limit; ++i)
                if (results[i].status == CandidateStatus::Queued &&
                    fits(tasks[i].estimatedRamMB, tasks[i].cpuTokens)) {
                    ++bypasses;
                    return i;
                }
        return tasks.size();
    }
};
} // namespace UltraArch
