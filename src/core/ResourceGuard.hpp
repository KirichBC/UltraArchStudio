#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace UltraArch {

struct CpuTopology {
    uint32_t logicalProcessors = 1;
    uint32_t physicalCores = 1;
    bool smtEnabled = false;
    std::vector<uint64_t> physicalCoreMasks; // One mask per physical core
    uint32_t processorGroups = 1;
    std::vector<uint32_t> coresPerGroup;
};

class ResourceGuard {
  public:
    ResourceGuard();
    ~ResourceGuard();

    // Disable copy
    ResourceGuard(const ResourceGuard &) = delete;
    ResourceGuard &operator=(const ResourceGuard &) = delete;

    // Initialize WinAPI Job Object with a hard child committed-memory limit.
    bool initialize(uint64_t maxMemoryLimitMB);

    // Updates the existing job without replacing it or disturbing its other limits.
    bool updateMemoryLimits(uint64_t maxMemoryLimitMB);
    uint64_t getCurrentMemoryMB() const;
    static double systemCpuUsage(); // Recent system-wide busy fraction; zero until sampled.
    static bool hasMemoryHeadroom(uint64_t additionalMB, uint64_t reserveMB = 1024);

    // Binds a child process handle to this job object and sets physical CPU affinity
    bool attachProcess(void *processHandle, uint32_t workerIndex = 0);

    void setScheduling(int affinity, bool background) {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_affinity = affinity;
        m_background = background;
    }
    bool setCpuRate(uint32_t percent);

    // Queries peak memory usage (in MB) reached by processes in this job object
    uint64_t getPeakMemoryMB() const;
    double getCpuSeconds() const;
    struct Accounting {
        bool available = false;
        uint64_t processes = 0, readBytes = 0, writeBytes = 0;
    };
    Accounting getAccounting() const;

    // Checks if large pages privilege (SeLockMemoryPrivilege) is enabled or enables it
    static bool enableLargePagesPrivilege();

    // Queries CPU topology, discovering physical cores and calculating non-SMT affinity masks
    static CpuTopology detectCpuTopology();
    // Re-queries this process's currently available CPUs; affinity 1 counts physical cores.
    static uint32_t availableCpuThreads(int affinity = 0);

    // Gets affinity mask for a specific worker ensuring physical core separation
    static uint64_t getWorkerAffinityMask(uint32_t workerIndex, const CpuTopology &topology);

    // Closes and cleans up Job Object
    void release();

    uint64_t getMemoryLimitMB() const {
        return m_memoryLimitMB.load();
    }
    static void setWorkerCount(uint32_t count) {
        s_workerCount = count;
    }

  private:
    friend struct ResourceGuardTestAccess;
    void releaseUnlocked();
    mutable std::mutex m_jobMutex;
    static std::atomic<uint32_t> s_workerCount;
#ifdef _WIN32
    HANDLE m_jobHandle = nullptr;
#endif
    int m_affinity = 0;
    bool m_background = false;
    std::atomic<uint64_t> m_memoryLimitMB{0};
    bool m_initialized = false;
    static CpuTopology s_cachedTopology;
    static bool s_topologyCached;
};

} // namespace UltraArch
