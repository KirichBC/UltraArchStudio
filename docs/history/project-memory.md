# Актуализация — 2026-10-04

> Исторический документ. Актуальные файлы и сборка описаны в [главном README](../../README.md).

Поддерживаемый продукт: только Windows 10/11 x64, Win32 + DirectX 11. SDL/macOS/Linux GUI удалён из сборки; native CLI служит стендом тестирования ядра. Версия проекта 2.1.0. Текущее поведение и команды сборки описаны в README.md, изменения и проверка — в docs/history/2026-10-04-fixes.md.

Движок использует снимок RunContext, общий Windows Job Object с лимитом committed memory, резервирование памяти в очереди, отдельные токены отмены задач и тайм-ауты процессов. GUI опрашивает состояние только на основном потоке. Пауза останавливает очередь. Итог всегда проходит тест 7-Zip и контрольную распаковку с проверкой содержимого, затем публикуется с резервированием прежнего выхода. Томам нужна пересборка; solid и шифрование входят в параметры кандидатов. Препроцессинг Leanify/ECT использует копии и меняет архивируемые ресурсы; Precomp/SREP не поддерживаются. Пароли не сериализуются. Presets/checkpoints используют nlohmann/json и атомарную запись; resume проверяет вход/настройки/компрессор и сохранённого лидера.

Старые сборки в build/ и release/ не обновлялись: актуальный пакет находится в release-fixed/. Ниже сохранена история разработки; прежние заявления о встроенных утилитах, полной совместимости WinRAR, обязательном Store по энтропии, Precomp/SREP и GUI других платформ не описывают текущую версию.

---

# UltraArch Studio Pro - Память проекта и Контекст разработки

Данный документ содержит полный контекст разработки, историю принятых решений, архитектуру, статус реализованных фичей и хронику отладки. Он позволяет продолжить работу с проектом на любом компьютере и в любой новой сессии ассистента без потери контекста.

---

## 1. О проекте
**UltraArch Studio Pro** — профессиональный графический и консольный оптимизатор/репакер архивов на C++20 с графическим интерфейсом на базе **Dear ImGui** и **DirectX 11** (под Windows) / **SDL3 + OpenGL** (под macOS/Linux).

Цель: автоматический перебор сотен матриц флагов 7-Zip, выбор наиболее эффективных параметров сжатия по размеру, времени и потреблению ОЗУ, глубокий анализ контента и сборка финального архива без мусора на накопителе.

---

## 2. Архитектура и Структура файлов

```
7zOptimizer/
├── src/
│   ├── main.cpp                  # Точка входа WinMain / main, инициализация DirectX 11, ImGui, CrashHandler
│   ├── core/
│   │   ├── Types.hpp             # Базовые структуры (CandidateResult, Settings, Telemetry, etc.)
│   │   ├── ContentAnalyzer.hpp/.cpp      # Анализ энтропии, категорий данных (бинарники, медиа, текст)
│   │   ├── TaskMatrixGenerator.hpp/.cpp  # Генерация матриц параметров 7z (Safe, Deep, Extreme)
│   │   ├── ResourceGuard.hpp/.cpp        # WinAPI Job Objects, динамический RAM Guard, лимиты памяти
│   │   ├── ProcessManager.hpp/.cpp       # Запуск консольных процессов с перехватом I/O
│   │   ├── OptimizerEngine.hpp/.cpp      # Главный координатор задач, очередь, пул потоков
│   │   ├── EmbeddedTools.hpp/.cpp        # Поиск утилит (7za, 7z, ect, leanify) рядом с exe
│   │   ├── PreprocessorPipeline.hpp/.cpp # Пайплайн препроцессоров (Leanify, ECT)
│   │   ├── SessionManager.hpp/.cpp       # Сохранение и возобновление сессий в JSON
│   │   ├── ArchiveExtractor.hpp/.cpp     # Распаковка входных архивов для режима RePack
│   │   └── PresetManager.hpp/.cpp        # Менеджер пресетов (Game, ISO, Source, Photo, custom)
│   └── ui/
│       ├── MainWindow.hpp/.cpp           # Главное окно UI, обработка событий, таймеры
│       ├── LeaderboardWidget.hpp/.cpp    # Таблица лидеров, сортировка, метрики, экспорт CSV/JSON
│       ├── TelemetryWidget.hpp/.cpp      # Графики CPU, RAM, Disk I/O в реальном времени
│       ├── Theme.hpp                     # Dark Fluent UI палитра, Segoe UI шрифты
│       └── Localization.hpp              # Двуязычная локализация (RU / EN)
├── vendor/
│   └── imgui/                            # Dear ImGui + Win32/DX11/SDL3 бэкенды
├── resources/
│   ├── app.ico / icon.ico                # Иконки приложения
│   ├── app.manifest                      # Манифест приложения Windows
│   └── resource.rc                       # Скрипт ресурсов (иконка ID 101, манифест)
├── release/
│   ├── UltraArchStudio_Win64/            # Готовая переносимая сборка Windows x64
│   │   ├── UltraArchStudio.exe
│   │   ├── 7za.exe, 7z.exe, 7z.dll, ect.exe, leanify.exe
│   │   └── presets/
│   ├── UltraArchStudio_Win64.zip         # ZIP дистрибутив
│   └── UltraArchStudio_Win64.7z          # 7z дистрибутив
└── CMakeLists.txt                        # Конфигурация сборки CMake
```

---

## 3. Все согласованные и внедренные модули ("ВСЕ ПУНКТЫ")

1. **Zero-Waste SSD Cleaner**: немедленное удаление временных архивов, показавших худший результат, чем текущий лидер. Предотвращает переполнение SSD.
2. **WinRAR Compatibility Matrix**: профили `WinRAR_Safe` (гарантированное открытие в WinRAR), `DeepSearch`, `Extreme_RePack`.
3. **Smart Resource Guard**: защита от вылетов по нехватке памяти (OOM), WinAPI Job Objects, жесткий контроль RAM.
4. **Deep Content Inspection**: детекция типов файлов по сигнатурам и энтропии шеннона, группировка схожих файлов для solid-блоков.
5. **Preprocessor Pipeline**: предварительная оптимизация PNG, JPEG, ZIP, GZ через ECT и Leanify перед сжатием 7z.
6. **Session Resume & State Persistence**: автосохранение прогресса в `sessions/*.json`, возможность возобновления прерванного тестирования.
7. **Repack Engine**: поддержка переупаковки существующих архивов (.zip/.rar/.7z) с авто-распаковкой во временную директорию.
8. **Modern Dark Fluent UI**: стиль Windows 11 Fluent Dark, шрифт Segoe UI с поддержкой кириллицы, адаптивный DPI, системный трей, горячие клавиши (Space — пауза, Esc — отмена).
9. **Telemetry & Analytics Dashboard**: графики нагрузки CPU %, использования ОЗУ (включая лимит) и скорости ввода/вывода диска в реальном времени.
10. **Headless CLI Mode**: полноценная работа из консоли (`UltraArchStudio.exe --cli --input <path> [options]`). Консольное окно автоматически скрывается в GUI-режиме и отображается только при тестировании/сборке.
11. **Presets System**: встроенные пресеты (Game RePack, ISO Archive, Source Code, Photo Archive) + сохранение/загрузка пользовательских пресетов в JSON.
12. **Security & Packaging**: шифрование AES-256, скрытие заголовков `-mhe`, разбиение на тома `-v`, генерация SFX-архивов.
13. **Smart ETA & Auto-Shutdown**: точный расчет оставшегося времени и опция автоматического выключения ПК по завершении оптимизации.
14. **Drag & Drop**: перетаскивание любых файлов и папок прямо в окно приложения.
15. **Custom Temp Workspace**: выбор произвольной папки на любом диске для временных файлов, чтобы избежать переполнения диска C:.

---

## 4. Хроника решенных проблем и багов на Windows 10

1. **Вылеты из-за `<activeCodePage>UTF-8</activeCodePage>` в манифесте**:
   - На некоторых редакциях Windows 10 экспериментальный тег UTF-8 в `app.manifest` приводил к Side-by-Side (SxS) ошибкам. Тег был удален, кодировка переведена на стандартный Win32 WideChar API.
2. **Ложные срабатывания Windows Defender**:
   - Внедрение исполняемых файлов внутрь .rc ресурсов триггерило эвристику антивируса. Все внешние утилиты (`7za.exe`, `7z.dll`, `ect.exe`, `leanify.exe`) вынесены рядом с `.exe` в переносимый пакет. Размер бинарника уменьшился с 6 МБ до 2.8 МБ.
3. **Скрытие консольного окна в GUI-режиме**:
   - Приложение собрано с флагом `-mwindows`. Окно консоли выделяется динамически через `AllocConsole()` только при старте тестов/сборки и скрывается при переходе в режим ожидания.
4. **Сбой 0xC0000005 при первом кадре в `LeaderboardWidget`**:
   - **Причина 1**: В `LeaderboardWidget.cpp:160` при `sorts_specs->SpecsDirty == true` происходило обращение к `sorts_specs->Specs[0]`, но на первом кадре `SpecsCount == 0` и `Specs == nullptr`. Добавлена проверка `if (sorts_specs->SpecsCount > 0 && sorts_specs->Specs != nullptr)`.
   - **Причина 2 (главная)**: В блоке карточек метрик вызывался `ImGui::BeginTable("MetricsTable", 4)` без проверки возвращаемого значения. Когда Dear ImGui на первом кадре калибрует окно, `BeginTable` возвращает `false`, и `TableNextRow()` вызывал `g.CurrentTable->IsLayoutLocked` по нулевому указателю. Добавлена строгая проверка `if (ImGui::BeginTable(...)) { ... ImGui::EndTable(); }`.
5. **Безопасная обработка аргументов командной строки**:
   - В `main.cpp` устаревшие `__argc`/`__argv` заменены на Win32 API `CommandLineToArgvW(GetCommandLineW(), &numArgs)` с корректным перекодированием в UTF-8.
6. **Поддержка любых видеокарт и драйверов**:
   - В `CreateDeviceD3D` добавлен список `D3D_FEATURE_LEVEL` от 11.1 до 9.3 с автоматическим fallback на программный WARP-рендерер в случае проблем с драйверами.
7. **Шрифты и русский язык**:
   - Подключен шрифт `C:\Windows\Fonts\segoeui.ttf` с глифами `0x0400-0x052F`.

---

## 5. Как собрать проект на Windows

### Вариант 1: MinGW-w64 (GCC / Clang)
```bash
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --target UltraArchStudio -j8
```

### Вариант 2: Visual Studio / MSVC
```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release --target UltraArchStudio
```
