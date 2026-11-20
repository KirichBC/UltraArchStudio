# Проверки на Windows — обновлено 2026-10-23

Последний проверенный Release: `release/msvc-2026-10-12/photoshop-postrun-final/`; фактические проверки и ограничения: [Photoshop postrun implementation](photoshop-postrun-implementation-2026-10-11.md). При [очистке 23 октября](project-cleanup.md) удалены старые пакеты и пересоздаваемые build trees; исторические трассы и тестовые данные упакованы без потери содержимого. Сборочные команды ниже создают `build/` заново. Раздел roadmap и его точная команда сохраняют историю измерений от 9 октября; указанные там старые пакеты больше не лежат в рабочем дереве.

Проект UltraArch Studio 2.2.0-dev. Поддерживаемая ОС: Windows 10/11 x64.
MSVC Debug/Release и fault build проверены нативно на Windows 10 x64. Измерения, исправления и оставшаяся приёмка: [отчёт от 6 октября](windows-acceptance-2026-10-06.md). Windows 11 и ручные GUI/DPI проверки пока не подтверждены.
Исходная исправленная версия 2.1 сохранена в Git: тег `baseline-2.1`.
Инструкция установки и открытия проекта в Codex: [windows-setup.md](windows-setup.md).

## Автоматические проверки

PowerShell 7, папка исходников `C:\Projects\7zOptimizer`; VS 2022 C++ x64 / v143 / Windows SDK, CMake >=3.24, Python 3, Git.

```powershell
pwsh -File ./tools/windows-build.ps1 -Configuration Debug
pwsh -File ./tools/windows-build.ps1 -Configuration Release
pwsh -File ./tools/windows-build.ps1 -Configuration Debug -FaultTests
```

Первые две команды собирают, запускают unit/integration/contracts suites, создают отдельные portable/developer ZIP и проверяют установленный developer-пакет из другого CWD в Unicode-пути. Пакеты появляются в новом каталоге `release-windows-*`. Запуск fault tests разрешён только в отдельной developer-сборке: `ULTRAARCH_FAULT_INJECTION` выключен в обычном продукте.

Обязательно проверить: правильный/неправильный пароль, header encryption, тома, запуск SFX; Dual verifier, повреждённый архив и ошибочный SHA; Unicode и пути >260 символов; read-only output (сохранность прежнего файла), read-only source и cleanup overlay; ADS/reparse rejection; несколько jobs с общим OOM; pause/skip/cancel/finish/resume; crash/recovery на шести границах транзакции. Автоматизация уже находится в `tests/IntegrationTests.cpp`, `tests/VerificationTests.cpp`, `tests/RecoveryTests.cpp`.

`tools/windows-smoke.ps1 -PackageDirectory <developer> -KeepEvidence` сохраняет manifest SHA-256, результаты Authenticode и CLI-report. Неподписанный файл и неверная подпись — разные результаты; не объявлять подписи проверенными только по хешу.

## Историческая проверка roadmap — 9 октября 2026

Этапы словарей, отбора PE/малых групп, совместного PPMd, capability cache, модели стоимости/очереди, экспериментальной similarity-группировки и интерфейса реализованы. MSVC Debug/Release собраны. Полный native Debug: 6/6 за 90,68 с; Recovery: 1/1; verify-worker с backend 26.04: 18/18; installed smoke `roadmap-final-release`: все 6 suites прошли. Evidence: `validation/roadmap-2026-10-09-debug-final.xml`, `validation/roadmap-2026-10-09-recovery.xml`, `validation/roadmap-2026-10-09/worker-smoke-2604/`, `validation/roadmap-2026-10-09/installed-final-release-smoke.log`.

Один полный контроль нового Balanced завершён: 367 238 968 байт за 1298,130884 с, пик committed memory дерева 13 883 707 392 байта (12,93 ГиБ), средняя CPU 38,77%, 18 229 процессов. Независимая проверка 10 144 файлов/890 папок по SHA прошла за дополнительные 14,49743 с. Доказательства: `F:/UltraArch-validation-2026-10-09/grouped-balanced-roadmap-01/measurement.json`, `application-report.json`, `independent-verify.log`.

Прежний Balanced: 367 248 253 байта, 1436,507718 с, 9,32 ГиБ, 48 649 процессов. Новый результат: −9285 байт, −138,376834 с (−9,63%), −62,53% процессов, но +38,7% пикового RAM. BIN/текстовый PPMd улучшились на 8093/1580 байт; до исторического Full 367 238 589 байт осталось 379 байт — многочасовой повтор ради них не требуется. Ultra7z: 367 383 626 байт/2850 с; n=1, backend 24.08 против 26.04 и разные механизмы учёта времени/памяти не допускают универсального вывода.

Измеренный `release/msvc-2026-10-09/roadmap-final-release` сохраняется; SHA-256 EXE `3b86082fc3a95bdbe7f1c9c1645adf834c047876272dfa7f1fa903588fd5589f`. Последняя корректировка описания декодерного словаря LZMA1 не меняет команды компрессии и резервы в МиБ: ниже 2 МиБ — ступени 2 или 3, умноженные на степень двойки с минимумом 4 КиБ; от 2 МиБ — ceil до МиБ. Проверены 441 строка preview и весь допустимый диапазон КиБ в unit-тесте. Новые пакеты `roadmap-verified-{debug,release}` собраны; installed smoke последнего Release прошёл (шесть suites и CLI), см. `validation/roadmap-2026-10-09/installed-verified-release-smoke.log`.

На 9 октября очистка не выполнялась; `tools/clean-generated.ps1` сохраняет прежний ограниченный dry-run список. 23 октября по запросу пользователя старые пакеты и build trees удалены, эталоны и доказательства сохранены; актуальный перечень находится в [журнале очистки](project-cleanup.md).

## Производительность

Подставить путь к Release EXE и его `bin`. Полный стандартный корпус: source, PE x86/x64 из Windows, media, уже сжатые данные, repetition, mixed, 500 мелких файлов, большой solid. Каждый запуск сохраняет JSON/CSV, корпус с SHA-256, ОС/CPU/RAM/disk/tool identities. Минимум три повтора.

```powershell
python ./tools/benchmark.py --exe ./build/windows/Release/UltraArchStudio.exe --tools ./bin --output ./benchmark-results/windows-release --ram 1024 4096 --workers 1 2 --mode balanced exhaustive --large-mib 16
python ./tools/benchmark.py --exe ./build/windows/Release/UltraArchStudio.exe --tools ./bin --output ./benchmark-results/windows-dual --ram 1024 --workers 1 --mode balanced exhaustive --dataset solid mixed --verifier dual
```

Проверить `comparison.json`: цель Balanced — минимум 30% экономии времени и максимум 1% увеличения размера против того же Exhaustive. Это критерий приёмки, а не гарантия. Малые входы <=2 MiB обходят probe: Balanced/Fast используют сокращённый отбор по семействам, Full/Exhaustive — всю допустимую матрицу. Полный контроль новой версии завершён; отдельный вклад сокращения малых групп в ускорение не изолирован. Сравнить Disk/Dual/Streaming; потоковый verifier пока экспериментальный и не является режимом по умолчанию. Счётчики I/O кандидатов относятся к главному процессу компрессии, scratch peak — наблюдаемый логический объём, не сумма реально выделенных блоков на диске. Корпус после первого запуска можно переиспользовать через `--corpus` с проверкой его manifest.

IPO выключен. Включать `-IPO` только после сравнения размера EXE и времени на том же корпусе. Автоматически очищать системный файловый cache runner не пытается.

Для новой реализации roadmap основной порядок — короткие targeted сравнения затронутых групп и один полный Test.7z после значимого набора изменений. Остановленная серия из 62 запусков не запускается и не возобновляется автоматически. Ранее сохранённые результаты относятся к прежним бинарникам и backend; повторный большой корпус требует отдельного решения с понятным бюджетом времени.

Backend 26.04 и installed smoke измеренного `roadmap-final-release` проверены. Полный контроль ниже уже завершён; это запись команды для воспроизводимости, а не указание автоматически повторять его. Последний `roadmap-verified-release` после корректировки отображения словаря также прошёл installed smoke (шесть suites и CLI):

```powershell
python ./tools/compare-archivers-windows.py --exe ./release/msvc-2026-10-09/roadmap-final-release/portable/UltraArchStudio.exe --tools ./release/msvc-2026-10-09/roadmap-final-release/portable/bin --input ./validation/ultra7z-comparison-2026-10-08-no-ttf/input-original.7z --reference ./validation/ultra7z-comparison-2026-10-08-no-ttf/reference-manifest.json --output F:/UltraArch-validation-2026-10-09/roadmap-balanced-01 --ram 26624 --workers 4 --cpu-threads 8 --search balanced --strategy groups
```

Расширенный `compression-benchmark-windows.py` остаётся инструментом отдельной приёмки, а не обязательным продолжением этого контроля. Он поддерживает сравнение общего/группового поиска, наборов корпуса и фонового режима. Каждый завершённый результат должен независимо распаковываться и сверяться по путям, размерам, SHA-256 файлов и папкам.

Runner сохраняет `active-case.json`, `runs.json`, `summary.json`, `comparison.json`, отчёты приложения, ежесекундные `job-samples.jsonl` и учёт дерева процессов Windows Job Object. Счётчики Job Object сохраняют завершившиеся короткие процессы; интервал наблюдения не определяет их время CPU или пик committed memory. Время запуска заканчивается при выходе процесса и не округляется до интервала наблюдения. При возобновлении runner принимает только полностью проверенные результаты с теми же SHA приложения, backend, DLL и входа, а также бюджетами RAM/CPU, числом задач, стратегией и режимом поиска. Системный cache сохраняется. Для сравнения скорости не запускайте сборку, другие тесты или второй архиватор одновременно.

Этот runner не запускает GUI Ultra7z. Его отдельные повторы на том же корпусе, проверка штатного backend и внешнего распаковщика остаются самостоятельной частью сравнительной приёмки. Сохранённый единственный прогон Ultra7z не подтверждает универсальное превосходство UltraArch.

## Ручная приёмка GUI и системы

- RU/EN: все новые параметры, импорт пресета (точный RAM/solid/CPU/timeout), What-if для архива без полной распаковки, отмена preview, история/экспорт/удаление сессии, JSON диагностика, команды выбранного кандидата.
- DPI 100/150/200%, перенос между двумя мониторами; узкое окно, длинные Unicode-пути, drag-and-drop, tray/minimize/close. Проверить отсутствие обрезанных controls/нечитаемых сообщений.
- Nested Job Objects и общий OOM прошли автоматические тесты на Windows 10. Дополнительно проверить сторонние ограничивающие jobs/CI. CPU Sets/Physical affinity — отдельно, в том числе >64 логических CPU, если ПК доступен.
- Жёсткое завершение/перезапуск во время checkpoint, assembly и multi-volume publication. Recovery должен восстановить прежние выходы, сохранить неизвестные файлы и отказать, если другой процесс изменил выход.
- Проверить disk-full на ограниченном тестовом томе. Quota watchdog проверяет распаковку раз в 100 ms: допустим временный выход за лимит между опросами; это не жёсткая файловая квота NTFS.
- Потоковый worker: native suites и `tools/windows-worker-smoke.py` подтвердили pinned DLL/SHA/password/SFX/volumes, cancel/timeout/OOM и 62 bytes IPC на 19.65 MB восстановленного содержимого без изменения файлового дерева. Metadata manifest и scratch описаны в отчёте; режим остаётся экспериментальным.
- WinRAR: вручную распаковать Safe/Deep/Extreme результаты доступными целевыми версиями и сверить SHA/пустые каталоги. До этого не обещать универсальную совместимость.

Исправлять обнаруженные дефекты и обновлять docs/implementation-status.md фактическими версиями Windows/MSVC, результатами и путями к evidence. Windows CI настроен, но без GitHub remote/workflow run здесь не запускался. Лицензии и manifest находятся в `licenses`, `bin/tool-manifest.json`, `vendor/7zip/PROVENANCE.json`.
