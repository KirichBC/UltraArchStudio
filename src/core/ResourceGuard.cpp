#include "ResourceGuard.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#ifdef _WIN32
#include <psapi.h>
#endif

namespace UltraArch {

CpuTopology ResourceGuard::s_cachedTopology;
std::atomic<uint32_t> ResourceGuard::s_workerCount{1};
bool ResourceGuard::s_topologyCached = false;

ResourceGuard::ResourceGuard() = default;

ResourceGuard::~ResourceGuard() {
    release();
}
ResourceGuard::Accounting ResourceGuard::getAccounting() const {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    Accounting result;
#ifdef _WIN32
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION info{};
    if (m_jobHandle && QueryInformationJobObject(m_jobHandle, JobObjectBasicAndIoAccountingInformation,
        &info, sizeof(info), nullptr)) {
        result.available = true;
        result.processes = info.BasicInfo.TotalProcesses;
        result.readBytes = info.IoInfo.ReadTransferCount;
        result.writeBytes = info.IoInfo.WriteTransferCount;
    }
#endif
    return result;
}
double ResourceGuard::getCpuSeconds() const {
    std::lock_guard<std::mutex> lock(m_jobMutex);
#ifdef _WIN32
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (m_jobHandle && QueryInformationJobObject(m_jobHandle, JobObjectBasicAccountingInformation,
        &accounting, sizeof(accounting), nullptr))
        return double(accounting.TotalUserTime.QuadPart + accounting.TotalKernelTime.QuadPart) / 10000000.0;
#endif
    return 0;
}
bool ResourceGuard::setCpuRate(uint32_t percent) {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    if (percent > 100)
        return false;
#ifdef _WIN32
    if (!m_jobHandle)
        return false;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
    if (percent) {
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        rate.CpuRate = percent * 100;
    }
    return SetInformationJobObject(m_jobHandle, JobObjectCpuRateControlInformation, &rate, sizeof(rate)) != FALSE;
#else
    return percent == 0;
#endif
}

void ResourceGuard::release() {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    releaseUnlocked();
}

void ResourceGuard::releaseUnlocked() {
#ifdef _WIN32
    if (m_jobHandle) {
        CloseHandle(m_jobHandle);
        m_jobHandle = nullptr;
    }
#endif
    m_initialized = false;
    m_memoryLimitMB.store(0);
}

bool ResourceGuard::initialize(uint64_t maxMemoryLimitMB) {
    constexpr uint64_t mib = 1024ULL * 1024ULL;
    if (maxMemoryLimitMB > std::numeric_limits<size_t>::max() / mib)
        return false;
    std::lock_guard<std::mutex> lock(m_jobMutex);
    releaseUnlocked();

#ifdef _WIN32
    m_jobHandle = CreateJobObjectW(nullptr, nullptr);
    if (!m_jobHandle) {
        std::cerr << "[ResourceGuard] Warning: Failed to create WinAPI Job Object. Error: "
                  << GetLastError() << std::endl;
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo{};
    limitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                                 JOB_OBJECT_LIMIT_JOB_MEMORY |
                                                 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!maxMemoryLimitMB)
        limitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    const uint64_t bytesLimit = maxMemoryLimitMB * 1024ULL * 1024ULL;
    limitInfo.ProcessMemoryLimit = static_cast<SIZE_T>(bytesLimit);
    limitInfo.JobMemoryLimit = static_cast<SIZE_T>(bytesLimit);

    if (!SetInformationJobObject(m_jobHandle, JobObjectExtendedLimitInformation, &limitInfo,
                                 sizeof(limitInfo))) {
        std::cerr << "[ResourceGuard] Warning: Failed to set memory limits on Job Object. Error: "
                  << GetLastError() << std::endl;
        CloseHandle(m_jobHandle);
        m_jobHandle = nullptr;
        return false;
    }
#endif

    m_initialized = true;
    m_memoryLimitMB.store(maxMemoryLimitMB);
    return true;
}

bool ResourceGuard::updateMemoryLimits(uint64_t maxMemoryLimitMB) {
    constexpr uint64_t mib = 1024ULL * 1024ULL;
    if (maxMemoryLimitMB > std::numeric_limits<size_t>::max() / mib)
        return false;
    std::lock_guard<std::mutex> lock(m_jobMutex);
    if (!m_initialized)
        return false;
#ifdef _WIN32
    if (!m_jobHandle)
        return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(m_jobHandle, JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits), nullptr))
        return false;
    const uint64_t bytes = maxMemoryLimitMB * mib;
    if (maxMemoryLimitMB &&
        (!m_memoryLimitMB.load() || maxMemoryLimitMB < m_memoryLimitMB.load())) {
        JOBOBJECT_LIMIT_VIOLATION_INFORMATION usage{};
        if (!QueryInformationJobObject(m_jobHandle, JobObjectLimitViolationInformation,
                                      &usage, sizeof(usage), nullptr) || usage.JobMemory > bytes)
            return false;
    }
    if (maxMemoryLimitMB)
        limits.BasicLimitInformation.LimitFlags |=
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
    else
        limits.BasicLimitInformation.LimitFlags &=
            ~(JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY);
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(bytes);
    limits.JobMemoryLimit = static_cast<SIZE_T>(bytes);
    // One call changes both memory limits. On failure the published limit remains unchanged.
    if (!SetInformationJobObject(m_jobHandle, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        return false;
#endif
    m_memoryLimitMB.store(maxMemoryLimitMB);
    return true;
}

uint64_t ResourceGuard::getCurrentMemoryMB() const {
    std::lock_guard<std::mutex> lock(m_jobMutex);
#ifdef _WIN32
    JOBOBJECT_LIMIT_VIOLATION_INFORMATION usage{};
    if (m_jobHandle && QueryInformationJobObject(m_jobHandle, JobObjectLimitViolationInformation,
                                                &usage, sizeof(usage), nullptr)) {
        constexpr uint64_t mib = 1024ULL * 1024ULL;
        return usage.JobMemory / mib + (usage.JobMemory % mib != 0);
    }
#endif
    return 0;
}

double ResourceGuard::systemCpuUsage() {
#ifdef _WIN32
    static std::mutex mutex;
    static uint64_t previousIdle = 0, previousTotal = 0;
    static double load = 0;
    static auto sampled = std::chrono::steady_clock::time_point{};
    std::lock_guard lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - sampled < std::chrono::milliseconds(500)) return load;
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
    const auto value = [](FILETIME time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    const auto total = value(kernel) + value(user), idleTicks = value(idle);
    if (previousTotal && total > previousTotal && idleTicks >= previousIdle)
        load = 1.0 - std::min(1.0, double(idleTicks - previousIdle) / double(total - previousTotal));
    previousTotal = total; previousIdle = idleTicks; sampled = now;
    return load;
#else
    return 0;
#endif
}
bool ResourceGuard::hasMemoryHeadroom(uint64_t additionalMB, uint64_t reserveMB) {
    constexpr uint64_t mib = 1024ULL * 1024ULL;
    if (additionalMB > std::numeric_limits<uint64_t>::max() - reserveMB)
        return false;
    const uint64_t neededMB = additionalMB + reserveMB;
    if (neededMB > std::numeric_limits<uint64_t>::max() / mib)
        return false;
#ifdef _WIN32
    MEMORYSTATUSEX physical{};
    physical.dwLength = sizeof(physical);
    PERFORMANCE_INFORMATION commit{};
    commit.cb = sizeof(commit);
    if (!GlobalMemoryStatusEx(&physical) || !GetPerformanceInfo(&commit, sizeof(commit)) ||
        commit.CommitTotal > commit.CommitLimit || !commit.PageSize)
        return false;
    const uint64_t pages = commit.CommitLimit - commit.CommitTotal;
    if (pages > std::numeric_limits<uint64_t>::max() / commit.PageSize)
        return false;
    const uint64_t needed = neededMB * mib;
    return physical.ullAvailPhys >= needed && pages * commit.PageSize >= needed;
#else
    // An unavailable system commit query must not authorize a memory override.
    return false;
#endif
}

bool ResourceGuard::attachProcess(void *processHandle, uint32_t workerIndex) {
    std::lock_guard<std::mutex> lock(m_jobMutex);
#ifdef _WIN32
    if (!processHandle)
        return false;
    HANDLE hProcess = static_cast<HANDLE>(processHandle);

    if (m_jobHandle) {
        if (!AssignProcessToJobObject(m_jobHandle, hProcess)) {
            std::cerr << "[ResourceGuard] Failed to assign process to Job Object. Error: "
                      << GetLastError() << std::endl;
            return false;
        }
    }

    if (m_background)
        SetPriorityClass(hProcess, BELOW_NORMAL_PRIORITY_CLASS);
    if (m_affinity == 1) {
        auto topo = detectCpuTopology();
        auto mask = getWorkerAffinityMask(workerIndex, topo);
        if (mask)
            SetProcessAffinityMask(hProcess, static_cast<DWORD_PTR>(mask));
    } else if (m_affinity == 2) {
        // CPU Sets are a soft preference, and must not be combined with restrictive affinity.
        auto kernel = GetModuleHandleW(L"kernel32.dll");
        using Query = BOOL(WINAPI *)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
        using Assign = BOOL(WINAPI *)(HANDLE, const ULONG *, ULONG);
        auto query = reinterpret_cast<Query>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
        auto assign = reinterpret_cast<Assign>(GetProcAddress(kernel, "SetProcessDefaultCpuSets"));
        ULONG size = 0;
        if (query && assign && detectCpuTopology().processorGroups == 1) {
            query(nullptr, 0, &size, hProcess, 0);
            std::vector<unsigned char> data(size);
            if (size && query(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data()), size,
                              &size, hProcess, 0)) {
                std::vector<ULONG> ids;
                for (size_t offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= size;) {
                    auto info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data() + offset);
                    if (info->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) ||
                        info->Size > size - offset)
                        break;
                    if (info->Type == CpuSetInformation && !info->CpuSet.Parked &&
                        (!info->CpuSet.Allocated || info->CpuSet.AllocatedToTargetProcess))
                        ids.push_back(info->CpuSet.Id);
                    offset += info->Size;
                }
                if (!ids.empty())
                    assign(hProcess, ids.data(), static_cast<ULONG>(ids.size()));
            }
        } // Multi-group or missing API: let the Windows scheduler choose.
    }
    return true;
#else
    (void)processHandle;
    (void)workerIndex;
    return true;
#endif
}

uint64_t ResourceGuard::getPeakMemoryMB() const {
    std::lock_guard<std::mutex> lock(m_jobMutex);
#ifdef _WIN32
    if (!m_jobHandle)
        return 0;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    DWORD returnedLength = 0;
    if (QueryInformationJobObject(m_jobHandle, JobObjectExtendedLimitInformation, &info,
                                  sizeof(info), &returnedLength)) {
        return static_cast<uint64_t>(info.PeakJobMemoryUsed / (1024 * 1024));
    }
    return 0;
#else
    return 0;
#endif
}

bool ResourceGuard::enableLargePagesPrivilege() {
#ifdef _WIN32
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &luid)) {
        CloseHandle(hToken);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);

    return (ok && err == ERROR_SUCCESS);
#else
    return false;
#endif
}

CpuTopology ResourceGuard::detectCpuTopology() {
    static std::mutex topologyMutex;
    std::lock_guard<std::mutex> lock(topologyMutex);
    if (s_topologyCached) {
        return s_cachedTopology;
    }

    CpuTopology topo;
    topo.logicalProcessors = std::max<uint32_t>(1, std::thread::hardware_concurrency());
    topo.physicalCores = topo.logicalProcessors;

#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        std::vector<uint8_t> buffer(len);
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX infoPtr =
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

        if (GetLogicalProcessorInformationEx(RelationProcessorCore, infoPtr, &len)) {
            topo.physicalCoreMasks.clear();
            topo.processorGroups = 0;
            std::vector<uint32_t> groupCores(64, 0);

            DWORD offset = 0;
            uint32_t allPhysicalCores = 0;
            uint32_t allLogicalProcessors = 0;
            while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= len) {
                PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX current =
                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() +
                                                                               offset);

                if (current->Size < sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) ||
                    current->Size > len - offset)
                    break;
                if (current->Relationship == RelationProcessorCore) {
                    ++allPhysicalCores;
                    for (WORD i = 0; i < current->Processor.GroupCount; ++i) {
                        KAFFINITY coreMask = current->Processor.GroupMask[i].Mask;
                        WORD group = current->Processor.GroupMask[i].Group;
                        for (auto bits = coreMask; bits; bits &= bits - 1)
                            ++allLogicalProcessors;

                        if ((coreMask & (coreMask - 1)) != 0) {
                            topo.smtEnabled = true;
                        }
                        KAFFINITY primaryThreadMask = coreMask & (~coreMask + 1);

                        if (group == 0)
                            topo.physicalCoreMasks.push_back(
                                static_cast<uint64_t>(primaryThreadMask));
                        if (group < 64) {
                            groupCores[group]++;
                            if (group >= topo.processorGroups) {
                                topo.processorGroups = group + 1;
                            }
                        }
                    }
                }
                offset += current->Size;
            }
            if (allPhysicalCores)
                topo.physicalCores = allPhysicalCores;
            if (allLogicalProcessors)
                topo.logicalProcessors = allLogicalProcessors;
            for (uint32_t i = 0; i < topo.processorGroups; ++i) {
                topo.coresPerGroup.push_back(groupCores[i]);
            }
        }
    }
#endif

    // Do not invent affinity masks when the Windows topology query fails.
#ifndef _WIN32
    if (topo.physicalCoreMasks.empty()) {
        uint32_t count = topo.physicalCores;
        for (uint32_t i = 0; i < count; ++i) {
            topo.physicalCoreMasks.push_back(1ULL << (i % 64));
        }
    }

#endif
    s_cachedTopology = topo;
    s_topologyCached = true;
    return topo;
}

uint32_t ResourceGuard::availableCpuThreads(int affinity) {
#ifdef _WIN32
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto process = GetCurrentProcess();
    using GroupQuery = BOOL(WINAPI *)(HANDLE, PUSHORT, PUSHORT);
    using ActiveQuery = DWORD(WINAPI *)(WORD);
    using SetQuery = BOOL(WINAPI *)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    using DefaultQuery = BOOL(WINAPI *)(HANDLE, PULONG, ULONG, PULONG);
    const auto groupsQuery = reinterpret_cast<GroupQuery>(GetProcAddress(kernel, "GetProcessGroupAffinity"));
    const auto activeQuery = reinterpret_cast<ActiveQuery>(GetProcAddress(kernel, "GetActiveProcessorCount"));
    const auto setsQuery = reinterpret_cast<SetQuery>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    const auto defaultsQuery = reinterpret_cast<DefaultQuery>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    std::vector<USHORT> groups;
    USHORT groupCount = 0;
    if (groupsQuery) {
        groupsQuery(process, &groupCount, nullptr);
        if (groupCount) {
            groups.resize(groupCount);
            if (groupsQuery(process, &groupCount, groups.data()))
                groups.resize(groupCount);
            else
                groups.clear();
        }
    }
    if (groups.empty()) {
        GROUP_AFFINITY current{};
        if (GetThreadGroupAffinity(GetCurrentThread(), &current))
            groups.push_back(current.Group);
        else
            groups.push_back(0);
    }
    DWORD_PTR processMask = 0, systemMask = 0;
    const bool hasMask = groups.size() == 1 &&
        GetProcessAffinityMask(process, &processMask, &systemMask) && processMask;
    auto bitCount = [](uint64_t bits) {
        uint32_t count = 0;
        for (; bits; bits &= bits - 1)
            ++count;
        return count;
    };
    std::vector<ULONG> defaults;
    bool defaultsKnown = false;
    if (defaultsQuery) {
        ULONG count = 0;
        const BOOL first = defaultsQuery(process, nullptr, 0, &count);
        if (!count) {
            defaultsKnown = first != FALSE;
        } else {
            defaults.resize(count);
            defaultsKnown = defaultsQuery(process, defaults.data(), count, &count) != FALSE;
            if (defaultsKnown)
                defaults.resize(count);
        }
    }
    if (setsQuery && defaultsKnown) {
        ULONG bytes = 0;
        setsQuery(nullptr, 0, &bytes, process, 0);
        std::vector<unsigned char> data(bytes);
        if (bytes && setsQuery(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(data.data()), bytes,
                               &bytes, process, 0)) {
            uint32_t logical = 0;
            std::set<std::pair<USHORT, BYTE>> cores;
            for (size_t offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= bytes;) {
                const auto info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(data.data() + offset);
                if (info->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) || info->Size > bytes - offset)
                    break;
                if (info->Type == CpuSetInformation) {
                    const auto &cpu = info->CpuSet;
                    const bool member = std::find(groups.begin(), groups.end(), cpu.Group) != groups.end();
                    const bool selected = defaults.empty() ||
                        std::find(defaults.begin(), defaults.end(), cpu.Id) != defaults.end();
                    const bool inMask = !hasMask || (cpu.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8 &&
                        (processMask & (DWORD_PTR(1) << cpu.LogicalProcessorIndex)));
                    if (member && selected && inMask && !cpu.Parked &&
                        (!cpu.Allocated || cpu.AllocatedToTargetProcess)) {
                        ++logical;
                        cores.emplace(cpu.Group, cpu.CoreIndex);
                    }
                }
                offset += info->Size;
            }
            // A zero result may mean all CPUs are temporarily parked. Keep a one-thread fallback.
            return std::max<uint32_t>(1, affinity == 1 ?
                std::min<uint32_t>(logical, static_cast<uint32_t>(cores.size())) : logical);
        }
    }
    // Without CPU Set information only one process group is budgeted conservatively.
    uint32_t available = hasMask ? bitCount(processMask) :
        (activeQuery ? activeQuery(groups.front()) : 1);
    if (affinity == 1) {
        const auto topology = detectCpuTopology();
        uint32_t physical = 1;
        if (hasMask && groups.front() == 0 && !topology.physicalCoreMasks.empty()) {
            physical = 0;
            for (auto mask : topology.physicalCoreMasks)
                physical += (mask & processMask) != 0;
        } else if (groups.front() < topology.coresPerGroup.size()) {
            physical = topology.coresPerGroup[groups.front()];
        }
        available = std::min(available, physical);
    }
    return std::max<uint32_t>(1, available);
#else
    const auto topology = detectCpuTopology();
    return std::max<uint32_t>(1, affinity == 1 ? topology.physicalCores : topology.logicalProcessors);
#endif
}

uint64_t ResourceGuard::getWorkerAffinityMask(uint32_t workerIndex, const CpuTopology &topology) {
    if (topology.physicalCoreMasks.empty() || topology.processorGroups > 1) {
        return 0;
    }
    uint32_t numWorkers = std::max<uint32_t>(1, s_workerCount.load());
    uint32_t coresPerWorker = std::max<uint32_t>(1, topology.physicalCores / numWorkers);

    uint64_t combinedMask = 0;
    uint32_t startCore = (workerIndex * coresPerWorker) % topology.physicalCoreMasks.size();
    for (uint32_t i = 0; i < coresPerWorker; ++i) {
        combinedMask |=
            topology.physicalCoreMasks[(startCore + i) % topology.physicalCoreMasks.size()];
    }
    return combinedMask;
}

} // namespace UltraArch
