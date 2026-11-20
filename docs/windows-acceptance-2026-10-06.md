# Windows-приёмка UltraArch Studio — 2026-10-06

UltraArch Studio 2.2.0-dev собран и проверен нативно в PowerShell на **Windows 10 x64 10.0.19045**. Windows 11 на этом компьютере не установлен; его runtime отдельно не проверен. Git baseline `baseline-2.1` сохранён, новый commit не создавался.

## Среда и доказательства

- Visual Studio Build Tools 2022 17.14.41, MSVC 19.44.35229, v143 14.44.35207; SDK 10.0.26100.0. IPO выключен.
- AMD Ryzen 5 7600, 6 физических ядер / 12 потоков, RAM 33 934 004 224 bytes. ОС сохраняет файловый cache; принудительного сброса между benchmark-запусками нет.
- MSVC Debug: 3/3 CTest suites, Release: 3/3. Отдельный Debug с `ULTRAARCH_FAULT_INJECTION=ON`: 4/4, включая recovery. Последний запуск трёх конфигураций выполнялся параллельно, поэтому длительность CTest не является benchmark.
- JUnit: `validation/windows-2026-10-06/debug-tests.xml`, `release-tests.xml`, `fault-tests.xml`. Логи MSVC: `build/windows/debug-build.log`, `release-build.log`, `build/windows-faults/fault-build.log`.
- Машинная сводка, среда, installed smoke и измерения: `validation/windows-2026-10-06/`. Полные benchmark JSON/CSV, команды, отчёты каждого запуска и SHA корпуса находятся в `benchmark-results/`.

## Найденные и исправленные ошибки

1. MSVC resource/linker создавали два manifest с ID 1, что давало CVT1100/LNK1123. Для EXE с собственным RC manifest выключен автоматический linker manifest. Версия manifest обновлена до 2.2.
2. Windows-проверка пути пропускала `/absolute`, поскольку `is_absolute()` требует диск. Добавлена проверка root directory.
3. Собственный parser 7-Zip обрабатывает кавычки иначе, чем CRT: пароль с кавычкой и backslash разрывал команду и создавал посторонний `with.7z`. Пароль дочернего 7-Zip теперь передаётся через приватный UTF-8 stdin, включён `-sccUTF-8`. Тест включает русский/японский пароль, кавычку и завершающий backslash. NUL/CR/LF явно отклоняются. У приложения `--password` остаётся собственным CLI-аргументом; GUI этого не требует.
4. Standalone assembly не могла записать manifest worker в ещё не созданный custom temp. Каталог создаётся с диагностикой ошибки. Worker обнаруживается также рядом с MSVC multi-config EXE.
5. CreateProcessW возвращал error 267 для локального CWD длиннее 260 символов. Используется принадлежащий запуску короткий NTFS junction, удаляемый без рекурсивного удаления входа. Протестирован локальный NTFS; длинный UNC CWD явно не поддержан этим обходом.
6. Последовательные sample probes съедали выигрыш Balanced при двух workers. Они выполняются ограниченным пулом с RAM/CPU reservations, отменой и общим deadline. Cache по одинаковой probe spec сохранён. Дробный размер sample округляется до KiB для словаря; регрессия проверена на входе 101 MiB + 17 bytes. Finalists проходят прежнюю проверку на полном входе.
7. Runtime package исключает старые необязательные `bin/Codecs` DLL, отсутствующие в pinned manifest и поддерживаемой стандартной матрице. Исходные DLL сохранены в рабочем дереве.

## Подтверждённые автоматические сценарии

Integration выполняет реальные инструменты; обязательные 7-Zip/SFX не пропускаются. Проверены full-content SHA и empty directories, Unicode и пути >260, правильный/неправильный пароль и encrypted headers, тома, реальное исполнение SFX, сохранность прежнего read-only output, ADS rejection, mutation входа, безопасный overlay, quotas/watchdog, preview, pause/finish/cancel/resume, исключения UI callback, общий RAM/CPU scheduler.

На Windows прошли suspended launch под Job Object, nested jobs, ограничение памяти и одновременный OOM двух процессов. Generic ProcessManager проверен на timeout/cancel и точную передачу CRT-аргументов. Дополнительный `workerProcessControls` удерживает чтение DLL identity через локальный named pipe: timeout 1 s и отмена через 500 ms завершают реальный worker через ProcessManager/Job. Dual проверяет также корректные encrypted volumes и SFX, corrupt archive и неверный SHA. Эти проверки не означают приёмку всех конфигураций ограничивающих jobs сторонних CI.

`tools/windows-worker-smoke.py` прошёл **18/18** сценариев на Release worker: encrypted 7z/volumes/SFX × success/decoder OOM/wrong password/manifest identity/content SHA/DLL identity. Каждый success восстановил **19 647 152 bytes** в SHA sink. `GetProcessIoCounters.WriteTransferCount` составил **62 bytes**, ровно размер ответа IPC; дерево файлов и SHA не изменились. На этом входе записи восстановленных байтов на диск нет. Parent заранее записывает metadata manifest отдельно; это измерение worker, а не всей программы. Decoder OOM при лимите 16 MiB подтверждён `JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT` (9), exit 1 и отрицательным ответом. Его текст пока общий: `Incomplete archive extraction`.

Recovery завершает дочерний процесс с exit 86 на Prepared, Verified, BackingUp, Publishing, AfterFile, Committed. Проверены восстановление прежнего набора томов, сохранение committed результата, постороннего файла и отказ при чужой модификации. Это управляемые process-crash tests, а не физическое отключение питания.

## Benchmark и поставка

Итоговый Disk benchmark завершён: **192/192** успешных запуска на финальной Release. Balanced дал **0%** увеличения размера во всех 32 сравнениях. На двух входах >2 MiB достигнута цель ≥30% во всех восьми сочетаниях RAM/workers:

| Вход | RAM MiB | Workers | Balanced, s | Экономия времени | Размер против Exhaustive |
|---|---:|---:|---:|---:|---:|
| mixed | 1024 | 1 | 5.444 | 50.75% | 0% |
| mixed | 1024 | 2 | 4.055 | 35.43% | 0% |
| mixed | 4096 | 1 | 5.403 | 50.96% | 0% |
| mixed | 4096 | 2 | 4.118 | 38.26% | 0% |
| solid | 1024 | 1 | 1.790 | 47.87% | 0% |
| solid | 1024 | 2 | 1.463 | 30.62% | 0% |
| solid | 4096 | 1 | 1.854 | 41.87% | 0% |
| solid | 4096 | 2 | 1.467 | 30.51% | 0% |

Это измерение данного корпуса на данном ПК, с сохранённым OS cache. Для остальных шести малых наборов цель 30% не достигнута; используется полная матрица. Два результата около 30% близки к границе и чувствительны к фоновой нагрузке. Универсальная цель для любых входов не подтверждена.

До параллельных probes полный прогон тех же 192 случаев прошёл, но solid/workers=2 дал только 22.44% / 24.86% при RAM 1024/4096. Оба прогона сохранены отдельно, со своими SHA EXE. Итог: `benchmark-results/windows-msvc-release-parallel-20261006/`; исходное измерение: `benchmark-results/windows-msvc-release-20261006/`.

Dual и Streaming прошли ещё **12 + 12** запусков того же mixed/solid корпуса, RAM 1024 MiB, worker 1, два режима × три повтора. Размеры равны Disk: mixed 661 255 bytes, solid 68 336 bytes. Медианы финализации:

| Вход / режим | Disk, s | Dual, s | Streaming, s |
|---|---:|---:|---:|
| mixed / Balanced | 0.449 | 0.480 | 0.090 |
| mixed / Exhaustive | 0.386 | 0.440 | 0.091 |
| solid / Balanced | 0.131 | 0.170 | 0.067 |
| solid / Exhaustive | 0.131 | 0.171 | 0.067 |

Финализация включает assembly/verification; это не выделенное время decoder. Наблюдаемый scratch peak практически равен между режимами: ~20.2 MB mixed, ~17.0 MB solid. Он включает поиск и не доказывает уменьшение физического disk peak. Отдельная сводка: `validation/windows-2026-10-06/verifier-comparison.json`. Streaming остаётся экспериментальным, Disk — default.

MSVC **Debug и Release installed-package smoke прошли**: CMake Runtime + Developer установлены в отдельные stage-каталоги, затем скопированы в Unicode-путь `Приложение 日本語`. Из другого CWD выполнены все три test EXE, включая worker/SFX/volumes/long paths, и CLI с Dual; оба CLI report имеют `Completed`. Логи и копии CLI/signature JSON сохранены в `validation/windows-2026-10-06/`; исходные Unicode evidence сохранены в `%TEMP%` (точный путь есть в логах).

Итоговые ZIP создаются из той же сборки с финальной документацией. EXE/worker/test EXE/bin сверяются с прошедшим smoke stage; добавление документации не требует повторной проверки кода. Пакеты: `release/msvc-2026-10-06/debug/` и `release/msvc-2026-10-06/release/`, в каждом portable/developer ZIP и каталоги с SHA256SUMS.

Подписи приложения, worker и всех семи инструментов: **NotSigned**. SHA-256/provenance инструментов совпадают с manifest; цифровая подпись и происхождение по хешу — разные проверки. Статусы сохранены в `authenticode-debug.json` / `authenticode-release.json`.

Для воспроизведения сборки:

```powershell
cmake --preset windows
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
cmake --preset windows-faults
cmake --build --preset windows-faults
ctest --preset windows-faults --output-on-failure
```

Первый Windows benchmark создал фиксированный корпус в `benchmark-results/windows-msvc-release-20261006/corpus`. Повторный benchmark использует именно его SHA manifest:

```powershell
python tools/benchmark.py --exe build/windows/Release/UltraArchStudio.exe --tools bin --output benchmark-results/windows-msvc-release-parallel-20261006 --corpus benchmark-results/windows-msvc-release-20261006/corpus --ram 1024 4096 --workers 1 2 --mode balanced exhaustive --large-mib 16
python tools/benchmark.py --exe build/windows/Release/UltraArchStudio.exe --tools bin --output benchmark-results/windows-msvc-dual-20261006 --corpus benchmark-results/windows-msvc-release-20261006/corpus --ram 1024 --workers 1 --mode balanced exhaustive --dataset solid mixed --verifier dual
python tools/benchmark.py --exe build/windows/Release/UltraArchStudio.exe --tools bin --output benchmark-results/windows-msvc-stream-20261006 --corpus benchmark-results/windows-msvc-release-20261006/corpus --ram 1024 --workers 1 --mode balanced exhaustive --dataset solid mixed --verifier stream
python tools/windows-worker-smoke.py --worker build/windows/Release/verify-worker.exe --tools bin --output validation/windows-2026-10-06/worker-smoke
```

Один полный прогон: восемь наборов × два RAM × два worker × два режима × три повтора = 192 запуска. Balanced сравнивается с Exhaustive того же корпуса/бюджета по медиане wall time и худшему размеру. Малые входы <=2 MiB используют полную матрицу. Цель ≥30% времени и ≤1% размера не является универсальной гарантией.

## Ручные GUI/DPI проверки, требующие ввода пользователя

- DPI 100/150/200%, перенос между мониторами с разным DPI; узкое окно и длинные Unicode-пути, читаемость всех controls.
- Drag-and-drop, tray/minimize/close; pause/skip/cancel/assemble из интерфейса.
- RU/EN labels, точный импорт RAM/solid/CPU/timeout, отмена What-if/preview большой папки и архива.
- История, resume/delete, асинхронный экспорт, выбор и повторная проверка кандидата; redaction диагностического экспорта.

## Другие открытые условия приёмки

- Windows 11; реальные WinRAR целевых версий (WinRAR на этом ПК не найден).
- Disk-full на отдельном ограниченном томе, power loss и аппаратный reset; процессные fault tests их не заменяют.
- Более 64 логических CPU / несколько processor groups, другие сторонние jobs и CPU Sets/physical affinity на целевом оборудовании.
- GUI latency на большой папке и реальная межмониторная DPI-проверка. Нативная сборка подтверждена, визуальная приёмка ещё не выполнена.
- IPO/large pages не включены: измерений их выгоды нет. Windows CI не запускался на удалённом runner.

[Состояние реализации](implementation-status.md) · [Порядок проверок](windows-validation.md) · [План](development-plan.md).
