#pragma once
#include <string>
#include <string_view>

namespace UltraArch {

enum class StringId {
    // Window
    AppTitle, StatusIdle, StatusAnalyzing, StatusTesting, StatusAssembling,
    StatusPaused, StatusCompleted, StatusCancelled, StatusPreprocessing, StatusExtracting,
    
    // Input section
    InputPath, OutputPath, TempWorkspace, FreeSpace, AutoDetect, Browse,
    DropFileHere,
    
    // Settings
    Profile, ProfileWinRAR, ProfileDeep, ProfileExtreme,
    RamLimit, Workers, VerifyArchives, ZeroWaste, PortableSfx,
    EnablePreprocessors, EnableAdaptivePruning, PruningThreshold,
    Encryption, Password, EncryptHeaders,
    VolumeSize, SolidBlock,
    
    // Controls  
    BtnStart, BtnCancel, BtnPause, BtnResume, BtnSkip, BtnAssembleBest,
    BtnWhatIf, BtnAutoShutdown,
    
    // Leaderboard
    LeaderboardTitle, ColId, ColPreset, ColFlags, ColSize, ColRatio, ColTime, ColRam, ColStatus,
    SortBy, FilterPlaceholder, ExportCsv, ExportJson,
    CopyFlags, Retest, SelectWinner,
    
    // Telemetry
    CpuLoad, RamUsage, DiskIO,
    
    // Log
    LogTitle, LogClear, LogCopy, LogSearch,
    
    // Dialogs
    ConfirmCancel, ConfirmCancelMsg, Yes, No,
    DiskSpaceWarning, DiskSpaceError,
    
    // ETA
    EtaRemaining, EtaCalculating,
    
    // Presets
    PresetSave, PresetLoad, PresetDelete, PresetName, PresetDesc,
    
    // Misc
    Language, Hotkeys, PortableMode,
    
    _COUNT
};

class L {
public:
    static void setLanguage(const std::string& lang) { s_lang = (lang == "ru") ? 1 : 0; }
    static const char* tr(StringId id) { return s_strings[static_cast<int>(id)][s_lang]; }
    static int currentLang() { return s_lang; }
private:
    static inline int s_lang = 0; // 0=EN, 1=RU
    static constexpr int COUNT = static_cast<int>(StringId::_COUNT);
    // [stringId][lang]
    static inline const char* s_strings[COUNT][2] = {
        {"UltraArch Studio", "UltraArch Studio"},
        {"Ready", "Готово"},
        {"Analyzing...", "Анализ..."},
        {"Testing candidates...", "Тестирование кандидатов..."},
        {"Assembling final archive...", "Сборка финального архива..."},
        {"Paused", "Пауза"},
        {"Completed", "Завершено"},
        {"Cancelled", "Отменено"},
        {"Preprocessing...", "Препроцессинг..."},
        {"Extracting...", "Извлечение..."},
        
        {"Input File or Folder", "Входной файл или папка"},
        {"Output Path", "Путь вывода"},
        {"Temp Workspace", "Временная папка"},
        {"Free Space", "Свободное место"},
        {"Auto-Detect", "Авто-определение"},
        {"Browse...", "Обзор..."},
        {"Drop file or folder here", "Перетащите файл или папку сюда"},
        
        {"Profile", "Профиль"},
        {"WinRAR Safe", "WinRAR Безопасный"},
        {"Deep Search", "Глубокий Поиск"},
        {"Extreme", "Экстрим"},
        {"RAM Limit", "Лимит ОЗУ"},
        {"Workers", "Потоки"},
        {"Verify Archives", "Проверять Архивы"},
        {"Zero Waste", "Zero Waste очистка"},
        {"Portable SFX", "Портативный SFX"},
        {"Enable Preprocessors", "Включить препроцессоры"},
        {"Adaptive Pruning", "Адаптивная обрезка"},
        {"Pruning Threshold", "Порог обрезки"},
        {"Encryption", "Шифрование"},
        {"Password", "Пароль"},
        {"Encrypt Headers", "Шифровать заголовки (-mhe)"},
        {"Volume Size (MB)", "Размер тома (МБ)"},
        {"Solid Block", "Непрерывный блок"},
        
        {"START OPTIMIZATION", "НАЧАТЬ ОПТИМИЗАЦИЮ"},
        {"CANCEL", "ОТМЕНА"},
        {"PAUSE", "ПАУЗА"},
        {"RESUME", "ПРОДОЛЖИТЬ"},
        {"SKIP CURRENT", "ПРОПУСТИТЬ"},
        {"ASSEMBLE BEST", "СОБРАТЬ ЛУЧШИЙ"},
        {"WHAT IF?", "ЧТО ЕСЛИ?"},
        {"Auto Shutdown", "Автовыключение"},
        
        {"Leaderboard", "Таблица лидеров"},
        {"#", "#"},
        {"Preset", "Пресет"},
        {"7z Flags Matrix", "Флаги 7z"},
        {"Output Size", "Размер вывода"},
        {"Ratio", "Сжатие"},
        {"Time", "Время"},
        {"RAM", "ОЗУ"},
        {"Status", "Статус"},
        {"Sort by:", "Сортировка:"},
        {"Type to filter...", "Фильтр..."},
        {"Export CSV", "Экспорт CSV"},
        {"Export JSON", "Экспорт JSON"},
        {"Copy Flags", "Копировать Флаги"},
        {"Retest", "Перетестировать"},
        {"Select Winner", "Выбрать Победителем"},
        
        {"CPU Load", "Загрузка ЦП"},
        {"RAM Usage", "Использование ОЗУ"},
        {"Disk I/O", "Диск (Ввод/Вывод)"},
        
        {"Log", "Журнал"},
        {"Clear", "Очистить"},
        {"Copy", "Копировать"},
        {"Search", "Поиск"},
        
        {"Confirm Cancel", "Подтверждение отмены"},
        {"Are you sure you want to cancel?", "Вы уверены, что хотите отменить?"},
        {"Yes", "Да"},
        {"No", "Нет"},
        {"Disk Space Warning", "Предупреждение о месте на диске"},
        {"Not enough disk space!", "Недостаточно места на диске!"},
        
        {"Time Remaining", "Осталось времени"},
        {"Calculating ETA...", "Расчет времени..."},
        
        {"Save Preset", "Сохранить Пресет"},
        {"Load Preset", "Загрузить Пресет"},
        {"Delete Preset", "Удалить Пресет"},
        {"Preset Name", "Имя Пресета"},
        {"Preset Description", "Описание Пресета"},
        
        {"Language", "Язык"},
        {"Hotkeys", "Горячие клавиши"},
        {"Portable Mode", "Портативный режим"}
    };
};

} // namespace UltraArch
