# CI/CD XLibrary

Автоматические проверки запускаются для PR **в `main`** и для merge queue
(`merge_group`). `push` не запускает workflow: после успешного PR и его merge
тесты повторно не выполняются. `workflow_dispatch` позволяет вручную проверить
выбранную ветку. Новое изменение PR отменяет устаревший запуск того же workflow;
ручные, PR и merge queue запуски не отменяют друг друга.

Merge queue проверяет сочетание PR с актуальной базой и другими PR очереди.
Это отдельная проверка другого дерева исходников до merge. При обычном merge
GitHub требует актуальную базу благодаря `strict_required_status_checks_policy`.
Проверки выполняются и для draft PR: переход в ready не требует второй сборки.
Используются стандартные PR-события `opened`, `synchronize`, `reopened`;
редактирование заголовка и описания не запускает CI. Если у существующего PR
сменили base на `main`, переоткройте PR или добавьте commit для запуска checks.

## Покрытие

| Workflow | Проверки |
| --- | --- |
| Windows Build | MSVC Debug/Release, MinGW Debug/Release; Qt 6.8.3, x64, `XLIBRARY_NATIVE_CPU=OFF` |
| Windows Build / MSVC Release | PowerShell parser для всех отслеживаемых `.ps1`; CMake install, наличие Qt DLL, плагина `qwindows`, лицензий; `--check-startup` с очищенным Qt-окружением и `PATH`, из другой рабочей папки, install-путь с Unicode и пробелами |
| Linux Build | GCC Debug/Release, Clang Debug/Release; Qt 6.8.3; X11/Xvfb startup, CMake install, desktop entry, offscreen startup |
| Linux Build / packages | Debian 13 и системный Qt; AppImage и DEB, startup offscreen/X11, SHA256, установка и удаление DEB |
| Workflow Lint | actionlint с проверкой SHA256 инструмента, Python parser упаковщиков, CMake presets, JSON-конфигурация |
| Prepare Release Draft | Ручной запуск из main, VERSION/SemVer/тег, один SHA; MinGW ZIP/setup, Debian 13 AppImage/DEB, startup/установка/удаление, метаданные и общий SHA256SUMS; только Draft после успеха обеих платформ |
| Clang Format | Все отслеживаемые C/C++ файлы, clang-format 18.1.8 |

Обычный CI не публикует Release, не создаёт теги и не выполняет deploy.
Linux-пакеты доступны как artifacts 14 дней; публикация выпуска остаётся отдельным
решением по [инструкции](../BUILDING.md#github-pre-release).
Release workflow существует отдельно от обычного CI: подготавливает Draft и тег,
но **Publish release** выполняется исключительно владельцем вручную.
Release jobs имеют отдельные таймауты: Validation — 5 минут, сборки — 45,
Draft — 15. Существующий CI с MinGW и локальные упаковщики сохранены.
Корневой `aqtinstall.log`, который создаёт установщик Qt, исключён из Git:
служебный лог не делает исходники изменёнными. Проверка `source_dirty=false`
остаётся обязательной; при несовпадении метаданных Windows выводятся
ожидаемые значения и фактический `build-info.json`, включая `source_changes`.
Текстовая версия установщика сравнивается с `VERSION` после удаления только
завершающих пробелов из PE-поля `ProductVersion`; номер и SemVer-суффиксы
по-прежнему должны совпадать точно.

## Время и диагностика

Сборки имеют лимит **10 минут на job**, lint и форматирование — 5 минут.
Jobs работают параллельно. Ориентир всего PR — 7–10 минут после старта runners;
очередь GitHub, сеть, cache miss и загрузка runner могут увеличить общее ожидание.
Таймаут означает ошибку проверки, а не пропуск оставшейся работы.

Qt SDK кэшируется install-qt-action. Linux-сборки используют ccache до 200 МиБ
на конфигурацию, отдельные ключи для preset, Qt и CMake-входов с восстановлением
предыдущего кэша. Готовые install-каталоги не кэшируются. Linux использует
доступные ядра runner, Windows и упаковщик — до четырёх параллельных задач.
Windows release дополнительно кеширует проверенные исходные архивы Qt по хешу
manifest; таймаут одной попытки скачивания — 300 секунд, максимум три попытки.
Проверка SHA256, копирование и извлечение лицензий отмечаются отдельно;
извлечение через CMake ограничено 120 секундами на архив, его диагностика
сохраняется в `packaging-release.log` каталога сборки.

Каждый внешний startup check ограничен 45 секундами; внутренние проверки
Linux-упаковщика — 60 секундами. Windows install step ограничен двумя минутами.
При сбое сборки сохраняются CMake cache/configure logs на 7 дней; Windows также
сохраняет stdout/stderr запуска и Qt plugin diagnostics в `build/install-check`.

## Защита `main`

[rulesets/main.json](rulesets/main.json) — шаблон настроек GitHub, а не автоматически
применяемый workflow. После merge администратор должен обновить ruleset `main-ci`
через Settings → Rules → Rulesets или GitHub API. Включены требование PR,
разрешение review threads и все 11 обязательных checks. Перед слиянием нужен
как минимум один approval от code owner; новые коммиты сбрасывают прежние
одобрения. [CODEOWNERS](CODEOWNERS) назначает `@lappland-open` maintainer для всех
путей, включая сам CODEOWNERS, CI и ruleset. Чтобы делегировать ревью другому
maintainer, добавьте его в CODEOWNERS и предоставьте ему доступ на запись.
GitHub использует CODEOWNERS из целевой ветки: сначала нужно слить этот файл
в `main`. Автор PR не может одобрить собственный PR.

Существующий bypass сохранён только для роли **Repository admin**
(`RepositoryRole`, `actor_id: 5`, режим `always`). Администраторы могут обходить
эти требования; остальные участники должны отправлять изменения в `main` через
PR с одобрением maintainer и успешными checks. Прямые push для них запрещены;
автоматических push-проверок больше нет.
Перед применением новых required checks нужно дождаться успешного CI с этими именами.

Изменение JSON в репозитории само по себе не меняет действующие серверные правила.
При удалении обязательных checks сначала уберите их из серверного ruleset,
до слияния PR: иначе GitHub будет ждать проверки, которые больше не запускаются.
Имена jobs используются ruleset: при переименовании workflow jobs или матрицы
нужно одновременно обновлять этот шаблон и настройки GitHub.

В workflows используются минимальные permissions, checkout без сохранения
credentials и actions, закреплённые на commit SHA. Dependabot еженедельно
обновляет GitHub Actions одной группой PR. `pull_request_target` с выполнением
кода PR не используется.

## Ограничения

`--check-startup` подтверждает создание приложения и временного профиля,
но не заменяет интерактивные проверки импорта, сканирования, обновления,
сохранности пользовательского профиля или установки Inno Setup.
MSVC install использует CRT, установленный на runner: состав app-local CRT
проверяется отдельно при выпуске Windows-пакетов через `check_package.ps1`.
Дистрибутивные AppImage/DEB проверяются на базе Debian 13; запуск на других
дистрибутивах и полноценный Wayland-сеанс требуют отдельной проверки.

## CTest в v0.7.0

Все четыре Linux preset и четыре Windows-конфигурации после сборки выполняют
`ctest --output-on-failure` (Windows также передаёт `-C` для multi-config).
Ненулевой код завершения останавливает job; LastTest.log включён в артефакты
диагностики. Существующие имена обязательных jobs сохранены. Для локального
запуска см. раздел «Автоматические тесты» в BUILDING.md. Изменение настроек
branch protection через API не требуется и этой задачей не выполняется.
