# Продолжение на Windows-ПК

Дата проверки документации: 2026-10-05. Проект: UltraArch Studio, Windows 10/11 x64.

## Установить Codex и открыть проект

1. Установите приложение по [официальной инструкции OpenAI для Windows](https://learn.chatgpt.com/docs/windows/windows-app). Сейчас Codex находится в desktop-приложении ChatGPT. Официальная команда установки из Microsoft Store:

   ```powershell
   winget install --id 9PLM9XGG6VKS -s msstore
   ```

2. Войдите в свой аккаунт, откройте Codex. В настройках выберите **Windows native** и **PowerShell**. При предложении настройте native sandbox; используйте обычные ограниченные разрешения. Для тестирования Windows API нужен нативный агент, а не Linux-агент в WSL. [Официальная документация sandbox](https://learn.chatgpt.com/docs/windows/windows-sandbox).
3. Перенесите исходники проекта (включая `src`, `vendor`, `bin`, `resources`, `tests`, `tools`, CMake-файлы и документацию) в `C:\Projects\7zOptimizer`. В подготовленном source ZIP есть `git-history.bundle`: для восстановления истории можно выполнить `git clone -b main <путь-к-bundle> C:\Projects\7zOptimizer-Git` и открыть полученную папку. Не переносите macOS build-каталоги. Откройте эту папку через Add project / Ctrl+O. Секреты аккаунта и папку `.codex` переносить не нужно.
4. Установите Visual Studio 2022 Community или Build Tools с workload **Desktop development with C++**: MSVC v143 x64, Windows SDK, CMake tools. Также нужны Git, PowerShell 7 и Python 3 для benchmark. CMake можно взять из VS. После установки перезапустите приложение, чтобы обновился PATH.
5. Отправьте в новом чате Codex текст ниже. Историю этого локального чата считать автоматически перенесённой нельзя; состояние работы находится в файлах проекта.

```text
Открой docs/windows-validation.md и docs/implementation-status.md, затем docs/development-plan.md.
Продолжи полную реализацию плана UltraArch Studio. Работаем на Windows 10/11 x64
нативно в PowerShell. Собери MSVC Debug/Release, запусти автоматические проверки,
installed-package smoke и benchmark. Исправляй найденные ошибки. Проверь verify-worker,
Job Objects/OOM, recovery публикации, Unicode/длинные пути, SFX и тома.
Не объявляй Windows-проверки и цели ускорения подтверждёнными без измерений.
Ручные GUI/DPI проверки, которым нужен мой ввод, перечисли отдельно.
```

## Что требует Windows

- Запуск EXE, MSVC, CNG SHA-256 и verify-worker с `7z.dll`.
- Job Objects, committed-memory/OOM, вложенные jobs, affinity / CPU Sets.
- DirectX GUI: DPI на мониторах, drag-and-drop, tray, сворачивание и shutdown.
- NTFS: длинные пути, reparse points, ADS, ACL и read-only output.
- Выполнение SFX, проверка внешним WinRAR и установленного portable-пакета.
- Измерения скорости/диска/RAM на целевом Windows-ПК.

Кросс-компиляция на macOS проверяет компиляцию, но не перечисленное поведение.
