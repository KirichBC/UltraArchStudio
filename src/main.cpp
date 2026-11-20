#include "core/ContentAnalyzer.hpp"
#include "core/EmbeddedTools.hpp"
#include "core/OptimizerEngine.hpp"
#include "core/ResourceGuard.hpp"
#include "core/SettingsJson.hpp"
#include "core/TaskMatrixGenerator.hpp"
#include "core/Types.hpp"
#include "core/PublicationTransaction.hpp"
#include "imgui.h"
#include "ui/MainWindow.hpp"
#include "ui/Theme.hpp"
#include <atomic>
#include <charconv>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <commctrl.h>
#include <d3d11.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <windows.h>

using namespace UltraArch;

static LONG WINAPI GlobalCrashHandler(EXCEPTION_POINTERS *pException) {
    char modName[MAX_PATH] = "Unknown Module";
    uintptr_t modBase = 0;
    uintptr_t rva = 0;
    void *addr = pException->ExceptionRecord->ExceptionAddress;

    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hMod) &&
        hMod) {
        GetModuleFileNameA(hMod, modName, sizeof(modName));
        modBase = (uintptr_t)hMod;
        rva = (uintptr_t)addr - modBase;
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "UltraArch Studio encountered an unexpected fatal error.\n\n"
             "Exception Code: 0x%08lX\n"
             "Fault Address:  0x%p\n"
             "Module:         %s\n"
             "Module Base:    0x%p\n"
             "RVA Offset:     +0x%llX\n\n"
             "Crash details: LocalAppData/UltraArchStudio/logs/crash_log.txt",
             (unsigned long)pException->ExceptionRecord->ExceptionCode, addr, modName,
             (void *)modBase, (unsigned long long)rva);

    FILE *f = nullptr;
    try {
        auto path = Diagnostics::logDirectory() / "crash_log.txt";
        f = _wfopen(path.c_str(), L"w");
    } catch (...) {
    }
    if (f) {
        fprintf(f, "Exception Code: 0x%08lX\n",
                (unsigned long)pException->ExceptionRecord->ExceptionCode);
        fprintf(f, "Fault Address:  0x%p\n", addr);
        fprintf(f, "Module:         %s\n", modName);
        fprintf(f, "Module Base:    0x%p\n", (void *)modBase);
        fprintf(f, "RVA Offset:     +0x%llX\n", (unsigned long long)rva);

        void *stack[16];
        USHORT frames = CaptureStackBackTrace(0, 16, stack, NULL);
        fprintf(f, "\nCall Stack Backtrace (%u frames):\n", frames);
        for (USHORT i = 0; i < frames; ++i) {
            HMODULE frameMod = nullptr;
            char frameModName[MAX_PATH] = "Unknown";
            uintptr_t frameBase = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)stack[i], &frameMod) &&
                frameMod) {
                GetModuleFileNameA(frameMod, frameModName, sizeof(frameModName));
                frameBase = (uintptr_t)frameMod;
            }
            fprintf(f, "  [%02u] 0x%p  %s (+0x%llX)\n", i, stack[i], frameModName,
                    (unsigned long long)((uintptr_t)stack[i] - frameBase));
        }
        fclose(f);
    }

    MessageBoxA(nullptr, buf, "UltraArch Studio - Fatal Crash", MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
static MainWindow *g_mainWindow = nullptr;
static OptimizerEngine *g_engine = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon(HWND hwnd);

#define WM_TRAYICON (WM_USER + 1)
static NOTIFYICONDATAW nid = {};
static bool isMinimizedToTray = false;

#endif

using namespace UltraArch;

static void printUsage(const char *prog) {
    std::cout
        << "UltraArch Studio / Repack Engine Pro - Next-Gen Archive Optimizer\n"
        << "Usage:\n"
        << "  GUI Mode: " << prog << "\n"
        << "  CLI Mode: " << prog << " --cli --input <file/dir> [options]\n\n"
        << "Options:\n"
        << "  --input <path>       Target file or directory to optimize\n"
        << "  --output <path>      Final output .7z archive path\n"
        << "  --temp <path>        Custom temporary workspace (avoids C: drive space exhaustion)\n"
        << "  --profile <name>     safe (WinRAR Safe) | deep (Deep Search) | extreme (Extreme "
           "RePack)\n"
        << "  --ram <mb>           RAM Guard limit in MB (default: 8192)\n"
        << "  --lzma-dictionary <mib> Maximum requested dictionary (0=auto; 1..1536)\n"
        << "  --ram-candidates     Keep small candidate archives in bounded RAM\n"
        << "  --ram-group-limit <mib> Maximum uncompressed RAM group (1..256; default 40)\n"
        << "  --ram-temp-budget <mib> Temporary RAM sub-budget (32..4096; default 1024)\n"
        << "  --similarity-groups   Add experimental competitor (Extreme grouping)\n"
        << "  --workers <count>    Parallel workers (default: 2)\n"
        << "  --sfx                Generate portable self-extracting archive (.exe)\n"
        << "  --no-verify          Disable candidate checks (final verification remains "
           "mandatory)\n"
        << "  --keep-all           Disable immediate Zero-Waste SSD cleaner\n"
        << "  --password <pass>    Set encryption password\n"
        << "  --encrypt-headers    Encrypt file headers\n"
        << "  --volume <mb>        Split output into volumes of <mb> size\n"
        << "  --solid <str>        Solid block size (on, off, e, 512m, etc.)\n"
        << "  --sort-types         Order whole-input files by type\n"
        << "  --preprocess         Enable preprocessors\n"
        << "  --adaptive           Alias for balanced search; explicit --search wins\n"
        << "  --tools <path>       Directory containing trusted tools\n"
        << "  --cpu-threads <n>    Total compression CPU thread budget\n"
        << "  --cpu-auto          Resolve available logical CPUs: up to CPUs/2 tasks, 2 threads/task\n"
        << "  --probe-sample-mb <mib> Parameter-search sample, 1..256 (not solid block size)\n"
        << "  --probe-budget <sec> Sample tournament budget, 1..3600\n"
        << "  --probe-timeout <sec> Per-sample process timeout, 1..600\n"
        << "  --probe-passes <n>  Sample passes, 1..4\n"
        << "  --full-attempt-limit <n> Explicit full-data attempt limit, 0..10000 (0=unlimited)\n"
        << "                      Per search/group; excludes original and orchestration; limits Full if nonzero\n"
        << "  --timeout <seconds>  Maximum time per process (0 = unlimited)\n"
        << "  --time-budget <sec>  Stop scheduling after budget, assemble best\n"
        << "  --verifier <mode>    disk (default) | dual | stream (Windows worker)\n"
        << "  --search <mode>     fast | balanced | full/exhaustive (default)\n"
        << "                      Full has no hidden sample pruning/time budget; explicit limits still apply\n"
        << "  --goal <goal>       size | speed (within --tolerance percent)\n"
        << "  --tolerance <pct>   Size tolerance for speed goal (default 1)\n"
        << "  --content <policy>  exact | optimize (changes resource bytes)\n"
        << "  --affinity <mode>   none | physical | sets\n"
        << "  --background        Child processes below normal priority\n"
        << "  --cpu-limit <pct>   Windows child job CPU hard cap, 0..100\n"
        << "  --disk-tasks <n>    Concurrent disk tasks, 0 = worker count\n"
        << "  --no-grouping       Disable grouped method search in Extreme\n"
        << "  --grouping-only     Measure grouped search with Store/original fallback\n"
        << "  --extract-limit <mb> / --extract-file-limit <mb> / --extract-count <n>\n"
        << "  --recover <dir>     Restore interrupted publication in output folder\n"
        << "  --report <path>     Write redacted JSON run report\n"
        << "  --what-if            Show actual candidate matrix without compressing\n";
}

static std::atomic<bool> cliInterrupted{false};
#ifdef _WIN32
static BOOL WINAPI cliControlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        cliInterrupted = true;
        return TRUE;
    }
    return FALSE;
}
#endif
int runCliMode(int argc, char *argv[]) {
#ifdef _WIN32
    HANDLE oldOut = GetStdHandle(STD_OUTPUT_HANDLE), oldErr = GetStdHandle(STD_ERROR_HANDLE);
    bool redirectedOut =
        oldOut && oldOut != INVALID_HANDLE_VALUE &&
        (GetFileType(oldOut) == FILE_TYPE_PIPE || GetFileType(oldOut) == FILE_TYPE_DISK);
    bool redirectedErr =
        oldErr && oldErr != INVALID_HANDLE_VALUE &&
        (GetFileType(oldErr) == FILE_TYPE_PIPE || GetFileType(oldErr) == FILE_TYPE_DISK);
    if (!GetConsoleWindow() && !AttachConsole(ATTACH_PARENT_PROCESS) && !redirectedOut)
        AllocConsole();
    auto connectStream = [](FILE *stream, HANDLE original, bool redirected) {
        if (redirected) {
            HANDLE copy = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &copy, 0, FALSE,
                                DUPLICATE_SAME_ACCESS)) {
                freopen("NUL", "w", stream);
                int fd = _open_osfhandle(reinterpret_cast<intptr_t>(copy), _O_TEXT);
                if (fd >= 0) {
                    _dup2(fd, _fileno(stream));
                    _close(fd);
                } else
                    CloseHandle(copy);
            }
        } else {
            freopen("CONOUT$", "w", stream);
        }
    };
    connectStream(stdout, oldOut, redirectedOut);
    connectStream(stderr, oldErr, redirectedErr);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(cliControlHandler, TRUE);
#endif
    try {
        std::filesystem::path input, output;
        OptimizerSettings settings;
        bool whatIf = false, explicitTools = false, explicitSearch = false;
        std::filesystem::path reportPath, recoveryDirectory;
        auto number = [](const std::string &text) -> uint64_t {
            uint64_t value = 0;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || text.empty())
                throw std::invalid_argument("Invalid nonnegative integer: " + text);
            return value;
        };
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc)
                    throw std::invalid_argument("Missing value for " + arg);
                return argv[++i];
            };
            auto smallNumber = [&]() -> uint32_t {
                auto n = number(value());
                if (n > UINT32_MAX)
                    throw std::invalid_argument("Integer out of range");
                return static_cast<uint32_t>(n);
            };
            auto boundedNumber = [&](uint32_t minimum, uint32_t maximum) -> uint32_t {
                const auto n = smallNumber();
                if (n < minimum || n > maximum)
                    throw std::invalid_argument(arg + " must be in " + std::to_string(minimum) +
                                                ".." + std::to_string(maximum));
                return n;
            };
            if (arg == "--cli")
                continue;
            else if (arg == "--help" || arg == "-h") {
                printUsage(argv[0]);
                return 0;
            } else if (arg == "--input")
                input = utf8Path(value());
            else if (arg == "--output")
                output = utf8Path(value());
            else if (arg == "--temp")
                settings.customTempDir = utf8Path(value());
            else if (arg == "--tools") {
                settings.toolsBinDir = utf8Path(value());
                explicitTools = true;
            } else if (arg == "--recover")
                recoveryDirectory = utf8Path(value());
            else if (arg == "--report")
                reportPath = utf8Path(value());
            else if (arg == "--verifier") {
                auto mode = value();
                if (mode == "disk")
                    settings.verificationMode = VerificationMode::Disk;
                else if (mode == "dual")
                    settings.verificationMode = VerificationMode::Dual;
                else if (mode == "stream")
                    settings.verificationMode = VerificationMode::Streaming;
                else
                    throw std::invalid_argument("Unknown verifier mode");
            } else if (arg == "--search") {
                auto mode = value();
                explicitSearch = true;
                if (mode == "fast")
                    settings.searchMode = SearchMode::Fast;
                else if (mode == "balanced")
                    settings.searchMode = SearchMode::Balanced;
                else if (mode == "exhaustive" || mode == "full")
                    settings.searchMode = SearchMode::Exhaustive;
                else
                    throw std::invalid_argument("Unknown search mode");
            } else if (arg == "--goal") {
                auto goal = value();
                if (goal == "size")
                    settings.selectionGoal = SelectionGoal::MinimumSize;
                else if (goal == "speed")
                    settings.selectionGoal = SelectionGoal::FastWithinTolerance;
                else
                    throw std::invalid_argument("Unknown selection goal");
            } else if (arg == "--tolerance") {
                auto text = value();
                std::istringstream parser(text);
                parser.imbue(std::locale::classic());
                parser >> settings.sizeTolerancePercent;
                if (!parser || !parser.eof())
                    throw std::invalid_argument("Invalid size tolerance");
            } else if (arg == "--content") {
                auto policy = value();
                if (policy == "exact")
                    settings.contentPolicy = ContentPolicy::ExactBytes;
                else if (policy == "optimize")
                    settings.contentPolicy = ContentPolicy::OptimizeResources;
                else
                    throw std::invalid_argument("Unknown content policy");
            } else if (arg == "--affinity") {
                auto mode = value();
                if (mode == "none")
                    settings.affinityMode = AffinityMode::None;
                else if (mode == "physical")
                    settings.affinityMode = AffinityMode::PhysicalCores;
                else if (mode == "sets")
                    settings.affinityMode = AffinityMode::CpuSets;
                else
                    throw std::invalid_argument("Unknown affinity mode");
            } else if (arg == "--background")
                settings.backgroundPriority = true;
            else if (arg == "--no-grouping")
                settings.enableGrouping = false;
            else if (arg == "--sort-types")
                settings.sortByType = true;
            else if (arg == "--grouping-only")
                settings.groupedSearchOnly = true;
            else if (arg == "--similarity-groups")
                settings.enableSimilarityGroups = true;
            else if (arg == "--cpu-limit")
                settings.cpuLimitPercent = smallNumber();
            else if (arg == "--disk-tasks")
                settings.diskTaskLimit = smallNumber();
            else if (arg == "--extract-limit")
                settings.extractionLimitMB = number(value());
            else if (arg == "--extract-file-limit")
                settings.extractionFileLimitMB = number(value());
            else if (arg == "--extract-count")
                settings.extractionFileCount = smallNumber();
            else if (arg == "--profile") {
                auto p = value();
                if (p == "safe")
                    settings.profile = OptimizationProfile::WinRAR_Safe;
                else if (p == "deep")
                    settings.profile = OptimizationProfile::DeepSearch;
                else if (p == "extreme")
                    settings.profile = OptimizationProfile::Extreme_RePack;
                else
                    throw std::invalid_argument("Unknown profile: " + p);
            } else if (arg == "--ram")
                settings.ramLimitMB = number(value());
            else if (arg == "--lzma-dictionary")
                settings.maxLzmaDictionaryMB = number(value());
            else if (arg == "--ram-candidates")
                settings.enableRamCandidates = true;
            else if (arg == "--ram-group-limit")
                settings.ramGroupLimitMB = number(value());
            else if (arg == "--ram-temp-budget")
                settings.ramTemporaryBudgetMB = number(value());
            else if (arg == "--workers")
                settings.workerThreads = smallNumber();
            else if (arg == "--cpu-threads")
                settings.cpuThreads = smallNumber();
            else if (arg == "--cpu-auto")
                settings.autoCpu = true;
            else if (arg == "--probe-sample-mb")
                settings.probeSampleMB = boundedNumber(1, 256);
            else if (arg == "--probe-budget")
                settings.probeBudgetSeconds = boundedNumber(1, 3600);
            else if (arg == "--probe-timeout")
                settings.probeTimeoutSeconds = boundedNumber(1, 600);
            else if (arg == "--probe-passes")
                settings.probePasses = boundedNumber(1, 4);
            else if (arg == "--full-attempt-limit")
                settings.fullAttemptLimit = boundedNumber(0, 10000);
            else if (arg == "--timeout")
                settings.processTimeoutSeconds = smallNumber();
            else if (arg == "--time-budget")
                settings.timeBudgetSeconds = smallNumber();
            else if (arg == "--sfx")
                settings.sfxOutput = true;
            else if (arg == "--no-verify")
                settings.verifyArchives = false;
            else if (arg == "--keep-all")
                settings.immediateCleanInferior = false;
            else if (arg == "--password")
                settings.password = value();
            else if (arg == "--encrypt-headers")
                settings.encryptHeaders = true;
            else if (arg == "--volume")
                settings.volumeSizeMB = number(value());
            else if (arg == "--solid")
                settings.solidBlockSize = value();
            else if (arg == "--preprocess")
                settings.enablePreprocessors = true;
            else if (arg == "--adaptive") {
                if (!explicitSearch)
                    settings.searchMode = SearchMode::Balanced;
                settings.enableAdaptivePruning = false;
            }
            else if (arg == "--what-if")
                whatIf = true;
            else
                throw std::invalid_argument("Unknown argument: " + arg);
        }
        if (!recoveryDirectory.empty()) {
            for (const auto &stage :
                 PublicationTransaction::pending(std::filesystem::absolute(recoveryDirectory))) {
                PublicationTransaction::recover(stage);
                std::cout << "Recovered: " << pathUtf8(stage) << "\n";
            }
            if (input.empty())
                return 0;
        }
        if (input.empty())
            throw std::invalid_argument("--input is required");
        input = std::filesystem::absolute(input);
        if (output.empty())
            output = utf8Path(pathUtf8(input) + (settings.sfxOutput ? ".ultra.exe" : ".ultra.7z"));
        if (explicitTools &&
            ProcessManager::resolveExecutable("7z", settings.toolsBinDir, true).empty())
            throw std::invalid_argument("Explicit --tools directory does not contain 7-Zip");
        if (!explicitTools && settings.toolsBinDir == "bin")
            settings.toolsBinDir = EmbeddedTools::ensureToolsReady();
        validateSettings(settings);
        if (whatIf) {
            auto effective = input;
            auto scratch = settings.customTempDir.empty() ? std::filesystem::temp_directory_path()
                                                          : settings.customTempDir;
            if (std::filesystem::is_directory(input) && isWithin(scratch, input))
                throw std::invalid_argument("Scratch must be outside input");
            auto temporary = scratch / ("ultraarch-preview-" + uniqueId());
            struct Cleanup {
                std::filesystem::path path;
                ~Cleanup() {
                    std::error_code ec;
                    std::filesystem::remove_all(path, ec);
                }
            } cleanup{temporary};
            ContentAnalyzer analyzer;
            auto summary = ArchiveExtractor::isArchive(input)
                               ? ArchiveExtractor::preview(input, settings.toolsBinDir, nullptr,
                                                           settings.password)
                               : analyzer.analyze(effective);
            if (!summary.totalFiles)
                throw std::runtime_error("Input has no files");
            TaskMatrixGenerator generator;
            auto tasks = generator.generateMatrix(summary, settings);
            std::cout << "Files: " << summary.totalFiles << ", bytes: " << summary.totalBytes
                      << ", candidates: " << tasks.size() << "\n";
            for (const auto &t : tasks) {
                std::cout << t.taskId << " | " << t.name << " | " << t.estimatedRamMB << " MiB | "
                          << redactFlags(t.flags) << "\n";
                if (!t.description.empty())
                    std::cout << "  " << t.description << "\n";
            }
            if (settings.enablePreprocessors)
                std::cout << "Preprocessing can change the final candidate matrix.\n";
            return 0;
        }
        OptimizerEngine engine;
        engine.setSettings(settings);
        engine.setLogCallback([](const auto &message) { std::cout << message << std::endl; });
        cliInterrupted = false;
        if (!engine.start(input, output))
            throw std::runtime_error(engine.getLastError());
        while (engine.getState() != EngineState::Completed &&
               engine.getState() != EngineState::Failed &&
               engine.getState() != EngineState::Cancelled) {
            if (cliInterrupted)
                engine.cancel();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        engine.wait();
        if (!reportPath.empty() && !engine.saveRunReport(std::filesystem::absolute(reportPath)))
            throw std::runtime_error("Cannot save run report");
        if (engine.getState() != EngineState::Completed) {
            std::cerr << engine.getLastError() << "\n";
            return engine.getState() == EngineState::Cancelled ? 2 : 1;
        }
        uint64_t total = 0;
        for (const auto &p : engine.getOutputFiles()) {
            total += std::filesystem::file_size(p);
            std::cout << "Output: " << pathUtf8(p) << "\n";
        }
        std::cout << "Completed; verified output size: " << total << " bytes\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

#if defined(_WIN32)

void AddTrayIcon(HWND hwnd) {
    memset(&nid, 0, sizeof(NOTIFYICONDATAW));
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1001;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcsncpy(nid.szTip, L"UltraArch Studio", 64);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nShowCmd;
    SetUnhandledExceptionFilter(GlobalCrashHandler);

    int numArgs = 0;
    LPWSTR *szArgList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    std::vector<std::string> args;
    if (szArgList) {
        for (int i = 0; i < numArgs; ++i) {
            int len =
                WideCharToMultiByte(CP_UTF8, 0, szArgList[i], -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                std::string s(len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, szArgList[i], -1, &s[0], len, nullptr, nullptr);
                s.resize(static_cast<size_t>(len - 1));
                args.push_back(std::move(s));
            } else {
                args.push_back("");
            }
        }
        LocalFree(szArgList);
    }

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--cli" || args[i] == "--help" || args[i] == "-h") {
            std::vector<char *> c_argv;
            for (auto &a : args)
                c_argv.push_back(&a[0]);
            return runCliMode(static_cast<int>(c_argv.size()), c_argv.data());
        }
    }

    try {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
        wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"UltraArchStudioClass";

        RegisterClassExW(&wc);
        HWND hwnd =
            CreateWindowW(wc.lpszClassName, L"UltraArch Studio Pro - Next-Gen Archive Optimizer",
                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr,
                          nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            MessageBoxW(nullptr, L"Failed to create application window.", L"UltraArch Studio Error",
                        MB_ICONERROR);
            CoUninitialize();
            return 1;
        }

        DragAcceptFiles(hwnd, TRUE);

        // 1. Direct3D 11 initialization
        if (!CreateDeviceD3D(hwnd)) {
            CleanupDeviceD3D();
            DestroyWindow(hwnd);
            UnregisterClassW(wc.lpszClassName, wc.hInstance);
            CoUninitialize();
            return 1;
        }

        // 2. Dear ImGui context and backends initialized BEFORE ShowWindow so messages are handled
        // safely
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        Theme::applyDpiScale(static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f);
        Theme::applyDarkFluentStyle();

        // Load Segoe UI with Cyrillic & Latin support if available
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 2;
        const char *segoePath = "C:\\Windows\\Fonts\\segoeui.ttf";
        std::error_code fec;
        if (std::filesystem::exists(segoePath, fec)) {
            static const ImWchar ranges[] = {
                0x0020, 0x00FF, // Basic Latin + Latin Supplement
                0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
                0x2DE0, 0x2DFF, // Cyrillic Extended-A
                0xA640, 0xA69F, // Cyrillic Extended-B
                0,
            };
            io.Fonts->AddFontFromFileTTF(segoePath, 17.0f, &fontConfig, ranges);
        } else {
            io.Fonts->AddFontDefault();
        }

        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

        OptimizerEngine engine;
        g_engine = &engine;
        MainWindow mainWindow(engine);
        g_mainWindow = &mainWindow;

        if (args.size() > 1 && !args[1].empty() && args[1][0] != '-') {
            mainWindow.setInputPath(args[1]);
        }

        AddTrayIcon(hwnd);

        // 3. Show window now that all subsystems and backends are ready
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);

        static bool g_SwapChainOccluded = false;
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                    done = true;
            }
            if (done)
                break;

            mainWindow.update();
            if (isMinimizedToTray || IsIconic(hwnd)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (g_SwapChainOccluded && g_pSwapChain &&
                g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            g_SwapChainOccluded = false;

            // Handle window resize (we don't resize directly in the WM_SIZE handler)
            if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
                CleanupRenderTarget();
                if (g_pSwapChain) {
                    g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                                DXGI_FORMAT_UNKNOWN, 0);
                }
                g_ResizeWidth = g_ResizeHeight = 0;
                CreateRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            mainWindow.render();

            ImGui::Render();
            if (g_mainRenderTargetView && g_pd3dDeviceContext) {
                const float clear_color_with_alpha[4] = {0.102f, 0.106f, 0.118f, 1.0f};
                g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
                g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                           clear_color_with_alpha);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }

            if (g_pSwapChain) {
                HRESULT hr = g_pSwapChain->Present(1, 0); // VSync = 1
                g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
            }
        }

        engine.cancel();
        engine.wait();
        g_engine = nullptr;
        g_mainWindow = nullptr;
        RemoveTrayIcon(hwnd);

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();

        return 0;
    } catch (const std::exception &e) {
        MessageBoxA(nullptr, e.what(), "UltraArch Studio - Error", MB_ICONERROR);
        return 1;
    } catch (...) {
        MessageBoxA(nullptr, "An unexpected fatal error occurred.", "UltraArch Studio - Error",
                    MB_ICONERROR);
        return 1;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                                   D3D_FEATURE_LEVEL_9_3};
    UINT numFeatureLevels = sizeof(featureLevelArray) / sizeof(featureLevelArray[0]);

    // 1. Try Hardware Driver
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray,
        numFeatureLevels, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
        &g_pd3dDeviceContext);

    // 2. If Hardware fails for ANY reason, fallback to WARP software renderer
    if (res != S_OK) {
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            createDeviceFlags, featureLevelArray, numFeatureLevels,
                                            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                                            &featureLevel, &g_pd3dDeviceContext);
    }

    if (res != S_OK) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg),
                 "DirectX 11 initialization failed (HRESULT: 0x%08lX).\nPlease verify display "
                 "adapter drivers.",
                 (unsigned long)res);
        MessageBoxA(nullptr, errMsg, "UltraArch Studio - Graphics Error", MB_ICONERROR);
        return false;
    }

    CreateRenderTarget();
    return (g_mainRenderTargetView != nullptr);
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget() {
    ID3D11Texture2D *pBackBuffer = nullptr;
    if (g_pSwapChain && g_pd3dDevice) {
        HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (SUCCEEDED(hr) && pBackBuffer) {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (g_engine && g_engine->getState() != EngineState::Idle &&
                g_engine->getState() != EngineState::Completed &&
                g_engine->getState() != EngineState::Cancelled &&
                g_engine->getState() != EngineState::Failed) {
                ShowWindow(hWnd, SW_HIDE);
                isMinimizedToTray = true;
            }
            return 0;
        }
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT length = DragQueryFileW(hDrop, 0, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(hDrop, 0, path.data(), length + 1);
        path.resize(length);
        if (g_mainWindow)
            g_mainWindow->handleDragDrop(pathUtf8(std::filesystem::path(path)).c_str());
        DragFinish(hDrop);
        return 0;
    }
    case WM_DPICHANGED: {
        Theme::applyDpiScale(static_cast<float>(HIWORD(wParam)) / 96.0f);
        RECT *suggested = (RECT *)lParam;
        SetWindowPos(hWnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
            isMinimizedToTray = false;
        }
        break;
    case WM_CLOSE:
        if (g_engine && g_engine->getState() != EngineState::Idle &&
            g_engine->getState() != EngineState::Completed &&
            g_engine->getState() != EngineState::Cancelled &&
            g_engine->getState() != EngineState::Failed) {
            ShowWindow(hWnd, SW_HIDE);
            isMinimizedToTray = true;
            return 0; // Prevent close
        }
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int main(int argc, char *argv[]) {
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOWDEFAULT);
}

#else

// Non-Windows entry point (macOS / Linux)
int main(int argc, char *argv[]) {
    // If CLI flag passed or no GUI display, run CLI
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--cli") {
            return runCliMode(argc, argv);
        }
    }

    return runCliMode(argc, argv);
}

#endif
