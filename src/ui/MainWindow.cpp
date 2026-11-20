#include "MainWindow.hpp"
#include "../core/EmbeddedTools.hpp"
#include "../core/PublicationTransaction.hpp"
#include "../core/SettingsJson.hpp"
#include "../core/DictionaryPolicy.hpp"
#include "../core/RamPolicy.hpp"
#include "Localization.hpp"
#include "Theme.hpp"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <limits>
#include <sstream>

#ifdef _WIN32
#include <shobjidl.h>
#include <windows.h>
#endif

namespace UltraArch {
namespace {
float scaled(float value) {
    return value * Theme::getDpiScale();
}
ImVec2 scaledSize(float width, float height) {
    return ImVec2(width > 0 ? scaled(width) : width, height > 0 ? scaled(height) : height);
}
void flowNext(float width = 260) {
    auto right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (right + scaled(width) + ImGui::GetStyle().ItemSpacing.x +
            ImGui::GetStyle().WindowPadding.x <
        ImGui::GetWindowWidth())
        ImGui::SameLine();
}
const char *localized(const char *en, const char *ru) {
    return L::currentLang() ? ru : en;
}
uint32_t previewAvailableCpuThreads(AffinityMode affinity) {
    // UI previews avoid a system query every frame; the engine resolves its own effective settings.
    static int cachedAffinity = -1;
    static double refreshedAt = -10;
    static uint32_t cachedAvailable = 1;
    const auto now = ImGui::GetTime();
    if (cachedAffinity != static_cast<int>(affinity) || now - refreshedAt >= 2.0) {
        cachedAvailable = ResourceGuard::availableCpuThreads(static_cast<int>(affinity));
        cachedAffinity = static_cast<int>(affinity);
        refreshedAt = now;
    }
    return cachedAvailable;
}
std::string sessionDate(std::chrono::system_clock::time_point value) {
    if (value.time_since_epoch().count() == 0) return "—";
    const auto stamp = std::chrono::system_clock::to_time_t(value);
    std::tm calendar{};
#ifdef _WIN32
    if (localtime_s(&calendar, &stamp) != 0) return "—";
#else
    if (!localtime_r(&stamp, &calendar)) return "—";
#endif
    std::ostringstream text;
    text << std::put_time(&calendar, "%Y-%m-%d %H:%M:%S");
    return text.str();
}
} // namespace

void MainWindow::showConsoleWindow(bool show) {
#ifdef _WIN32
    static bool s_consoleAllocated = false;
    if (show) {
        if (!s_consoleAllocated) {
            if (AllocConsole()) {
                s_consoleAllocated = true;
                FILE *fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
                freopen_s(&fp, "CONIN$", "r", stdin);
                SetConsoleTitleW(L"UltraArch Studio - Live Optimization & Test Output Console");

                HWND hConsole = GetConsoleWindow();
                if (hConsole) {
                    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
                    if (hIcon) {
                        SendMessageW(hConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                        SendMessageW(hConsole, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                    }
                }
            }
        }
        HWND hConsole = GetConsoleWindow();
        if (hConsole) {
            ShowWindow(hConsole, SW_SHOW);
            BringWindowToTop(hConsole);
        }
    } else {
        HWND hConsole = GetConsoleWindow();
        if (hConsole) {
            ShowWindow(hConsole, SW_HIDE);
        }
    }
#else
    (void)show;
#endif
}

MainWindow::MainWindow(OptimizerEngine &engine) : m_engine(engine) {
    m_presetManager.initialize(EmbeddedTools::getDataDirectory() / "presets", false);

    m_engine.setLogCallback([this](const std::string &msg) {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logs.push_back(msg);
        if (m_logs.size() > 500) {
            m_logs.pop_front();
        }
#ifdef _WIN32
        std::cout << msg << std::endl;
#endif
    });

    m_leaderboardWidget.selectCandidate = [this](const auto &id) {
        return m_engine.selectAndAssemble(id);
    };
    m_leaderboardWidget.retestCandidate = [this](const auto &id) { return m_engine.retest(id); };
    applyState(m_engine.getState());
}
MainWindow::~MainWindow() {
    m_previewCancel = true;
    if (m_preview.valid())
        m_preview.wait();
    m_exportCancel = true;
    if (m_sessionExport.valid())
        m_sessionExport.wait();
    m_engine.cancel();
    m_engine.wait();
    m_engine.setLogCallback(nullptr);
    m_engine.setStateCallback(nullptr);
}
void MainWindow::applyState(EngineState s) {
    switch (s) {
    case EngineState::Idle:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", L::tr(StringId::StatusIdle));
        m_settingsLocked = false;
        showConsoleWindow(false);
        break;
    case EngineState::Analyzing:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      L::tr(StringId::StatusAnalyzing));
        m_settingsLocked = true;
        showConsoleWindow(true);
        break;
    case EngineState::TestingCandidates:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      L::tr(StringId::StatusTesting));
        m_settingsLocked = true;
        showConsoleWindow(true);
        break;
    case EngineState::AssemblingFinal:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      L::tr(StringId::StatusAssembling));
        m_settingsLocked = true;
        showConsoleWindow(true);
        break;
    case EngineState::Paused:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      L::tr(StringId::StatusPaused));
        m_settingsLocked = true;
        break;
    case EngineState::Completed:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      L::tr(StringId::StatusCompleted));
        m_settingsLocked = false;
        if (m_autoShutdown) {
            m_shutdownScheduled = scheduleShutdown(false);
            std::lock_guard<std::mutex> lock(m_logMutex);
            m_logs.push_back(m_shutdownScheduled ? "System will shut down in 60 seconds"
                                                 : "Could not schedule system shutdown");
        }
        break;
    case EngineState::Cancelled:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      L::tr(StringId::StatusCancelled));
        m_settingsLocked = false;
        showConsoleWindow(false);
        break;
    case EngineState::Extracting:
    case EngineState::Preprocessing:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      s == EngineState::Extracting ? "Extracting input archive"
                                                   : "Preprocessing resources");
        m_settingsLocked = true;
        break;
    case EngineState::Failed:
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      m_engine.getLastError().c_str());
        m_settingsLocked = false;
        showConsoleWindow(false);
        break;
    default:
        break;
    }
    m_lastState = s;
}
OptimizerSettings MainWindow::collectSettings() const {
    OptimizerSettings settings = m_additionalSettings;
    settings.profile = static_cast<OptimizationProfile>(m_selectedProfileIndex);
    settings.ramLimitMB = static_cast<uint64_t>(m_ramLimitMB);
    settings.ramGroupLimitMB = std::clamp<uint64_t>(settings.ramGroupLimitMB, 1, 256);
    settings.ramTemporaryBudgetMB = std::clamp<uint64_t>(
        settings.ramTemporaryBudgetMB, 32, std::min<uint64_t>(4096, std::max<uint64_t>(32, settings.ramLimitMB)));
    settings.workerThreads = static_cast<uint32_t>(m_workerThreads);
    settings.verifyArchives = m_verifyArchives;
    settings.immediateCleanInferior = m_zeroWasteCleaner;
    settings.sfxOutput = m_createSfx;
    settings.password = m_passwordBuffer;
    settings.encryptHeaders = m_encryptHeaders;
    settings.volumeSizeMB = static_cast<uint64_t>(std::max(0, m_volumeSizeMB));
    const char *solid[] = {"", "on", "off", "e", "512m", "128m"};
    settings.solidBlockSize =
        m_solidBlockIndex == 6 ? m_customSolid : solid[std::clamp(m_solidBlockIndex, 0, 5)];
    settings.enablePreprocessors = m_enablePreprocessors;
    settings.enableAdaptivePruning = false;
    settings.autoShutdown = m_autoShutdown;
    settings.language = L::currentLang() ? "ru" : "en";

    settings.toolsBinDir = EmbeddedTools::ensureToolsReady();
    settings.customTempDir = utf8Path(m_tempPathBuffer);
    return settings;
}

void MainWindow::setInputPath(const std::string &path) {
    m_inputPathBuffer = path;
    if (m_outputPathBuffer.empty())
        m_outputPathBuffer = path + (m_createSfx ? ".ultra.exe" : ".ultra.7z");
}

void MainWindow::handleDragDrop(const char *path) {
    if (!m_settingsLocked)
        setInputPath(path);
}

std::string MainWindow::openFolderDialog() {
#ifdef _WIN32
    std::string result = "";
    IFileOpenDialog *pfd;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }
        if (SUCCEEDED(pfd->Show(NULL))) {
            IShellItem *psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    result = pathUtf8(std::filesystem::path(pszFilePath));
                    CoTaskMemFree(pszFilePath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return result;
#else
    char buffer[1024];
    std::string result = "";
    FILE *pipe = popen("zenity --file-selection --directory", "r");
    if (!pipe)
        return "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }
    pclose(pipe);
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
#endif
}

std::string MainWindow::openFileDialog() {
#ifdef _WIN32
    std::string result = "";
    IFileOpenDialog *pfd;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pfd)))) {
        if (SUCCEEDED(pfd->Show(NULL))) {
            IShellItem *psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    result = pathUtf8(std::filesystem::path(pszFilePath));
                    CoTaskMemFree(pszFilePath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return result;
#else
    char buffer[1024];
    std::string result = "";
    FILE *pipe = popen("zenity --file-selection", "r");
    if (!pipe)
        return "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }
    pclose(pipe);
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
#endif
}

bool MainWindow::scheduleShutdown(bool abort) {
#ifdef _WIN32
    wchar_t directory[32768]{};
    if (!GetSystemDirectoryW(directory, 32768))
        return false;
    ProcessControl control{nullptr, 5};
    auto result = ProcessManager::execute(
        pathUtf8(std::filesystem::path(directory) / "shutdown.exe"),
        abort ? std::vector<std::string>{"/a"} : std::vector<std::string>{"/s", "/t", "60"}, {},
        nullptr, 0, nullptr, nullptr, &control);
    return result.success;
#else
    (void)abort;
    return false;
#endif
}
std::string MainWindow::saveFileDialog(bool sfx) {
#ifdef _WIN32
    IFileSaveDialog *dialog = nullptr;
    std::string result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        COMDLG_FILTERSPEC filter{sfx ? L"Self-extracting archive" : L"7-Zip archive",
                                 sfx ? L"*.exe" : L"*.7z"};
        dialog->SetFileTypes(1, &filter);
        dialog->SetDefaultExtension(sfx ? L"exe" : L"7z");
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = pathUtf8(std::filesystem::path(path));
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    return result;
#else
    (void)sfx;
    return {};
#endif
}
void MainWindow::startOptimization() {
    if (m_settingsLocked || m_sessionExport.valid() || m_inputPathBuffer.empty())
        return;
    if (m_outputPathBuffer.empty()) {
        auto output = std::string(m_inputPathBuffer) + (m_createSfx ? ".ultra.exe" : ".ultra.7z");
        m_outputPathBuffer = output;
    }
    try {
        m_engine.setSettings(collectSettings());
        if (!m_engine.start(utf8Path(m_inputPathBuffer), utf8Path(m_outputPathBuffer)))
            std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                          m_engine.getLastError().c_str());
        applyState(m_engine.getState());
    } catch (const std::exception &e) {
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", e.what());
    }
}
void MainWindow::update() {
    if (!m_showWhatIf && m_preview.valid()) {
        m_previewCancel = true;
        if (m_preview.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            m_preview.get();
    }
    if (m_sessionExport.valid() &&
        m_sessionExport.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        bool ok = false;
        try {
            ok = m_sessionExport.get();
        } catch (...) {
        }
        std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s",
                      ok ? (L::currentLang() ? "Лидер сохранён" : "Leader exported")
                         : (L::currentLang()
                                ? "Экспорт не удался; проверьте пароль, инструменты и workspace"
                                : "Export failed; check password, tools and workspace"));
    }
    auto state = m_engine.getState();
    m_leaderboardWidget.commandsEnabled =
        (state == EngineState::TestingCandidates || state == EngineState::Paused);
    m_leaderboardWidget.retestEnabled =
        m_leaderboardWidget.commandsEnabled && m_engine.getSnapshot().activeTasks > 0;
    if (state != m_lastState)
        applyState(state);
}
void MainWindow::render() {
    update();
    Theme::applyDarkFluentStyle();

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, scaledSize(16, 16));
    ImGui::Begin("UltraArchStudioMainWindow", nullptr, windowFlags);

    renderHotkeys();
    renderTopBar();
    ImGui::Spacing();

    ImGui::BeginDisabled(m_settingsLocked || m_sessionExport.valid());
    renderInputConfig();
    ImGui::Spacing();

    renderSettingsBar();
    ImGui::Spacing();

    renderEncryptionSection();
    ImGui::Spacing();

    renderPresetSelector();
    ImGui::EndDisabled();
    ImGui::Spacing();

    renderControlButtons();
    ImGui::Spacing();

    renderProgressBar();
    const auto results = m_engine.getResults();
    uint64_t ramCandidates = 0, candidateFileBytes = 0;
    for (const auto &candidate : results) {
        if (candidate.storage == "ram") ++ramCandidates;
        candidateFileBytes += candidate.temporaryFileBytes;
    }
    ImGui::TextWrapped(localized(
        "Listed RAM candidates: %llu; logical candidate file volume: %.2f MiB.",
        "RAM-кандидатов в таблице: %llu; логический объём файлов кандидатов: %.2f МиБ."),
        static_cast<unsigned long long>(ramCandidates), static_cast<double>(candidateFileBytes) / 1048576.0);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36);
        ImGui::TextWrapped("%s", localized(
            "Counts cover listed candidates. Logical file volume sums recorded candidate archives, including subsequently deleted files. Nested group searches and probe files are reported separately. This is not process I/O or physical SSD writes.",
            "Показаны кандидаты из таблицы. Логический объём — сумма учтённых архивов кандидатов, включая позднее удалённые. Вложенные поиски по группам и файлы проб учитываются отдельно. Это не I/O процесса и не физическая запись SSD."));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::Spacing();

    // Telemetry updates
    m_telemetryWidget.update(static_cast<uint64_t>(std::max<int64_t>(64, m_ramLimitMB)));
    m_telemetryWidget.render();
    ImGui::Spacing();

    // Tabbed view: Leaderboard & System Log
    if (ImGui::BeginTabBar("MainTabBar")) {
        if (ImGui::BeginTabItem(L::tr(StringId::LeaderboardTitle))) {
            auto bestLeader = m_engine.getBestLeader();
            auto summary = m_engine.getSnapshot();
            const auto inputArchiveBytes = summary.sourceArchiveBytes;
            if (inputArchiveBytes) {
                ImGui::Text(localized("Original archive: %.2f GiB | Uncompressed content: %.2f GiB",
                                      "Исходный архив: %.2f GiB | Несжатые данные: %.2f GiB"),
                            static_cast<double>(inputArchiveBytes) / 1073741824.0,
                            static_cast<double>(summary.totalBytes) / 1073741824.0);
                if (bestLeader.taskId > 0 && bestLeader.outputBytes > 0)
                    ImGui::Text(localized("Archive size change: %+.2f%%", "Изменение размера архива: %+.2f%%"),
                                100.0 * (static_cast<double>(bestLeader.outputBytes) /
                                         static_cast<double>(inputArchiveBytes) - 1.0));
            }
            m_leaderboardWidget.render(results, bestLeader, summary.totalBytes);

            if (m_engine.getState() == EngineState::Completed && bestLeader.taskId > 0) {
                ImGui::Spacing();
                ImGui::TextWrapped("Winning flags: %s", bestLeader.flagsStr.c_str());
                for (const auto &file : m_engine.getOutputFiles())
                    ImGui::TextWrapped("Output: %s", pathUtf8(file).c_str());
                if (ImGui::Button("Copy flags"))
                    ImGui::SetClipboardText(bestLeader.flagsStr.c_str());
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(L::tr(StringId::LogTitle))) {
            renderLogConsole();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(L::currentLang() ? "Сессии / диагностика"
                                                 : "Sessions / diagnostics")) {
            renderSessions();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    renderCancelConfirmDialog();
    renderWhatIfDialog();

    ImGui::End();
    ImGui::PopStyleVar();
}

void MainWindow::renderHotkeys() {
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape) && m_settingsLocked) {
        m_showCancelConfirm = true;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && !m_settingsLocked)
        startOptimization();
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space)) {
        if (m_engine.getState() == EngineState::TestingCandidates)
            m_engine.pause();
        else if (m_engine.getState() == EngineState::Paused)
            m_engine.resume();
    }
}

void MainWindow::renderCancelConfirmDialog() {
    if (m_showCancelConfirm) {
        ImGui::OpenPopup(L::tr(StringId::ConfirmCancel));
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal(L::tr(StringId::ConfirmCancel), NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", L::tr(StringId::ConfirmCancelMsg));
        ImGui::Separator();

        if (ImGui::Button(L::tr(StringId::Yes), scaledSize(120, 0))) {
            m_engine.cancel();
            m_showCancelConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(L::tr(StringId::No), scaledSize(120, 0))) {
            m_showCancelConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void MainWindow::renderSessions() {
    if (!ImGui::CollapsingHeader(L::currentLang() ? "Сессии и восстановление"
                                                  : "Sessions and recovery"))
        return;
    auto base = m_tempPathBuffer.empty()
                    ? utf8Path(m_inputPathBuffer).parent_path() / ".ultraarch_tmp"
                    : utf8Path(m_tempPathBuffer);
    static std::vector<SessionInfo> history;
    static std::vector<std::filesystem::path> recovery;
    struct SessionDiskUsage { uint64_t bytes = 0; bool complete = true; };
    using SessionSizes = std::map<std::string, SessionDiskUsage>;
    static SessionSizes sessionSizes;
    static std::future<SessionSizes> sizeScan;
    if (sizeScan.valid() && sizeScan.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try { sessionSizes = sizeScan.get(); }
        catch (...) { sessionSizes.clear(); }
    }
    ImGui::BeginDisabled(sizeScan.valid());
    if (ImGui::Button(L::currentLang() ? "Обновить список" : "Refresh history")) {
        try {
            history = m_engine.sessions(std::filesystem::absolute(base));
            recovery = PublicationTransaction::pending(utf8Path(m_outputPathBuffer).parent_path());
            sessionSizes.clear();
            sizeScan = std::async(std::launch::async, [sessions = history] {
                SessionSizes sizes;
                for (const auto &session : sessions) {
                    auto &size = sizes[session.sessionId];
                    std::error_code ec;
                    if (!std::filesystem::is_directory(session.workspace, ec) ||
                        isLinkOrReparse(session.workspace)) { size.complete = false; continue; }
                    std::filesystem::recursive_directory_iterator it(session.workspace,
                        std::filesystem::directory_options::skip_permission_denied, ec), end;
                    size_t entries = 0;
                    while (!ec && it != end && entries++ < 100000) {
                        if (isLinkOrReparse(it->path())) {
                            it.disable_recursion_pending();
                            size.complete = false;
                        } else if (it->is_regular_file(ec)) {
                            const auto bytes = it->file_size(ec);
                            if (!ec && bytes <= std::numeric_limits<uint64_t>::max() - size.bytes)
                                size.bytes += bytes;
                            else size.complete = false;
                        }
                        it.increment(ec);
                    }
                    if (ec || it != end) size.complete = false;
                }
                return sizes;
            });
        } catch (const std::exception &e) {
            std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", e.what());
        }
    }
    ImGui::EndDisabled();
    if (sizeScan.valid())
        ImGui::TextDisabled("%s", localized("Measuring saved workspace sizes…", "Подсчёт размеров сохранённых рабочих папок…"));
    if (m_sessionExport.valid() &&
        ImGui::Button(L::currentLang() ? "Отменить экспорт" : "Cancel export"))
        m_exportCancel = true;
    ImGui::BeginDisabled(m_settingsLocked || m_sessionExport.valid());
    for (const auto &stage : recovery) {
        ImGui::PushID(pathUtf8(stage).c_str());
        ImGui::TextUnformatted(pathUtf8(stage).c_str());
        if (ImGui::Button(L::currentLang() ? "Восстановить прежний выход"
                                           : "Recover previous output")) {
            try {
                PublicationTransaction::recover(stage);
            } catch (const std::exception &e) {
                std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", e.what());
            }
        }
        ImGui::PopID();
    }
    std::string deletedSession;
    for (const auto &session : history) {
        ImGui::PushID(session.sessionId.c_str());
        ImGui::TextUnformatted(pathUtf8(session.inputPath).c_str());
        std::error_code workspaceError;
        const bool workspacePresent = std::filesystem::is_directory(session.workspace, workspaceError);
        ImGui::TextWrapped("%s", pathUtf8(session.workspace).c_str());
        ImGui::Text(localized("Started: %s | Last checkpoint: %s (local time)",
                              "Начало: %s | Последняя контрольная точка: %s (местное время)"),
                    sessionDate(session.startTime).c_str(), sessionDate(session.lastCheckpoint).c_str());
        const auto size = sessionSizes.find(session.sessionId);
        if (size != sessionSizes.end())
            ImGui::Text(localized("Saved workspace: %s%.2f GiB | Completed entries: %zu | %s",
                                  "Сохранённая рабочая папка: %s%.2f GiB | Завершённых записей: %zu | %s"),
                        size->second.complete ? "" : ">= ", static_cast<double>(size->second.bytes) / 1073741824.0,
                        session.completedTaskIds.size(), workspacePresent
                            ? localized("Available for recovery", "Доступна для восстановления")
                            : localized("Workspace missing", "Рабочая папка отсутствует"));
        if (ImGui::Button(L::currentLang() ? "Продолжить" : "Resume")) {
            if (m_engine.resumeSession(session, m_passwordBuffer)) {
                m_inputPathBuffer = pathUtf8(session.inputPath);
                m_outputPathBuffer = pathUtf8(session.outputPath);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(L::currentLang() ? "Сохранить лидера" : "Export leader")) {
            auto target = saveFileDialog(false);
            if (!target.empty()) {
                m_exportCancel = false;
                m_sessionExport =
                    std::async(std::launch::async,
                               [this, session, target, password = std::string(m_passwordBuffer)] {
                                   return m_engine.exportSessionLeader(session, utf8Path(target),
                                                                       password, &m_exportCancel);
                               });
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(localized("Delete this saved session…", "Удалить эту сохранённую сессию…")))
            ImGui::OpenPopup("DeleteSavedSession");
        if (ImGui::BeginPopupModal("DeleteSavedSession", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", localized(
                "Delete this session's owned workspace and its checkpoint? Saved group leaders in it will be lost.",
                "Удалить рабочую папку этой сессии и её контрольную точку? Сохранённые в ней лидеры групп будут потеряны."));
            ImGui::TextWrapped("%s", pathUtf8(session.workspace).c_str());
            if (ImGui::Button(localized("Delete selected session", "Удалить выбранную сессию"))) {
                if (m_engine.discardSession(session)) {
                    deletedSession = session.sessionId;
                    sessionSizes.erase(session.sessionId);
                    ImGui::CloseCurrentPopup();
                } else
                    std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", localized(
                        "Session was not deleted: workspace ownership or cleanup check failed.",
                        "Сессия не удалена: проверка принадлежности папки или удаление не выполнены."));
            }
            ImGui::SameLine();
            if (ImGui::Button(localized("Keep session", "Сохранить сессию")))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!deletedSession.empty())
        history.erase(std::remove_if(history.begin(), history.end(), [&](const auto &session) {
            return session.sessionId == deletedSession;
        }), history.end());
    ImGui::EndDisabled();
    auto state = m_engine.getState();
    if (state == EngineState::Completed || state == EngineState::Failed ||
        state == EngineState::Cancelled) {
        if (ImGui::Button(L::currentLang() ? "Экспорт диагностики JSON"
                                           : "Export diagnostic JSON")) {
            auto target = saveFileDialog(false);
            if (!target.empty())
                m_engine.saveRunReport(utf8Path(target));
        }
        ImGui::TextWrapped("%s", pathUtf8(m_engine.getLogPath()).c_str());
    }
}

void MainWindow::renderWhatIfDialog() {
    if (m_showWhatIf) {
        ImGui::OpenPopup(L::tr(StringId::BtnWhatIf));
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    auto available = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(scaled(800), available.x * 0.9f),
                                    std::min(scaled(600), available.y * 0.9f)),
                             ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal(L::tr(StringId::BtnWhatIf), &m_showWhatIf)) {
        if (!m_previewReady && !m_preview.valid()) {
            auto input = utf8Path(m_inputPathBuffer);
            auto settings = collectSettings();
            m_previewCancel = false;
            m_previewTasks.clear();
            m_preview = std::async(std::launch::async, [this, input, settings] {
                PreviewResult result;
                try {
                    ContentAnalyzer analyzer;
                    auto summary =
                        ArchiveExtractor::isArchive(input)
                            ? ArchiveExtractor::preview(input, settings.toolsBinDir,
                                                        &m_previewCancel, settings.password)
                            : analyzer.analyze(input, false, &m_previewCancel);
                    if (!summary.totalFiles)
                        throw std::runtime_error("Select a readable input first");
                    TaskMatrixGenerator generator;
                    result.tasks = generator.generateMatrix(summary, settings);
                    result.message = ArchiveExtractor::isArchive(input)
                                         ? "Archive listing estimate; no extraction or content "
                                           "hash was performed"
                                         : "Candidate estimates; no compression was performed";
                } catch (const std::exception &e) {
                    result.message = e.what();
                }
                return result;
            });
        }
        if (m_preview.valid() &&
            m_preview.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = m_preview.get();
            m_previewTasks = std::move(result.tasks);
            m_previewReady = true;
            std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", result.message.c_str());
        }
        if (!m_previewReady)
            ImGui::TextUnformatted(L::currentLang() ? "Анализ в фоне..."
                                                    : "Analyzing in background...");
        ImGui::TextWrapped("%s", m_statusMessage);
        if (ImGui::BeginTable("whatif_table", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Preset");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableSetupColumn("Estimated RAM (MiB)");
            ImGui::TableSetupColumn(L::currentLang() ? "История LZMA: сжатие / распаковка (КиБ)" :
                                                      "LZMA history: encode / decode (KiB)");
            ImGui::TableHeadersRow();
            for (const auto &task : m_previewTasks) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(task.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(redactFlags(task.flags).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", static_cast<unsigned long long>(task.estimatedRamMB));
                ImGui::TableSetColumnIndex(3);
                for (const auto &method : task.spec.methods)
                    if (method.codec == Codec::LZMA1 || method.codec == Codec::LZMA2)
                        ImGui::Text("%llu / %llu", static_cast<unsigned long long>(method.dictionary.value / 1024),
                            static_cast<unsigned long long>(DictionaryPolicy::decoderBytes(method) / 1024));
            }
            ImGui::EndTable();
        }

        if (ImGui::Button(L::currentLang() ? "Закрыть / отменить" : "Close / cancel")) {
            m_previewCancel = true;
            m_showWhatIf = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void MainWindow::renderTopBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::ChildBg);
    ImGui::BeginChild("TopBar", ImVec2(0, ImGui::GetFrameHeight() + scaled(24)), true,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPos(scaledSize(12, 12));
    ImGui::TextColored(Theme::Colors::Accent, "%s", L::tr(StringId::AppTitle));
    ImGui::SameLine();
    ImGui::TextColored(Theme::Colors::TextSecondary, "| Repack Engine Pro");

    ImGui::SameLine(std::max(scaled(250), ImGui::GetWindowWidth() - scaled(400)));
    if (ImGui::Button(L::currentLang() == 0 ? "EN" : "RU", scaledSize(40, 0))) {
        L::setLanguage(L::currentLang() == 0 ? "ru" : "en");
    }

    ImGui::SameLine();
    EngineState state = m_engine.getState();
    if (state == EngineState::TestingCandidates) {
        ImGui::TextColored(Theme::Colors::Accent, "● %s", L::tr(StringId::StatusTesting));
    } else if (state == EngineState::AssemblingFinal) {
        ImGui::TextColored(Theme::Colors::LeaderGreen, "● %s", L::tr(StringId::StatusAssembling));
    } else if (state == EngineState::Completed) {
        ImGui::TextColored(Theme::Colors::LeaderGreen, "✔ %s", L::tr(StringId::StatusCompleted));
    } else if (state == EngineState::Paused) {
        ImGui::TextColored(Theme::Colors::WarningOrange, "⏸ %s", L::tr(StringId::StatusPaused));
    } else {
        ImGui::TextColored(Theme::Colors::TextSecondary, "○ %s", L::tr(StringId::StatusIdle));
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void MainWindow::renderInputConfig() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::CardBg);
    ImGui::BeginChild("InputConfigCard", scaledSize(0, 135), true, ImGuiWindowFlags_NoScrollbar);

    // 1. Target Source Path
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::Colors::TextSecondary, "%s:", L::tr(StringId::InputPath));
    ImGui::SameLine(scaled(130));
    ImGui::SetNextItemWidth(std::max(scaled(100), ImGui::GetWindowWidth() - scaled(320)));
    ImGui::InputTextWithHint("##SourceInput", L::tr(StringId::DropFileHere), &m_inputPathBuffer);
    ImGui::SameLine();
    if (ImGui::Button("📂##Input", scaledSize(30, 0))) {
        std::string res = openFileDialog();
        if (!res.empty())
            setInputPath(res);
    }
    ImGui::SameLine();
    if (ImGui::Button(L::currentLang() ? "Папка" : "Folder", scaledSize(90, 0))) {
        auto selected = openFolderDialog();
        if (!selected.empty())
            setInputPath(selected);
    }

    // 2. Output Archive Path
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::Colors::TextSecondary, "%s:", L::tr(StringId::OutputPath));
    ImGui::SameLine(scaled(130));
    ImGui::SetNextItemWidth(std::max(scaled(100), ImGui::GetWindowWidth() - scaled(320)));
    ImGui::InputTextWithHint("##OutputInput", "", &m_outputPathBuffer);
    ImGui::SameLine();
    if (ImGui::Button("📂##Output", scaledSize(30, 0))) {
        std::string res = saveFileDialog(m_createSfx);
        if (!res.empty())
            m_outputPathBuffer = res;
    }

    // 3. Scratch / Temp Workspace (Avoid C: exhaustion)
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::Colors::TextSecondary, "%s:", L::tr(StringId::TempWorkspace));
    ImGui::SameLine(scaled(130));
    ImGui::SetNextItemWidth(std::max(scaled(100), ImGui::GetWindowWidth() - scaled(320)));
    ImGui::InputTextWithHint("##TempInput", "", &m_tempPathBuffer);
    ImGui::SameLine();
    if (ImGui::Button("📂##Temp", scaledSize(30, 0))) {
        std::string res = openFolderDialog();
        if (!res.empty())
            m_tempPathBuffer = res;
    }
    ImGui::SameLine();

    // Query and display free space on the selected temp partition (cached every 1s)
    static auto s_lastSpaceCheck = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    static uintmax_t s_cachedAvailable = 0;
    static bool s_hasSpaceInfo = false;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastSpaceCheck).count() >=
        1000) {
        s_lastSpaceCheck = now;
        std::error_code sec;
        std::filesystem::path checkPath =
            (!m_tempPathBuffer.empty())
                ? utf8Path(m_tempPathBuffer)
                : (!m_inputPathBuffer.empty() ? utf8Path(m_inputPathBuffer)
                                              : std::filesystem::current_path(sec));
        if (!sec && !checkPath.empty()) {
            auto sinfo = std::filesystem::space(checkPath, sec);
            if (!sec && sinfo.available > 0) {
                s_cachedAvailable = sinfo.available;
                s_hasSpaceInfo = true;
            } else {
                s_hasSpaceInfo = false;
            }
        } else {
            s_hasSpaceInfo = false;
        }
    }

    if (s_hasSpaceInfo && s_cachedAvailable > 0) {
        double freeGB = static_cast<double>(s_cachedAvailable) / (1024.0 * 1024.0 * 1024.0);
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << freeGB << " GiB " << L::tr(StringId::FreeSpace);
        ImGui::TextColored(Theme::Colors::LeaderGreen, "%s", ss.str().c_str());
    } else {
        ImGui::TextColored(Theme::Colors::TextSecondary,
                           localized("Auto-Drive", "Автовыбор диска"));
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void MainWindow::renderRamSettings() {
    ImGui::Separator();
    ImGui::Checkbox(localized("Test small groups in RAM", "Тестировать маленькие группы в RAM"),
                    &m_additionalSettings.enableRamCandidates);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", localized(
            "Keeps trial archives in memory to reduce temporary disk writes. Saved winners remain recoverable.",
            "Пробные архивы хранятся в памяти, уменьшая временные записи на диск. Сохранённые победители доступны для восстановления."));

    ImGui::BeginDisabled(!m_additionalSettings.enableRamCandidates);
    int threshold = static_cast<int>(std::clamp<uint64_t>(
        m_additionalSettings.ramGroupLimitMB, 1, 256));
    ImGui::TextUnformatted(localized("Maximum uncompressed group size", "Максимальный несжатый размер группы"));
    const float sliderWidth = std::min(scaled(340), std::max(scaled(80),
        ImGui::GetContentRegionAvail().x - scaled(120)));
    ImGui::SetNextItemWidth(sliderWidth);
    ImGui::SliderInt("##RamGroupThreshold", &threshold, 1, 256, "%d MiB",
                     ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    flowNext(110);
    ImGui::SetNextItemWidth(scaled(100));
    ImGui::InputInt("##RamGroupThresholdExact", &threshold, 1, 16);
    m_additionalSettings.ramGroupLimitMB = static_cast<uint64_t>(std::clamp(threshold, 1, 256));

    const int budgetMax = static_cast<int>(std::clamp<int64_t>(m_ramLimitMB, 32, 4096));
    int budget = static_cast<int>(std::clamp<uint64_t>(
        m_additionalSettings.ramTemporaryBudgetMB, 32, static_cast<uint64_t>(budgetMax)));
    ImGui::SetNextItemWidth(sliderWidth);
    ImGui::SliderInt(localized("Temporary RAM budget (MiB)", "Бюджет временной RAM (МиБ)"),
                     &budget, 32, budgetMax, "%d", ImGuiSliderFlags_AlwaysClamp);
    m_additionalSettings.ramTemporaryBudgetMB = static_cast<uint64_t>(budget);
    ImGui::EndDisabled();

    // Estimate from settings only: preview generation and tool discovery must not run per frame.
    auto estimateSettings = m_additionalSettings;
    estimateSettings.profile = static_cast<OptimizationProfile>(m_selectedProfileIndex);
    estimateSettings.ramLimitMB = static_cast<uint64_t>(std::max<int64_t>(64, m_ramLimitMB));
    estimateSettings.workerThreads = static_cast<uint32_t>(std::clamp(m_workerThreads, 1, 64));
    if (estimateSettings.autoCpu) {
        estimateSettings.cpuThreads = previewAvailableCpuThreads(estimateSettings.affinityMode);
        estimateSettings.workerThreads = std::clamp(estimateSettings.cpuThreads / 2, 1u, 64u);
    }
    const auto estimate = RamPolicy::estimate(estimateSettings,
        estimateSettings.ramGroupLimitMB * 1024 * 1024);
    ImGui::TextWrapped(localized(
        "Estimate for %llu MiB: one active task ~%llu MiB RAM; %u concurrent ~%llu MiB.",
        "Оценка для %llu МиБ: одна активная задача ~%llu МиБ RAM; %u одновременно ~%llu МиБ."),
        static_cast<unsigned long long>(estimateSettings.ramGroupLimitMB),
        static_cast<unsigned long long>(estimate.perTaskMB), estimate.slots,
        static_cast<unsigned long long>(estimate.concurrentMB));
    ImGui::TextWrapped(localized(
        "Per task: compression ~%llu MiB + temporary data ~%llu MiB. This is a reservation estimate, not measured usage.",
        "На задачу: сжатие ~%llu МиБ + временные данные ~%llu МиБ. Это оценка резерва, а не измеренный расход."),
        static_cast<unsigned long long>(estimate.codecMB),
        static_cast<unsigned long long>(estimate.temporaryMB));
    ImGui::TextWrapped("%s", localized(
        "Temporary RAM is part of the total memory limit. Groups that exceed the threshold or available budget use disk.",
        "Временная RAM входит в общий лимит памяти. Группы сверх порога или доступного бюджета обрабатываются на диске."));
    if (m_additionalSettings.enableRamCandidates && !estimate.fits)
        ImGui::TextWrapped("%s", localized(
            "A group at this size does not fit the current reservation: disk fallback will be used.",
            "Группа такого размера не помещается в текущий резерв: будет использован диск."));
}

void MainWindow::renderSettingsBar() {
    const bool stacked = ImGui::GetContentRegionAvail().x < scaled(1000);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::CardBg);
    const float panelHeight = ImGui::GetTextLineHeightWithSpacing() +
                              5 * ImGui::GetFrameHeightWithSpacing() +
                              2 * ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("SettingsCard", ImVec2(0, panelHeight * (stacked ? 3 : 1)), true,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::Columns(stacked ? 1 : 3, "SettingsCols", false);

    // Column 1: Profile Selector
    ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::Profile));
    const char *profileNames[] = {L::tr(StringId::ProfileWinRAR), L::tr(StringId::ProfileDeep),
                                  L::tr(StringId::ProfileExtreme)};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##ProfileSelector", &m_selectedProfileIndex, profileNames,
                 IM_ARRAYSIZE(profileNames));

    ImGui::Checkbox(L::tr(StringId::EnablePreprocessors), &m_enablePreprocessors);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Leanify/ECT change stored resource bytes and may remove metadata. "
                          "Original files stay unchanged.");
    if (!stacked)
        ImGui::SameLine();
    bool adaptive = m_additionalSettings.searchMode != SearchMode::Exhaustive;
    if (ImGui::Checkbox(L::tr(StringId::EnableAdaptivePruning), &adaptive))
        m_additionalSettings.searchMode = adaptive ? SearchMode::Balanced : SearchMode::Exhaustive;
    ImGui::NextColumn();

    // Column 2: WinAPI Resource Limits (RAM & CPU)
    ImGui::TextColored(Theme::Colors::TextSecondary,
                       localized("WINAPI RESOURCE GUARD", "ОГРАНИЧЕНИЯ РЕСУРСОВ"));
    ImGui::SetNextItemWidth(-1);

    ImGui::InputScalar((std::string(L::tr(StringId::RamLimit)) + " (MiB)").c_str(),
                       ImGuiDataType_S64, &m_ramLimitMB);

    ImGui::Checkbox(localized("Auto CPU (2 threads per task)", "Авто CPU (2 потока на задачу)"),
                    &m_additionalSettings.autoCpu);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", localized(
            "Uses currently available CPUs and the selected affinity policy. RAM and codec limits can reduce concurrency; manual values are preserved.",
            "Учитывает доступные CPU и выбранную привязку. RAM и ограничения кодека могут снизить число задач; ручные значения сохраняются."));
    ImGui::SetNextItemWidth(-1);
    std::string workerStr = std::string(localized("Concurrent tasks", "Одновременные задачи")) + ": %d";
    ImGui::BeginDisabled(m_additionalSettings.autoCpu);
    ImGui::SliderInt("##CpuWorkers", &m_workerThreads, 1, 64, workerStr.c_str());
    ImGui::EndDisabled();
    ImGui::NextColumn();

    // Column 3: Feature Toggles
    ImGui::TextColored(Theme::Colors::TextSecondary,
                       localized("COMPRESSION POLICY", "ПОЛИТИКА СЖАТИЯ"));
    ImGui::Checkbox(L::tr(StringId::ZeroWaste), &m_zeroWasteCleaner);
    ImGui::SameLine();
    ImGui::Checkbox(L::tr(StringId::VerifyArchives), &m_verifyArchives);
    if (ImGui::Checkbox(L::tr(StringId::PortableSfx), &m_createSfx)) {
        auto automatic7z = std::string(m_inputPathBuffer) + ".ultra.7z";
        auto automaticSfx = std::string(m_inputPathBuffer) + ".ultra.exe";
        if (m_outputPathBuffer == automatic7z || m_outputPathBuffer == automaticSfx)
            m_outputPathBuffer = m_createSfx ? automaticSfx : automatic7z;
    }
    ImGui::SameLine();
    ImGui::Checkbox(L::tr(StringId::BtnAutoShutdown), &m_autoShutdown);

    ImGui::Columns(1);
    const auto topology = ResourceGuard::detectCpuTopology();
    const auto available = previewAvailableCpuThreads(m_additionalSettings.affinityMode);
    ImGui::TextWrapped(localized("CPU: %u physical / %u logical; available: %u; Auto: up to %u concurrent tasks.",
                                "CPU: %u физических / %u логических; доступно: %u; Авто: до %u одновременных задач."),
                        topology.physicalCores, topology.logicalProcessors, available,
                        std::clamp(available / 2, 1u, 64u));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    renderRamSettings();
    if (ImGui::CollapsingHeader(L::currentLang() ? "Расширенные настройки" : "Advanced settings")) {
        const char *verifiers[] = {
            localized("Disk", "Распаковка на диск"),
            localized("Dual (worker + disk)", "Двойная (worker + диск)"),
            localized("Streaming (disk fallback)", "Потоковая (резерв: диск)")};
        int verifier = static_cast<int>(m_additionalSettings.verificationMode);
        if (ImGui::Combo(localized("Content verifier", "Проверка содержимого"), &verifier,
                         verifiers, 3))
            m_additionalSettings.verificationMode = static_cast<VerificationMode>(verifier);
        ImGui::BeginDisabled(m_additionalSettings.autoCpu);
        ImGui::InputScalar(localized("Total CPU thread budget (0=auto)", "Общий бюджет потоков CPU (0=авто)"),
                           ImGuiDataType_U32, &m_additionalSettings.cpuThreads);
        ImGui::EndDisabled();
        ImGui::InputScalar(localized("Maximum LZMA dictionary (MiB, 0=auto)",
                                    "Максимум словаря LZMA (МиБ, 0=авто)"),
                           ImGuiDataType_U64, &m_additionalSettings.maxLzmaDictionaryMB);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", localized(
                "Ceiling for compression history. Actual dictionaries also fit solid data and RAM. Decoder properties can round up.",
                "Предел истории сжатия. Реальный словарь ограничен данными solid-потока и RAM. При распаковке размер может округляться вверх."));
        ImGui::Checkbox(localized("Additional content-similarity groups (experimental)",
                                 "Дополнительные группы по сходству содержимого (эксперимент)"),
                        &m_additionalSettings.enableSimilarityGroups);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", localized(
                "Used with Extreme grouping. Adds search time; the original grouping remains a competitor.",
                "Работает в Extreme с группировкой. Добавляет время поиска; исходные группы остаются конкурентом."));
        ImGui::InputScalar(localized("Process timeout (seconds)", "Тайм-аут процесса (сек.)"),
                           ImGuiDataType_U32, &m_additionalSettings.processTimeoutSeconds);
        ImGui::InputScalar(localized("Search time budget (seconds)", "Бюджет поиска (сек.)"),
                           ImGuiDataType_U32, &m_additionalSettings.timeBudgetSeconds);
        ImGui::InputScalar(localized("Parameter-search sample (MiB)", "Сэмпл поиска параметров (МиБ)"),
                          ImGuiDataType_U32, &m_additionalSettings.probeSampleMB);
        m_additionalSettings.probeSampleMB = std::clamp(m_additionalSettings.probeSampleMB, 1u, 256u);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", localized(
                "Sample size for the preliminary parameter tournament; independent of the archive solid block size. Finalists compress the full group.",
                "Размер сэмпла предварительного перебора параметров; не размер solid-блока архива. Финалисты сжимают всю группу."));
        ImGui::InputScalar(localized("Sample tournament budget (seconds)", "Бюджет перебора на сэмплах (сек.)"),
                          ImGuiDataType_U32, &m_additionalSettings.probeBudgetSeconds);
        m_additionalSettings.probeBudgetSeconds = std::clamp(m_additionalSettings.probeBudgetSeconds, 1u, 3600u);
        ImGui::InputScalar(localized("Sample process timeout (seconds)", "Тайм-аут пробы (сек.)"),
                          ImGuiDataType_U32, &m_additionalSettings.probeTimeoutSeconds);
        m_additionalSettings.probeTimeoutSeconds = std::clamp(m_additionalSettings.probeTimeoutSeconds, 1u, 600u);
        ImGui::InputScalar(localized("Sample passes (1–4)", "Проходы на сэмплах (1–4)"),
                          ImGuiDataType_U32, &m_additionalSettings.probePasses);
        m_additionalSettings.probePasses = std::clamp(m_additionalSettings.probePasses, 1u, 4u);
        ImGui::InputScalar(localized("Explicit full-attempt limit (0=unlimited)", "Явный лимит полных попыток (0=без лимита)"),
                          ImGuiDataType_U32, &m_additionalSettings.fullAttemptLimit);
        m_additionalSettings.fullAttemptLimit = std::min(m_additionalSettings.fullAttemptLimit, 10000u);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", localized(
                "Explicit full-data attempt limit per search; each group has its own budget. Excludes the original archive and group orchestration. Also applies to Full when nonzero.",
                "Явный лимит полных попыток на каждый поиск; у каждой группы свой бюджет. Исходный архив и управляющая задача групп не учитываются. Ненулевое значение ограничивает и Полный поиск."));
        const char *modes[] = {localized("Fast", "Быстрый"),
                               localized("Balanced", "Сбалансированный"),
                               localized("Exhaustive", "Полный")};
        int mode = static_cast<int>(m_additionalSettings.searchMode);
        if (ImGui::Combo(localized("Search mode", "Режим поиска"), &mode, modes, 3))
            m_additionalSettings.searchMode = static_cast<SearchMode>(mode);
        const char *goals[] = {localized("Minimum size", "Минимальный размер"),
                               localized("Fast within tolerance", "Быстрее в пределах допуска")};
        int goal = static_cast<int>(m_additionalSettings.selectionGoal);
        if (ImGui::Combo(localized("Goal", "Цель"), &goal, goals, 2))
            m_additionalSettings.selectionGoal = static_cast<SelectionGoal>(goal);
        ImGui::InputDouble(localized("Size tolerance (%)", "Допуск размера (%)"),
                           &m_additionalSettings.sizeTolerancePercent);
        const char *policies[] = {localized("Exact bytes", "Точные байты"),
                                  localized("Optimize resources", "Оптимизация ресурсов")};
        int policy = static_cast<int>(m_additionalSettings.contentPolicy);
        if (ImGui::Combo(localized("Content policy", "Политика содержимого"), &policy, policies, 2))
            m_additionalSettings.contentPolicy = static_cast<ContentPolicy>(policy);
        const char *affinities[] = {
            localized("Windows scheduler", "Планировщик Windows"),
            localized("Physical core affinity", "Привязка к физическим ядрам"),
            localized("CPU Sets (experimental)", "CPU Sets (экспериментально)")};
        int affinity = static_cast<int>(m_additionalSettings.affinityMode);
        if (ImGui::Combo(localized("CPU scheduling", "Планирование CPU"), &affinity, affinities, 3))
            m_additionalSettings.affinityMode = static_cast<AffinityMode>(affinity);
        ImGui::Checkbox(localized("Background priority", "Фоновый приоритет"),
                        &m_additionalSettings.backgroundPriority);
        ImGui::Checkbox(localized("Grouped method search (Extreme)", "Поиск методов по группам (Extreme)"),
                        &m_additionalSettings.enableGrouping);
        if (m_additionalSettings.enableGrouping &&
            m_selectedProfileIndex == static_cast<int>(OptimizationProfile::Extreme_RePack))
            ImGui::Checkbox(localized("Only groups (skip whole-input search)", "Только группы (без общего поиска)"),
                            &m_additionalSettings.groupedSearchOnly);
        else
            m_additionalSettings.groupedSearchOnly = false;
        ImGui::Checkbox(localized("Whole-input order by type", "Общий поиск: порядок по типам"),
                        &m_additionalSettings.sortByType);
        ImGui::InputScalar(localized("CPU hard cap % (0=off)", "Лимит CPU % (0=выкл.)"),
                           ImGuiDataType_U32, &m_additionalSettings.cpuLimitPercent);
        ImGui::InputScalar(localized("Concurrent disk tasks (0=workers)", "Дисковые задачи (0=число работников)"),
                           ImGuiDataType_U32, &m_additionalSettings.diskTaskLimit);
        ImGui::InputScalar(localized("Extraction total limit (MiB)", "Лимит распаковки (MiB)"),
                           ImGuiDataType_U64, &m_additionalSettings.extractionLimitMB);
        ImGui::InputScalar(localized("Extraction file limit (MiB)", "Лимит файла (MiB)"),
                           ImGuiDataType_U64, &m_additionalSettings.extractionFileLimitMB);
        ImGui::InputScalar(localized("Extraction entry limit", "Лимит элементов"),
                           ImGuiDataType_U32, &m_additionalSettings.extractionFileCount);
    }
}

void MainWindow::renderEncryptionSection() {
    const bool stacked = ImGui::GetContentRegionAvail().x < scaled(1000);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::CardBg);
    const float panelHeight = ImGui::GetTextLineHeightWithSpacing() +
                              2 * ImGui::GetFrameHeightWithSpacing() +
                              2 * ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild("EncryptCard", ImVec2(0, panelHeight * (stacked ? 3 : 1)), true,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::Columns(stacked ? 1 : 3, "EncryptCols", false);

    ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::Encryption));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##Password", L::tr(StringId::Password), m_passwordBuffer,
                             sizeof(m_passwordBuffer), ImGuiInputTextFlags_Password);
    ImGui::Checkbox(L::tr(StringId::EncryptHeaders), &m_encryptHeaders);
    ImGui::NextColumn();

    ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::VolumeSize));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##VolumeSize", &m_volumeSizeMB);
    if (m_volumeSizeMB < 0)
        m_volumeSizeMB = 0;
    ImGui::NextColumn();

    ImGui::TextColored(Theme::Colors::TextSecondary, "%s", L::tr(StringId::SolidBlock));
    const char *solidBlocks[] = {localized("Auto", "Авто"),
                                 localized("On", "Вкл."),
                                 localized("Off", "Выкл."),
                                 localized("By Extension", "По расширению"),
                                 "512MB",
                                 "128MB",
                                 localized("Custom", "Пользовательский")};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##SolidBlock", &m_solidBlockIndex, solidBlocks, IM_ARRAYSIZE(solidBlocks));
    if (m_solidBlockIndex == 6)
        ImGui::InputText("##CustomSolid", &m_customSolid);

    ImGui::Columns(1);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void MainWindow::renderPresetSelector() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::CardBg);
    ImGui::BeginChild("PresetCard", scaledSize(0, 60), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(Theme::Colors::TextSecondary,
                       localized("Selected preset (click Load)", "Выбранный пресет (нажмите «Загрузить»)"));
    ImGui::SameLine();

    auto presets = m_presetManager.getAllPresets();
    static int currentPreset = 0;
    std::vector<const char *> presetNames;
    for (const auto &p : presets)
        presetNames.push_back(p.name.c_str());

    ImGui::SetNextItemWidth(scaled(200));
    if (!presetNames.empty()) {
        ImGui::Combo("##Presets", &currentPreset, presetNames.data(), presetNames.size());
    } else {
        ImGui::Text("No presets");
    }

    ImGui::SameLine();
    if (ImGui::Button(L::tr(StringId::PresetSave))) {
        ImGui::OpenPopup("SavePresetPopup");
    }
    ImGui::SameLine();
    if (ImGui::Button(L::tr(StringId::PresetLoad))) {
        if (currentPreset >= 0 && static_cast<size_t>(currentPreset) < presets.size()) {
            PresetInfo pi;
            if (m_presetManager.loadPreset(presets[currentPreset].name, pi)) {
                m_additionalSettings = normalizeSearchSettings(pi.settings);
                m_ramLimitMB = static_cast<int64_t>(pi.settings.ramLimitMB);
                m_selectedProfileIndex = static_cast<int>(pi.settings.profile);
                m_enablePreprocessors = pi.settings.enablePreprocessors;
                m_autoShutdown = pi.settings.autoShutdown;
                L::setLanguage(pi.settings.language);
                const char *solids[] = {"", "on", "off", "e", "512m", "128m"};
                m_solidBlockIndex = 6;
                m_customSolid = pi.settings.solidBlockSize;
                for (int i = 0; i < 6; ++i)
                    if (pi.settings.solidBlockSize == solids[i])
                        m_solidBlockIndex = i;
                m_tempPathBuffer = pathUtf8(pi.settings.customTempDir);
                m_workerThreads = pi.settings.workerThreads;
                m_verifyArchives = pi.settings.verifyArchives;
                m_zeroWasteCleaner = pi.settings.immediateCleanInferior;
                m_createSfx = pi.settings.sfxOutput;
                std::strncpy(m_passwordBuffer, pi.settings.password.c_str(),
                             sizeof(m_passwordBuffer) - 1);
                m_encryptHeaders = pi.settings.encryptHeaders;
                m_volumeSizeMB = pi.settings.volumeSizeMB;
                if (!pi.migrationWarning.empty())
                    std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", !L::currentLang()
                        ? pi.migrationWarning.c_str()
                        : pi.migrationWarning.find("ignored") != std::string::npos
                            ? "Старый переключатель адаптивной обрезки игнорируется: явно выбранный режим поиска имеет приоритет."
                            : "Старый адаптивный пресет переведён в Сбалансированный поиск. Проверьте режим перед запуском.");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(L::tr(StringId::PresetDelete))) {
        if (currentPreset >= 0 && static_cast<size_t>(currentPreset) < presets.size() &&
            !presets[currentPreset].isBuiltin) {
            m_presetManager.deletePreset(presets[currentPreset].name);
        }
    }
    if (currentPreset >= 0 && static_cast<size_t>(currentPreset) < presets.size()) {
        // Compare visible settings without collectSettings(), which discovers embedded tools.
        auto current = m_additionalSettings;
        current.profile = static_cast<OptimizationProfile>(m_selectedProfileIndex);
        current.ramLimitMB = static_cast<uint64_t>(std::max<int64_t>(0, m_ramLimitMB));
        current.workerThreads = static_cast<uint32_t>(m_workerThreads);
        current.verifyArchives = m_verifyArchives;
        current.immediateCleanInferior = m_zeroWasteCleaner;
        current.sfxOutput = m_createSfx;
        current.password = m_passwordBuffer;
        current.encryptHeaders = m_encryptHeaders;
        current.volumeSizeMB = static_cast<uint64_t>(std::max(0, m_volumeSizeMB));
        const char *solid[] = {"", "on", "off", "e", "512m", "128m"};
        current.solidBlockSize = m_solidBlockIndex == 6 ? m_customSolid : solid[std::clamp(m_solidBlockIndex, 0, 5)];
        current.enablePreprocessors = m_enablePreprocessors;
        current.enableAdaptivePruning = false;
        current.autoShutdown = m_autoShutdown;
        current.language = L::currentLang() ? "ru" : "en";
        current.customTempDir = utf8Path(m_tempPathBuffer);
        const bool matches = settingsJson(normalizeSearchSettings(current)) ==
                             settingsJson(normalizeSearchSettings(presets[currentPreset].settings));
        ImGui::TextDisabled("%s", matches
            ? localized("Current settings match selected preset.", "Текущие настройки совпадают с выбранным пресетом.")
            : localized("Current settings differ; selecting a preset does not apply it until Load.",
                        "Текущие настройки отличаются; выбор пресета применяется только кнопкой «Загрузить»."));
    }

    if (ImGui::BeginPopup("SavePresetPopup")) {
        ImGui::InputText(L::tr(StringId::PresetName), m_presetNameBuffer,
                         sizeof(m_presetNameBuffer));
        ImGui::InputText(L::tr(StringId::PresetDesc), m_presetDescBuffer,
                         sizeof(m_presetDescBuffer));
        if (ImGui::Button("Save")) {
            PresetInfo pi;
            pi.name = m_presetNameBuffer;
            pi.description = m_presetDescBuffer;
            pi.isBuiltin = false;

            pi.settings = collectSettings();

            m_presetManager.savePreset(pi);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void MainWindow::renderControlButtons() {
    EngineState state = m_engine.getState();
    bool isRunning =
        (state == EngineState::Analyzing || state == EngineState::Extracting ||
         state == EngineState::Preprocessing || state == EngineState::TestingCandidates ||
         state == EngineState::AssemblingFinal);
    bool isPaused = (state == EngineState::Paused);

    // 1. Start Button
    if (!isRunning && !isPaused) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Colors::AccentHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::AccentActive);
        if (ImGui::Button(L::tr(StringId::BtnStart), scaledSize(220, 36)))
            startOptimization();
        ImGui::PopStyleColor(3);
    } else {
        // Stop / Cancel button
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ErrorRed);
        if (ImGui::Button(L::tr(StringId::BtnCancel), scaledSize(120, 36))) {
            m_showCancelConfirm = true;
        }
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginPopupModal(L::tr(StringId::DiskSpaceWarning))) {
        ImGui::Text("%s", L::tr(StringId::DiskSpaceError));
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    flowNext();

    // 2. Pause / Resume Button
    if (isRunning) {
        if (ImGui::Button(L::tr(StringId::BtnPause), scaledSize(110, 36))) {
            m_engine.pause();
        }
    } else if (isPaused) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::WarningOrange);
        if (ImGui::Button(L::tr(StringId::BtnResume), scaledSize(110, 36))) {
            m_engine.resume();
        }
        ImGui::PopStyleColor();
    }

    flowNext();

    // Skip always names a leaf or a group; never silently cancels the grouped wrapper.
    if (isRunning || isPaused) {
        if (ImGui::Button(L::tr(StringId::BtnSkip), scaledSize(180, 36))) {
            ImGui::OpenPopup("SkipNamedTask");
        }
        if (ImGui::BeginPopup("SkipNamedTask")) {
            const auto snapshot = m_engine.getSnapshot();
            ImGui::TextUnformatted(localized("Skip candidate", "Пропустить кандидата"));
            if (snapshot.activeCandidates.empty())
                ImGui::TextDisabled("%s", localized("No active candidate", "Нет активного кандидата"));
            std::vector<std::string> groups;
            for (const auto &candidate : snapshot.activeCandidates) {
                const auto id = candidate.group + "/" + candidate.configId;
                ImGui::PushID(id.c_str());
                const auto label = candidate.group.empty() ? candidate.name
                                                          : candidate.group + ": " + candidate.name;
                if (ImGui::Selectable(label.c_str()))
                    m_engine.skipCandidate(candidate.group, candidate.configId);
                ImGui::PopID();
                if (!candidate.group.empty() &&
                    std::find(groups.begin(), groups.end(), candidate.group) == groups.end())
                    groups.push_back(candidate.group);
            }
            if (!groups.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(localized("Stop search in group", "Остановить поиск в группе"));
                for (const auto &group : groups) {
                    const auto label = group + "##StopGroup";
                    if (ImGui::Selectable(label.c_str()))
                        m_engine.skipGroup(group);
                }
                ImGui::TextDisabled("%s", localized("Verified group leaders are retained.",
                                                    "Проверенные лидеры групп сохраняются."));
            }
            ImGui::EndPopup();
        }
    }

    flowNext();

    if (state == EngineState::TestingCandidates) {
        if (ImGui::Button(localized("Start next candidate", "Запустить следующего"), scaledSize(220, 36))) {
            const bool accepted = m_engine.startNextCandidate();
            std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", accepted
                ? localized("Additional admission requested; the candidate start is recorded in the log.",
                            "Дополнительный запуск запрошен; старт кандидата записывается в журнал.")
                : localized("No candidate can start now: CPU/RAM/disk limits, search budget or queue state prevent admission.",
                            "Сейчас нельзя запустить кандидата: мешают лимиты CPU/RAM/диска, бюджет поиска или состояние очереди."));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", localized(
                "One extra admission request. RAM, Job Object and safety limits still apply.",
                "Разовый запрос дополнительного запуска. Ограничения RAM, Job Object и безопасности сохраняются."));
        const auto snapshot = m_engine.getSnapshot();
        if (snapshot.memoryOverrideEligible) {
            flowNext();
            const auto &offer = snapshot.memoryOverrideOffer;
            const auto label = std::string(localized("Run anyway (+", "Всё равно запустить (+")) +
                std::to_string(offer.extraMiB) + localized(" MiB over base)", " МиБ сверх базового)");
            if (ImGui::Button(label.c_str())) {
                const bool accepted = m_engine.requestMemoryOverride(offer.group, offer.configId, offer.extraMiB);
                std::snprintf(m_statusMessage, sizeof(m_statusMessage), "%s", accepted
                    ? localized("Temporary allowance admitted for the selected candidate; start details are in the log.",
                                "Временная надбавка разрешена выбранному кандидату; подробности запуска в журнале.")
                    : localized("Candidate not started: resources or queue changed, or fresh memory/Job checks rejected admission. Refresh the offer.",
                                "Кандидат не запущен: ресурсы или очередь изменились либо проверка памяти/Job отклонила допуск. Дождитесь обновления предложения."));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(localized("%s / %s\nRequired reservation: %llu MiB. Allowance above the original total RAM limit, including active reservations and persistent RAM input. Fresh physical-memory, commit and Job checks apply on click.",
                                            "%s / %s\nТребуемый резерв: %llu МиБ. Надбавка к исходному общему лимиту RAM с учётом активных резервов и постоянного RAM-входа. При нажатии проверяются память, commit и Job."),
                                  offer.group.empty() ? localized("Whole input", "Весь вход") : offer.group.c_str(),
                                  offer.name.c_str(), static_cast<unsigned long long>(offer.requiredRamMB));
        }
        flowNext();
    }

    // 4. Finish & Assemble Best Now
    if ((state == EngineState::TestingCandidates || isPaused) &&
        m_engine.getBestLeader().taskId > 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::LeaderBg);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::LeaderGreen);
        if (ImGui::Button(L::tr(StringId::BtnAssembleBest), scaledSize(240, 36))) {
            m_engine.finishAndAssembleBest();
        }
        ImGui::PopStyleColor(2);
    }

    if (m_shutdownScheduled) {
        flowNext();
        if (ImGui::Button(localized("Cancel shutdown", "Отменить выключение")) &&
            scheduleShutdown(true))
            m_shutdownScheduled = false;
    }
    if (!isRunning && !isPaused) {
        flowNext();
        if (ImGui::Button(L::tr(StringId::BtnWhatIf), scaledSize(120, 36))) {
            m_showWhatIf = true;
            m_previewReady = false;
        }
    }

    flowNext();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + scaled(8));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextWrapped("%s", m_statusMessage);
    ImGui::PopStyleColor();
}

void MainWindow::renderProgressBar() {
    auto snapshot = m_engine.getSnapshot();
    if (snapshot.groupsTotal && snapshot.state == EngineState::TestingCandidates)
        ImGui::Text("%s | %s", engineStateName(snapshot.state),
                    localized("Grouped search; candidate work varies by group", "Поиск по группам; объём работы кандидатов различается"));
    else
        ImGui::Text("%s | %s %.0f%%", engineStateName(snapshot.state),
                    L::currentLang() ? "Фаза" : "Phase", snapshot.phaseProgress * 100);
    if (snapshot.groupsTotal)
        ImGui::Text(L::currentLang() ? "Группы: %u/%u; активных поисков: %u" :
                                     "Groups: %u/%u; active searches: %u",
                    snapshot.groupsCompleted, snapshot.groupsTotal, snapshot.groupsActive);
    if (!snapshot.searchStage.empty())
        ImGui::Text(localized("Current search stage: %s", "Текущий этап поиска: %s"), snapshot.searchStage.c_str());
    ImGui::Text(localized("Top level: started %u / finished %u / deferred %u / total %d",
                          "Верхний уровень: запущено %u / завершено %u / отложено %u / всего %d"),
                snapshot.startedCandidates, snapshot.finishedCandidates, snapshot.deferredCandidates, snapshot.total);
    for (const auto &candidate : snapshot.activeCandidates)
        ImGui::TextWrapped(localized("%s: %s [%s]", "%s: %s [%s]"),
            candidate.group.empty() ? localized("Whole input", "Весь вход") : candidate.group.c_str(),
            candidate.name.c_str(), candidate.stage.c_str());
    if (!snapshot.localGroups.empty() && ImGui::TreeNode(localized(
            "Local group leaders (not a complete archive)", "Локальные лидеры групп (не полный архив)"))) {
        for (const auto &group : snapshot.localGroups) {
            ImGui::TextWrapped("%s [%s]", group.group.c_str(), group.stage.c_str());
            ImGui::Text(localized("Started %u / finished %u / deferred %u / total %u",
                                  "Запущено %u / завершено %u / отложено %u / всего %u"),
                        group.started, group.finished, group.deferred, group.total);
            if (group.leaderBytes) {
                ImGui::TextWrapped(localized("Local leader: %s — %.2f MiB [%s]",
                                            "Локальный лидер: %s — %.2f МиБ [%s]"),
                    group.leaderName.c_str(), static_cast<double>(group.leaderBytes) / 1048576.0,
                    group.contentVerified ? localized("Manifest/SHA verified", "Проверен manifest/SHA")
                                          : localized("Content verification pending", "Проверка содержимого ожидается"));
            } else
                ImGui::TextDisabled("%s", localized("No local leader yet", "Локального лидера пока нет"));
            ImGui::Separator();
        }
        ImGui::TreePop();
    }
    if (snapshot.groupsTotal) {
        if (snapshot.groupAssemblyPredictedSeconds > 0)
            ImGui::TextWrapped(localized("Group re-encoding estimate: ~%.0f s; assembly observed: %.1f s. Estimate excludes final verification.",
                                        "Оценка повторного сжатия групп: ~%.0f сек.; сборка измерена: %.1f сек. Оценка не включает финальную проверку."),
                              snapshot.groupAssemblyPredictedSeconds, snapshot.groupAssemblySeconds);
        else
            ImGui::TextWrapped(localized("Group assembly estimate unavailable; observed: %.1f s. Subsequent groups require re-encoding.",
                                        "Оценка сборки групп пока недоступна; измерено: %.1f сек. Последующие группы сжимаются повторно."),
                              snapshot.groupAssemblySeconds);
    }
    for (const auto &name : snapshot.activeTaskNames)
        ImGui::TextWrapped("%s", name.c_str());
    if (!snapshot.activeTaskNames.empty())
        ImGui::Text(L::currentLang() ? "Последнее завершённое задание: %.0f сек. назад" :
                                     "Last completed task: %.0f seconds ago",
                    snapshot.secondsSinceCandidateFinished);
    ImGui::Text(localized("%s: %llu / %llu | RAM reservation ~%llu MiB | CPU %u | active %u",
                          "%s: %llu / %llu | Резерв RAM ~%llu МиБ | CPU %u | активных %u"),
                L::currentLang() ? "Анализ файлов / байт" : "Scanned files / bytes",
                static_cast<unsigned long long>(snapshot.analyzedFiles),
                static_cast<unsigned long long>(snapshot.analyzedBytes),
                static_cast<unsigned long long>(snapshot.reservedRamMB), snapshot.reservedCpu,
                snapshot.activeTasks);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", localized(
            "RAM shows the scheduler reservation, including persistent input RAM. It is an estimate, not measured resident memory.",
            "RAM показывает резерв планировщика, включая постоянный входной RAM-кэш. Это оценка, а не измеренная физическая память."));
    ImGui::Text(localized("Child commit: %llu MiB | Effective limit: %llu MiB | Temporary allowance: %llu MiB",
                          "Commit дочерних процессов: %llu МиБ | Действующий лимит: %llu МиБ | Временная надбавка: %llu МиБ"),
                static_cast<unsigned long long>(snapshot.actualCommittedMB),
                static_cast<unsigned long long>(snapshot.effectiveRamLimitMB),
                static_cast<unsigned long long>(snapshot.memoryOverrideMB));
    if (!snapshot.blockedReason.empty())
        ImGui::TextWrapped(localized("Waiting: %s", "Ожидание: %s"), snapshot.blockedReason.c_str());
    if (snapshot.waitingCandidatesTotal &&
        ImGui::TreeNode("CandidateAdmissionQueue", localized("Waiting candidates: %llu (show up to 64)",
                                                           "Ожидающих кандидатов: %llu (показано до 64)"),
                        static_cast<unsigned long long>(snapshot.waitingCandidatesTotal))) {
        for (const auto &candidate : snapshot.waitingCandidates) {
            const auto label = candidate.group.empty() ? candidate.name : candidate.group + ": " + candidate.name;
            ImGui::TextWrapped("%s%s", candidate.deferred ? localized("[Deferred] ", "[Отложен] ") : "", label.c_str());
            ImGui::Text(localized("Required: %llu MiB RAM / %u CPU tokens", "Требуется: %llu МиБ RAM / %u CPU-токенов"),
                        static_cast<unsigned long long>(candidate.requiredRamMB), candidate.requiredCpu);
            ImGui::TextWrapped("%s", candidate.reason.c_str());
            if (!candidate.flags.empty()) ImGui::TextDisabled("%s", candidate.flags.c_str());
            ImGui::Separator();
        }
        ImGui::TreePop();
    }
    if (!snapshot.error.empty())
        ImGui::TextWrapped("%s", snapshot.error.c_str());
    if (!snapshot.stopReason.empty())
        ImGui::TextWrapped("%s", snapshot.stopReason.c_str());
    float progress = m_engine.getProgress();
    std::stringstream ss;
    const bool groupedSearch = snapshot.groupsTotal && snapshot.state == EngineState::TestingCandidates;
    if (groupedSearch) {
        progress = static_cast<float>(snapshot.groupsCompleted) / snapshot.groupsTotal;
        ss << localized("Groups finished: ", "Групп завершено: ") << snapshot.groupsCompleted << "/"
           << snapshot.groupsTotal << localized(" | ETA: unknown", " | ETA: неизвестно");
    } else
        ss << std::fixed << std::setprecision(1) << (progress * 100.0f)
           << localized("% Complete", "% завершено");

    EngineState state = m_engine.getState();
    if (state == EngineState::TestingCandidates && !groupedSearch) {
        double etaSeconds = m_engine.getEstimatedTimeRemaining();
        int comp = m_engine.getCompletedCount();
        int tot = m_engine.getTotalCount();
        if (etaSeconds > 0) {
            int hrs = etaSeconds / 3600;
            int mins = (int(etaSeconds) % 3600) / 60;
            ss << " | ETA: " << hrs << "h " << mins << "m (" << comp << "/" << tot << ")";
        } else {
            ss << " | ETA: " << L::tr(StringId::EtaCalculating);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Theme::Colors::Accent);
    ImGui::ProgressBar(progress, scaledSize(-1, 16), ss.str().c_str());
    ImGui::PopStyleColor();
}

void MainWindow::renderLogConsole() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::ChildBg);
    ImGui::BeginChild("LogConsoleChild", scaledSize(0, 0), true);

    ImGui::InputTextWithHint("##LogSearch", L::tr(StringId::LogSearch), m_logSearchBuffer,
                             sizeof(m_logSearchBuffer));
    ImGui::SameLine();
    if (ImGui::Button(L::tr(StringId::LogClear))) {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logs.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(L::tr(StringId::LogCopy))) {
        std::lock_guard<std::mutex> lock(m_logMutex);
        std::string allLogs;
        for (const auto &l : m_logs)
            allLogs += l + "\n";
        ImGui::SetClipboardText(allLogs.c_str());
    }

    ImGui::Separator();

    std::lock_guard<std::mutex> lock(m_logMutex);
    std::string searchStr = m_logSearchBuffer;
    std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });

    for (const auto &line : m_logs) {
        if (!searchStr.empty()) {
            std::string lowerLine = line;
            std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (lowerLine.find(searchStr) == std::string::npos)
                continue;
        }

        if (line.find("LEADER") != std::string::npos || line.find("SUCCESS") != std::string::npos) {
            ImGui::TextColored(Theme::Colors::LeaderGreen, "%s", line.c_str());
        } else if (line.find("Warning") != std::string::npos ||
                   line.find("hit memory") != std::string::npos) {
            ImGui::TextColored(Theme::Colors::WarningOrange, "%s", line.c_str());
        } else if (line.find("Error") != std::string::npos ||
                   line.find("failed") != std::string::npos) {
            ImGui::TextColored(Theme::Colors::ErrorRed, "%s", line.c_str());
        } else if (line.find("[7z]") != std::string::npos) {
            ImGui::TextColored(Theme::Colors::TextSecondary, "%s", line.c_str());
        } else {
            ImGui::Text("%s", line.c_str());
        }
    }

    if (m_autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace UltraArch
