# Diablo 4 Overlay Rotation Spam

[English](#english) | [Русский](#русский)

Latest version for Windows/Последняя версия для Windows
https://github.com/pdx15/Diablo-4-Rotation-Spam/releases/latest

---

## English

A lightweight, high-performance overlay and macro automation tool for Diablo 4, written in pure C++ using **Dear ImGui** and **DirectX 11**. It bypasses high CPU usage common with scripting engines and provides a minimal footprint.

### Features
* **Minimalist Status HUD:** A tiny `240x100` transparent window showing Game, Script, and Health status directly over the game.
* **Smart Click-Through:** The overlay is completely click-through while gaming. Pressing the custom Options key instantly activates the cursor for configuration.
* **Global Pixel Auto-Heal:** Scans the selected health point and heals when HP is low. By default it follows the combat spam activation condition; enable **Independent operation** to heal without holding a combat mouse button.
* **Interactive Position Picker:** Click a single button in settings, then left-click anywhere on your screen to set the exact health pixel coordinates.
* **Dynamic Combat Spam:** Add any number of skills with custom independent millisecond timers.
* **Persistent Settings:** Automatically saves your hotkeys, profiles, timers, and pixel configurations to `%APPDATA%\d4rt\config.txt`.

### Independent auto-heal
In settings, enable **Global Auto-Heal by HP pixel**, then check **Independent operation**. Auto-heal will ignore **Always / Hold LMB / Hold RMB** while combat spam continues to follow the selected condition. Healing still requires the script to be ON, Diablo IV to be the foreground window, and low HP to be detected; the heal timer is unchanged.

The checkbox is saved per profile. It is off by default, including when loading older configurations, so existing behavior is preserved.

Regression tests and the Windows smoke-test checklist: [tests/README.md](tests/README.md).

### Publishing a release
Maintainers can run **Actions → Release → Run workflow**, select the source branch, and enter a new version, for example `1.0.5.2`. The workflow must first be present on the repository's default branch to appear in the Actions UI.

* Accepts `X.Y.Z` or `X.Y.Z.B`, optionally prefixed with `v`. Components must be `0–65535`, without leading zeros. Choose a version newer than the latest published release so the auto-updater recognizes it.
* Runs regression tests, stamps all version definitions in `version.h`, and builds **Release x64** on Visual Studio 2026 (`v145`). The executable's file/product versions are checked before packaging.
* Publishes tag **`v1.0.5.2`**, title **`Diablo 4 Rotation Spam v1.0.5.2`**, and **`d4rt_v1052.zip`** containing `d4rt.exe`, with Russian/English installation instructions and generated release notes.
* Uses the automatic `GITHUB_TOKEN` with `contents: write`; no additional secrets are required. The tag targets the dispatched commit. `version.h` is changed only in the build workspace; no branch is committed or pushed.
* Creates a draft, uploads the ZIP, then publishes it as Latest. Existing tags/releases are never overwritten. If an interrupted run leaves a draft/tag, inspect and remove only that unfinished release/tag before retrying, or choose another version.

### Interface Preview
<p align="center">
  <img src="assets/hud_preview.png" alt="Minimal HUD Mode" width="240"/>
  <img src="assets/settings_preview.png" alt="Configuration Menu" width="460"/>
</p>

---

## Русский

Легкая, высокопроизводительная утилита-оверлей автоматизации макросов для Diablo 4, написанная на чистом C++ с использованием **Dear ImGui** и **DirectX 11**. Она полностью исключает высокую нагрузку на процессор, свойственную обычным скриптовым движкам, и имеет минимальный размер.

### Возможности
* **Минималистичный HUD статуса:** Крошечное прозрачное окно `240x100` отображает статус игры, скрипта и здоровья прямо поверх игрового клиента.
* **Умный сквозной клик (Click-Through):** Оверлей полностью пропускает клики мыши во время игры. Нажатие клавиши Опций мгновенно включает курсор для настройки.
* **Глобальный автохил по пикселю:** Сканирует выбранную точку на сфере здоровья и прожимает хил при низком ХП. По умолчанию следует условию активации боевого спама; галочка **Независимая работа** позволяет хилиться без удержания боевой кнопки мыши.
* **Интерактивный выбор координат:** Нажмите одну кнопку в меню, кликните левой кнопкой мыши в любой точке экрана игры, и точные координаты ХП запишутся автоматически.
* **Динамический боевой спам:** Возможность добавлять любое количество клавиш с уникальными независимыми таймерами задержки в миллисекундах.
* **Сохранение конфигурации:** Автоматически записывает хоткеи, профили, таймеры и координаты пикселей в `%APPDATA%\d4rt\config.txt`.

### Независимый автохил
В настройках включите **Глобальный автохил по пикселю ХП**, затем отметьте **Независимая работа**. Автохил будет игнорировать условие **Всегда / Зажата ЛКМ / Зажата ПКМ**, а боевой спам продолжит следовать выбранному условию. Для хила по-прежнему нужны включённый скрипт, активное окно Diablo IV и обнаруженное низкое ХП; таймер хила не меняется.

Галочка сохраняется отдельно для каждого профиля. По умолчанию, в том числе при загрузке старых конфигураций, она выключена — прежнее поведение сохраняется.

Регрессионные тесты и чек-лист проверки в Windows: [tests/README.md](tests/README.md).

### Публикация релиза
Для сопровождающих: откройте **Actions → Release → Run workflow**, выберите ветку с исходниками и введите новую версию, например `1.0.5.2`. Чтобы workflow появился в интерфейсе Actions, его файл сначала должен попасть в основную ветку репозитория.

* Поддерживаются `X.Y.Z` и `X.Y.Z.B`, в том числе с префиксом `v`. Каждое число — от `0` до `65535`, без ведущих нулей. Выбирайте версию новее последнего опубликованного релиза, чтобы её распознал автоапдейтер.
* Запускаются регрессионные тесты, обновляются все определения версии в `version.h`, собирается **Release x64** на Visual Studio 2026 (`v145`). Перед упаковкой проверяются файловая и продуктовая версии EXE.
* Создаются тег **`v1.0.5.2`**, заголовок **`Diablo 4 Rotation Spam v1.0.5.2`** и архив **`d4rt_v1052.zip`** с `d4rt.exe`, русско-английской инструкцией в описании релиза и автоматически сформированным списком изменений.
* Используется автоматический `GITHUB_TOKEN` с правом `contents: write`; дополнительные секреты не нужны. Тег указывает на коммит запуска. `version.h` изменяется только в рабочем каталоге сборки — коммиты и push в ветки не выполняются.
* Сначала создаётся черновик и загружается ZIP, затем релиз публикуется как Latest. Существующие теги и релизы не перезаписываются. Если прерванный запуск оставил черновик/тег, проверьте и удалите только этот незавершённый релиз/тег перед повтором либо выберите другую версию.

### Внешний вид интерфейса
<p align="center">
  <img src="assets/hud_preview_ru.png" alt="Мини-панель статуса" width="240"/>
  <img src="assets/settings_preview_ru.png" alt="Меню настроек" width="460"/>
</p>

[![ViewCount](https://views.whatilearened.today/views/github/pdx15/Diablo-4-Rotation-Spam.svg)](https://github.com/pdx15/Diablo-4-Rotation-Spam)
