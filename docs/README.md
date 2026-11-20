# Документация UltraArch Studio

- [Единый актуальный план доводки](implementation-plan.md) — порядок реализации, исправления практических проблем и критерии готовности. Это основной план дальнейшей работы.
- [Выполненный S0: база, проблемы и настройки](../validation/stabilization/README.md) — снимок текущих исходников, нативные сборки, полный контракт настроек и короткий контрольный корпус.
- [S1-01: budget expiry and verified publication](../validation/s1-01/README.md) — regression fix, finalization timing and lifecycle/recovery evidence.
- [S1-02: cancellation and checkpoint boundaries](../validation/s1-02/README.md) — repeated Cancel/resume, generation crashes, marker failure and fallback retention.
- [S1-03: cancellation precedence and decoded verification](../validation/s1-03/README.md) — command guards, concurrent dispatch and cancellation inside the decoder hash sink.

- [Разбор Ultra7z 1.15 от 23 октября](ultra7z-reverse-2026-10-23.md) — пробы, дешёвый Delta-перебор, настройки, графы кодеков и подтверждение сохранённой трассой.

- [Состояние реализации](implementation-status.md) — выполненные работы и оставшаяся приёмка.
- [Исторический разбор Photoshop и план от 11 октября](photoshop-postrun-plan-2026-10-11.md) — исходные проблемы; статус последующих исправлений учтён в едином плане.
- [Windows-приёмка от 6 октября](windows-acceptance-2026-10-06.md) — нативные MSVC проверки, исправления и измерения.
- [Подготовка Windows](windows-setup.md) — инструменты разработки и открытие проекта.
- [Проверки на Windows](windows-validation.md) — сборка, автоматические проверки, benchmark и ручная приёмка.
- [Исторический план после 2.1](development-plan.md) — исходные направления развития; дальнейшая работа ведётся по единому плану выше.
- [Сравнение с Ultra7z от 9 октября](ultra7z-comparison-2026-10-09.md) — завершённые прогоны, размер, время, память и независимая проверка.
- [План улучшения сжатия от 9 октября](compression-improvement-plan-2026-10-09.md) — исторические направления и измеримые критерии.
- [Ход реализации и измерения](compression-progress-2026-10-09.md) — новые группы, промежуточный разрыв и открытая сравнительная приёмка.
- [Карта поиска Ultra7z](search-coverage-2026-10-09.md) — блоки итоговых архивов, парные ETW-события и покрытие методов.
- [Упорядочивание проекта](project-cleanup.md) — выбор актуальных файлов, переименования и результаты проверки.

- [Реализация плана оптимизации](roadmap-implementation-2026-10-09.md) — новые механизмы, реальные проверки и отдельные неподтверждённые цели.
- [План RAM и дисковых записей от 10 октября](next-plan-2026-10-10.md) — историческая постановка; дальнейшая доводка включена в единый план.
- [Реализация RAM-групп](ram-implementation-2026-10-10.md) — настройки, оценка памяти, гибридный вход, проверки и ограничения.
- [Короткие Disk/RAM замеры](ram-benchmark.md) — повторяемая процедура и смысл метрик.

## История

Исторические отчёты описывают состояние на дату их составления. Старые пути сборок в них больше не используются.

- [Аудит от 4 октября 2026](history/2026-10-04-audit.md).
- [Исправления от 4 октября 2026](history/2026-10-04-fixes.md).
- [История разработки](history/project-memory.md).
- `history/development-chat.zip` — архив прежнего чата разработки.
- `images/app-screenshot.png` — сохранённый снимок интерфейса.

[Главная страница проекта](../README.md).
