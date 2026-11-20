#include "ProcessManager.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace UltraArch {
std::filesystem::path ProcessManager::resolveExecutable(const std::string &exeName,
                                                        const std::filesystem::path &customBinDir,
                                                        bool strict) {

    std::vector<std::string> candidates = {exeName};
    if (exeName == "7za" || exeName == "7z") {
        candidates.push_back("7za");
        candidates.push_back("7z");
        candidates.push_back("7zz");
    }
#ifdef _WIN32
    std::vector<std::string> winCandidates;
    for (const auto &c : candidates) {
        winCandidates.push_back(c);
        if (c.length() < 4 || c.substr(c.length() - 4) != ".exe") {
            winCandidates.push_back(c + ".exe");
        }
    }
    candidates = winCandidates;
#endif

    // An empty explicit directory must not implicitly search the working directory.
    if (!customBinDir.empty())
        for (const auto &name : candidates) {
            std::filesystem::path p = customBinDir / utf8Path(name);
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec))
                return std::filesystem::absolute(p);
        }

    if (strict)
        return {};

    // 3. Search in system PATH
    const char *pathEnv = std::getenv("PATH");
    if (pathEnv) {
        std::stringstream ss(pathEnv);
        std::string item;
#ifdef _WIN32
        const char delimiter = ';';
#else
        const char delimiter = ':';
#endif
        while (std::getline(ss, item, delimiter)) {
            if (item.empty())
                continue;
            for (const auto &name : candidates) {
                std::filesystem::path p = utf8Path(item) / name;
                std::error_code ec;
                if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec)) {
                    return std::filesystem::absolute(p);
                }
            }
        }
    }

    // 4. Common standard directories
    std::vector<std::filesystem::path> standardDirs = {
#ifdef _WIN32
        "C:\\Program Files\\7-Zip", "C:\\Program Files (x86)\\7-Zip"
#else
        "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"
#endif
    };
    for (const auto &dir : standardDirs) {
        for (const auto &name : candidates) {
            std::filesystem::path p = dir / name;
            std::error_code ec;
            if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec)) {
                return p;
            }
        }
    }

    // Missing tools must be reported before process creation.
    return {};
}
namespace {
constexpr size_t OutputLimit = 1024 * 1024;
void appendOutput(ProcessResult &r, std::string &pending, const char *data, size_t size,
                  const std::function<void(const std::string &)> &cb) {
    r.stdOutput.append(data, size);
    if (r.stdOutput.size() > OutputLimit) {
        r.outputTruncated = true;
        r.stdOutput.erase(0, r.stdOutput.size() - OutputLimit);
    }
    if (!cb)
        return;
    pending.append(data, size);
    size_t end;
    while ((end = pending.find_first_of("\r\n")) != std::string::npos) {
        if (end)
            try {
                cb(pending.substr(0, end));
            } catch (...) {
            }
        pending.erase(0, end + 1);
    }
    if (pending.size() > 65536) {
        try {
            cb(pending);
        } catch (...) {
        }
        pending.clear();
    }
}
void appendError(ProcessResult &r, const char *data, size_t size) {
    r.errOutput.append(data, size);
    if (r.errOutput.size() > OutputLimit) {
        r.stderrTruncated = true;
        r.errOutput.erase(0, r.errOutput.size() - OutputLimit);
    }
}
void notifyLifecycle(const ProcessControl *control, uint32_t pid, const char *event) noexcept {
    if (control && control->lifecycleCallback)
        try {
            control->lifecycleCallback(pid, event);
        } catch (...) {
        }
}
bool stopRequested(const std::atomic<bool> *run, const ProcessControl *control) {
    if ((run && run->load()) || (control && control->taskCancel && control->taskCancel->load()))
        return true;
    if (control && control->resourceStop) {
        try {
            return control->resourceStop();
        } catch (...) {
            return true;
        }
    }
    return false;
}
#ifdef _WIN32
std::mutex processesMutex;
std::map<DWORD, HANDLE> processes;
uint64_t retiredRead = 0, retiredWrite = 0;
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
struct WorkingDirectory {
    std::filesystem::path path, alias;
    ~WorkingDirectory() {
        // Remove only the junction itself, never recurse into the input directory.
        if (!alias.empty())
            RemoveDirectoryW(alias.c_str());
    }
    bool prepare(const std::filesystem::path &requested) {
        path = requested;
        if (path.empty() || path.native().size() < MAX_PATH)
            return true;
        // CreateProcessW rejects a long CWD even with a \\?\ prefix. A private
        // short junction preserves relative input names without copying data.
        wchar_t temp[32768]{};
        auto length = GetTempPathW(32768, temp);
        if (!length || length >= 32768)
            return false;
        alias = std::filesystem::path(temp) / ("ultraarch-cwd-" + uniqueId());
        if (alias.native().size() >= MAX_PATH || !CreateDirectoryW(alias.c_str(), nullptr)) {
            alias.clear();
            return false;
        }
        auto target = std::filesystem::absolute(requested).lexically_normal().native();
        if (target.rfind(L"\\\\?\\", 0) == 0)
            target.erase(0, 4);
        if (target.rfind(L"\\\\", 0) == 0) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return false;
        }
        auto substitute = L"\\??\\" + target;
        auto dataBytes = (substitute.size() + 1 + target.size() + 1) * sizeof(wchar_t);
        if (dataBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 16) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return false;
        }
        struct Header {
            DWORD tag;
            WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
        } header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(8 + dataBytes), 0, 0,
                 static_cast<WORD>(substitute.size() * sizeof(wchar_t)),
                 static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t)),
                 static_cast<WORD>(target.size() * sizeof(wchar_t))};
        static_assert(sizeof(Header) == 16);
        std::vector<unsigned char> data(sizeof(header) + dataBytes, 0);
        std::memcpy(data.data(), &header, sizeof(header));
        std::memcpy(data.data() + sizeof(header), substitute.data(), header.substituteLength);
        std::memcpy(data.data() + sizeof(header) + header.printOffset, target.data(),
                    header.printLength);
        Handle directory(CreateFileW(alias.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr));
        DWORD returned = 0;
        if (directory.value == INVALID_HANDLE_VALUE ||
            !DeviceIoControl(directory.value, FSCTL_SET_REPARSE_POINT, data.data(),
                              static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr))
            return false;
        path = alias;
        return true;
    }
};
#endif
} // namespace
ProcessTelemetry ProcessManager::telemetry() {
    ProcessTelemetry result;
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(processesMutex);
    result.readBytes = retiredRead;
    result.writeBytes = retiredWrite;
    for (const auto &[pid, handle] : processes) {
        (void)pid;
        PROCESS_MEMORY_COUNTERS_EX mem{};
        if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&mem),
                                 sizeof(mem)))
            result.memoryMB += mem.PrivateUsage / (1024 * 1024);
        IO_COUNTERS io{};
        if (GetProcessIoCounters(handle, &io)) {
            result.readBytes += io.ReadTransferCount;
            result.writeBytes += io.WriteTransferCount;
        }
    }
#endif
    return result;
}
ProcessResult ProcessManager::execute(const std::string &executablePath,
                                      const std::vector<std::string> &arguments,
                                      const std::filesystem::path &workingDir,
                                      ResourceGuard *resourceGuard, uint32_t workerIndex,
                                      const std::atomic<bool> *cancelToken,
                                      std::function<void(const std::string &)> outputCallback,
                                      const ProcessControl *control) {
    ProcessResult result;
    auto start = std::chrono::steady_clock::now();
    if (executablePath.empty()) {
        result.errOutput = "Executable not found";
        return result;
    }
#ifdef _WIN32
    // 7-Zip parses GetCommandLineW itself: quotes toggle grouping and backslashes
    // are literal. CRT escaping cannot encode a quote inside a 7-Zip argument.
    auto executableName = pathUtf8(utf8Path(executablePath).filename());
    std::transform(executableName.begin(), executableName.end(), executableName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool sevenZip = executableName == "7z.exe" || executableName == "7za.exe" ||
                          executableName == "7zz.exe";
    auto preparedArguments = arguments;
    ProcessControl passwordControl = control ? *control : ProcessControl{};
    if (sevenZip) {
        for (auto it = preparedArguments.begin(); it != preparedArguments.end();) {
            if (it->size() > 2 && it->rfind("-p", 0) == 0) {
                const auto password = it->substr(2);
                if (password.find_first_of("\r\n") != std::string::npos ||
                    password.find('\0') != std::string::npos ||
                    !passwordControl.stdinData.empty()) {
                    result.errOutput = "Invalid 7-Zip password input";
                    return result;
                }
                // Creation requests encryption with -p; readers prompt only when
                // -p is absent. UTF-8 stdin preserves quotes and trailing slashes.
                passwordControl.stdinData = password + "\n" + password + "\n";
                control = &passwordControl;
                if (!preparedArguments.empty() &&
                    (preparedArguments.front() == "a" || preparedArguments.front() == "u")) {
                    *it++ = "-p";
                } else {
                    it = preparedArguments.erase(it);
                }
            } else {
                ++it;
            }
        }
        if (std::none_of(preparedArguments.begin(), preparedArguments.end(),
                         [](const auto &arg) { return arg.rfind("-scc", 0) == 0; }))
            preparedArguments.push_back("-sccUTF-8");
        for (const auto &arg : preparedArguments) {
            if (arg.find('"') != std::string::npos || arg.find('\0') != std::string::npos) {
                result.errOutput = "Unrepresentable 7-Zip argument";
                return result;
            }
        }
    }
#endif
    if (stopRequested(cancelToken, control)) {
        result.cancelled = true;
        return result;
    }
    auto timeout = [&] {
        return control && control->timeoutSeconds &&
               std::chrono::steady_clock::now() - start >=
                   std::chrono::seconds(control->timeoutSeconds);
    };
    std::array<char, 16384> buffer{};
    std::string pending;
#ifdef _WIN32
    WorkingDirectory processDirectory;
    if (!processDirectory.prepare(workingDir)) {
        result.win32Error = GetLastError();
        result.errOutput = "Cannot prepare process working directory: " +
                           std::to_string(result.win32Error);
        return result;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readRaw = nullptr, writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) {
        result.errOutput = "CreatePipe failed";
        return result;
    }
    Handle readPipe(readRaw), writePipe(writeRaw);
    SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0);
    HANDLE errorReadRaw = nullptr, errorWriteRaw = nullptr;
    if (!CreatePipe(&errorReadRaw, &errorWriteRaw, &sa, 0)) {
        result.errOutput = "Cannot create stderr pipe";
        return result;
    }
    Handle errorReadPipe(errorReadRaw), errorWritePipe(errorWriteRaw);
    SetHandleInformation(errorReadPipe.value, HANDLE_FLAG_INHERIT, 0);
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr));
    if (nullInput.value == INVALID_HANDLE_VALUE) {
        result.errOutput = "Cannot open NUL input";
        return result;
    }
    HANDLE inputReadRaw = nullptr, inputWriteRaw = nullptr;
    bool hasInput = control && !control->stdinData.empty();
    if (hasInput && !CreatePipe(&inputReadRaw, &inputWriteRaw, &sa, 65536)) {
        result.errOutput = "Cannot create private IPC pipe";
        return result;
    }
    Handle inputRead(inputReadRaw), inputWrite(inputWriteRaw);
    if (hasInput)
        SetHandleInformation(inputWrite.value, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = writePipe.value;
    si.StartupInfo.hStdError = errorWritePipe.value;
    si.StartupInfo.hStdInput = hasInput ? inputRead.value : nullInput.value;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributes(attributeSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attributeSize)) {
        result.errOutput = "Cannot initialize handle list";
        return result;
    }
    HANDLE inherited[] = {writePipe.value, errorWritePipe.value, si.StartupInfo.hStdInput};
    bool updated =
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    auto exe = utf8Path(executablePath).wstring();
    auto quoteArgument = [&](const std::string &arg) {
        return sevenZip ? '"' + arg + '"' : quoteWindowsArgument(arg);
    };
    std::string command = quoteArgument(executablePath);
    for (const auto &a : preparedArguments)
        command += ' ' + quoteArgument(a);
    auto wideCommand = utf8Path(command).wstring();
    PROCESS_INFORMATION pi{};
    bool created =
        updated &&
        CreateProcessW(exe.c_str(), wideCommand.data(), nullptr, nullptr, TRUE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       processDirectory.path.empty() ? nullptr : processDirectory.path.c_str(),
                       &si.StartupInfo, &pi);
    DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(writePipe.value);
    writePipe.value = nullptr;
    CloseHandle(errorWritePipe.value);
    errorWritePipe.value = nullptr;
    if (!created) {
        result.win32Error = createError;
        result.errOutput = "CreateProcessW failed: " + std::to_string(createError);
        return result;
    }
    result.pid = pi.dwProcessId;
    Handle process(pi.hProcess), thread(pi.hThread);
    Handle taskJob(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION taskLimits{};
    taskLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool guarded = !resourceGuard || resourceGuard->attachProcess(process.value, workerIndex);
    guarded = guarded && taskJob.value &&
              SetInformationJobObject(taskJob.value, JobObjectExtendedLimitInformation, &taskLimits,
                                      sizeof(taskLimits)) &&
              AssignProcessToJobObject(taskJob.value, process.value);
    if (!guarded) {
        TerminateProcess(process.value, 99);
        WaitForSingleObject(process.value, INFINITE);
        result.errOutput = "Cannot apply process resource protection";
        return result;
    }
    notifyLifecycle(control, result.pid, "Start");
    if (ResumeThread(thread.value) == DWORD(-1)) {
        TerminateProcess(process.value, 99);
        WaitForSingleObject(process.value, INFINITE);
        result.errOutput = "Cannot resume protected process";
        notifyLifecycle(control, result.pid, "Finish");
        return result;
    }
    std::thread inputWriter;
    if (hasInput) {
        CloseHandle(inputRead.value);
        inputRead.value = nullptr;
        inputWriter = std::thread([&] {
            size_t offset = 0;
            while (offset < control->stdinData.size()) {
                DWORD wrote = 0;
                if (!WriteFile(inputWrite.value, control->stdinData.data() + offset,
                               static_cast<DWORD>(
                                   std::min<size_t>(65536, control->stdinData.size() - offset)),
                               &wrote, nullptr) ||
                    !wrote)
                    break;
                offset += wrote;
            }
            CloseHandle(inputWrite.value);
            inputWrite.value = nullptr;
        });
    }
    {
        std::lock_guard<std::mutex> lock(processesMutex);
        processes[pi.dwProcessId] = process.value;
    }
    // Drain both streams on every iteration so a full stderr pipe cannot block
    // an encoder whose stdout is a private binary response protocol.
    const auto drain = [&](HANDLE pipe, bool error, bool remaining) {
        bool consumed = false;
        for (size_t count = 0; count < (remaining ? 256u : 1u); ++count) {
            DWORD available = 0, read = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available ||
                !ReadFile(pipe, buffer.data(), static_cast<DWORD>(std::min<size_t>(buffer.size(), available)),
                          &read, nullptr) || !read)
                break;
            consumed = true;
            if (error) appendError(result, buffer.data(), read);
            else appendOutput(result, pending, buffer.data(), read, outputCallback);
        }
        return consumed;
    };
    while (true) {
        result.cancelled = stopRequested(cancelToken, control);
        result.timedOut = timeout();
        if (result.cancelled || result.timedOut) {
            TerminateJobObject(taskJob.value, 99);
            WaitForSingleObject(process.value, INFINITE);
            break;
        }
        const bool readStdout = drain(readPipe.value, false, false);
        const bool readStderr = drain(errorReadPipe.value, true, false);
        if (WaitForSingleObject(process.value, readStdout || readStderr ? 0 : 20) == WAIT_OBJECT_0) {
            break;
        }
    }
    drain(readPipe.value, false, true);
    drain(errorReadPipe.value, true, true);
    if (inputWriter.joinable())
        inputWriter.join();
    {
        std::lock_guard<std::mutex> lock(processesMutex);
        IO_COUNTERS io{};
        if (GetProcessIoCounters(process.value, &io)) {
            result.readBytes = io.ReadTransferCount;
            result.writeBytes = io.WriteTransferCount;
            retiredRead += io.ReadTransferCount;
            retiredWrite += io.WriteTransferCount;
        }
        processes.erase(pi.dwProcessId);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.value, &code);
    result.exitCode = static_cast<int>(code);
    PROCESS_MEMORY_COUNTERS_EX mem{};
    if (GetProcessMemoryInfo(process.value, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&mem),
                             sizeof(mem)))
        result.peakMemoryMB = mem.PeakPagefileUsage / (1024 * 1024);
    result.killedByOOM = code == 0xC0000017 || code == 8;
    notifyLifecycle(control, result.pid, "Finish");
#else
    // Portable core harness for development/testing; the product GUI targets Windows.
    (void)workerIndex;
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(executablePath.c_str()));
    for (const auto &a : arguments)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    int pipeFd[2];
    if (pipe(pipeFd) != 0) {
        result.errOutput = "pipe failed";
        return result;
    }
    fcntl(pipeFd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipeFd[1], F_SETFD, FD_CLOEXEC);
    uint64_t limit = resourceGuard ? resourceGuard->getMemoryLimitMB() : 0;
    const char *cwd = workingDir.empty() ? nullptr : workingDir.c_str();
    pid_t pid = fork();
    if (pid < 0) {
        close(pipeFd[0]);
        close(pipeFd[1]);
        result.errOutput = "fork failed";
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(pipeFd[0]);
        dup2(pipeFd[1], STDOUT_FILENO);
        dup2(pipeFd[1], STDERR_FILENO);
        close(pipeFd[1]);
        int nullFd = open("/dev/null", O_RDONLY);
        if (nullFd >= 0) {
            dup2(nullFd, STDIN_FILENO);
            close(nullFd);
        }
        if (cwd && chdir(cwd) != 0)
            _exit(126);
#ifndef __APPLE__
        if (limit) {
            struct rlimit lim{limit * 1024 * 1024, limit * 1024 * 1024};
            if (setrlimit(RLIMIT_AS, &lim) != 0)
                _exit(126);
        }
#else
        (void)limit;
#endif
        execv(executablePath.c_str(), argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    result.pid = static_cast<uint32_t>(pid);
    notifyLifecycle(control, result.pid, "Start");
    close(pipeFd[1]);
    fcntl(pipeFd[0], F_SETFL, O_NONBLOCK);
    int status = 0;
    struct rusage usage{};
    bool reaped = false;
    while (!reaped) {
        result.cancelled = stopRequested(cancelToken, control);
        result.timedOut = timeout();
        if (result.cancelled || result.timedOut) {
            kill(-pid, SIGTERM);
            for (int i = 0; i < 25; ++i) {
                pid_t w = wait4(pid, &status, WNOHANG, &usage);
                if (w == pid) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            kill(-pid, SIGKILL);
            if (!reaped) {
                while (wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
                }
                reaped = true;
            }
            break;
        }
        ssize_t n = read(pipeFd[0], buffer.data(), buffer.size());
        if (n > 0)
            appendOutput(result, pending, buffer.data(), static_cast<size_t>(n), outputCallback);
        pid_t w = wait4(pid, &status, WNOHANG, &usage);
        if (w == pid)
            reaped = true;
        else if (w < 0 && errno != EINTR) {
            result.errOutput = "wait4 failed";
            break;
        }
        if (n <= 0 && !reaped)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ssize_t n;
    while ((n = read(pipeFd[0], buffer.data(), buffer.size())) > 0)
        appendOutput(result, pending, buffer.data(), static_cast<size_t>(n), outputCallback);
    close(pipeFd[0]);
    if (reaped) {
        if (WIFEXITED(status))
            result.exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            result.exitCode = 128 + WTERMSIG(status);
    }
#ifdef __APPLE__
    result.peakMemoryMB = usage.ru_maxrss / (1024 * 1024);
#else
    result.peakMemoryMB = usage.ru_maxrss / 1024;
#endif
    notifyLifecycle(control, result.pid, "Finish");
#endif
    if (!pending.empty() && outputCallback)
        try {
            outputCallback(pending);
        } catch (...) {
        }
    result.success = result.exitCode == 0 && !result.cancelled && !result.timedOut;
    result.killedByOOM =
        result.killedByOOM || result.stdOutput.find("Out of memory") != std::string::npos ||
        result.stdOutput.find("Can't allocate required memory") != std::string::npos ||
        result.errOutput.find("Out of memory") != std::string::npos ||
        result.errOutput.find("Can't allocate required memory") != std::string::npos;
    if (!result.success && result.errOutput.empty())
        result.errOutput = result.timedOut ? "Process timed out" : result.stdOutput;
    result.durationSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}
} // namespace UltraArch
