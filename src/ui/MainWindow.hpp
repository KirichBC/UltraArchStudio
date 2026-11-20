#pragma once

#include "../core/OptimizerEngine.hpp"
#include "../core/PresetManager.hpp"
#include "LeaderboardWidget.hpp"
#include "Localization.hpp"
#include "TelemetryWidget.hpp"
#include <deque>
#include <future>
#include <string>
#include <vector>

namespace UltraArch {

class MainWindow {
  public:
    explicit MainWindow(OptimizerEngine &engine);
    ~MainWindow();

    // Renders the entire Dark Fluent UI window
    void render();
    void update();

    // Sets target path (e.g. from command line or drag and drop)
    void setInputPath(const std::string &path);

    void handleDragDrop(const char *path);

    // Shows/hides the external console window for live test and packaging output
    static void showConsoleWindow(bool show);

  private:
    OptimizerSettings collectSettings() const;
    void startOptimization();
    static bool scheduleShutdown(bool abort);
    void applyState(EngineState state);
    void renderTopBar();
    void renderInputConfig();
    void renderSettingsBar();
    void renderRamSettings();
    void renderEncryptionSection();
    void renderPresetSelector();
    void renderControlButtons();
    void renderProgressBar();
    void renderLogConsole();
    void renderCancelConfirmDialog();
    void renderWhatIfDialog();
    void renderHotkeys();
    void renderSessions();

    static std::string openFolderDialog();
    static std::string openFileDialog();
    static std::string saveFileDialog(bool sfx);

    OptimizerEngine &m_engine;
    LeaderboardWidget m_leaderboardWidget;
    TelemetryWidget m_telemetryWidget;
    PresetManager m_presetManager;

    std::string m_inputPathBuffer, m_outputPathBuffer, m_tempPathBuffer;

    char m_passwordBuffer[256] = {0};
    char m_presetNameBuffer[128] = {0};
    char m_presetDescBuffer[256] = {0};
    char m_logSearchBuffer[128] = {0};

    int m_selectedProfileIndex = 1; // 0: WinRAR Safe, 1: Deep Search, 2: Extreme RePack
    int64_t m_ramLimitMB = 8192;
    std::string m_customSolid;
    int m_workerThreads = 2;
    int m_volumeSizeMB = 0;
    int m_solidBlockIndex = 0; // 0=auto, 1=on, 2=off, 3=e, 4=512m, 5=128m

    bool m_verifyArchives = true;
    bool m_zeroWasteCleaner = true;
    bool m_createSfx = false;
    bool m_encryptHeaders = false;
    bool m_enablePreprocessors = false;
    bool m_showCancelConfirm = false;
    bool m_showWhatIf = false;
    bool m_settingsLocked = false;
    bool m_autoShutdown = false;
    bool m_shutdownScheduled = false;
    OptimizerSettings m_additionalSettings;

    EngineState m_lastState = EngineState::Idle;
    std::vector<TaskConfig> m_previewTasks;
    bool m_previewReady = false;
    struct PreviewResult {
        std::vector<TaskConfig> tasks;
        std::string message;
    };
    std::future<PreviewResult> m_preview;
    std::future<bool> m_sessionExport;
    std::atomic<bool> m_exportCancel{false};
    std::atomic<bool> m_previewCancel{false};

    std::deque<std::string> m_logs;
    std::mutex m_logMutex;
    bool m_autoScrollLog = true;
    char m_statusMessage[128] = "Ready.";
};

} // namespace UltraArch
