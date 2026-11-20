#pragma once

#include "ResourceGuard.hpp"
#include "Types.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace UltraArch {

struct ProcessResult {
    int exitCode = -1;
    uint32_t pid = 0;
    uint32_t win32Error = 0;
    bool success = false;
    bool killedByOOM = false;
    bool cancelled = false;
    bool timedOut = false;
    bool outputTruncated = false;
    bool stderrTruncated = false;
    double durationSeconds = 0.0;
    uint64_t peakMemoryMB = 0;
    uint64_t readBytes = 0,
             writeBytes = 0; // Main process I/O counters; unavailable in native harness.
    std::string stdOutput;
    std::string errOutput; // Separate child stderr on Windows; POSIX harness merges into stdout.
};

struct ProcessControl {
    ProcessControl(const std::atomic<bool> *token = nullptr, uint32_t timeout = 0)
        : taskCancel(token), timeoutSeconds(timeout) {}
    const std::atomic<bool> *taskCancel = nullptr;
    uint32_t timeoutSeconds = 0;
    std::function<bool()> resourceStop; // Bounded-workspace watchdog.
    std::string stdinData;              // Private IPC, never part of argv/logs.
    // Native process creation/completion notifications, including very short children.
    // Start is emitted only after resource protection succeeds. Exceptions are ignored.
    std::function<void(uint32_t, const char *)> lifecycleCallback;
};

struct ProcessTelemetry {
    uint64_t memoryMB = 0;
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
};

class ProcessManager {
  public:
    ProcessManager() = default;

    // Executes a process synchronously on the current thread, streaming output via callback
    static ProcessResult execute(const std::string &executablePath,
                                 const std::vector<std::string> &arguments,
                                 const std::filesystem::path &workingDir,
                                 ResourceGuard *resourceGuard = nullptr, uint32_t workerIndex = 0,
                                 const std::atomic<bool> *cancelToken = nullptr,
                                 std::function<void(const std::string &)> outputCallback = nullptr,
                                 const ProcessControl *control = nullptr);

    static ProcessTelemetry telemetry();

    // Resolves executable path searching in \bin\, system PATH, and relative directories
    static std::filesystem::path
    resolveExecutable(const std::string &exeName, const std::filesystem::path &customBinDir = "bin",
                      bool strict = false);
};

} // namespace UltraArch
