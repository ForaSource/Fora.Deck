# Architecture

XLibrary is a C++23 desktop application built with Qt Widgets and SQLite. It has
two primary application targets: `xlibrary_core` for the backend and `XLibrary` for the desktop
application. The backend uses Qt Core, Gui and Concurrent; on Linux it also uses
Qt DBus. It does not depend on widgets or headers in `desktop/`.

## Component ownership

| Component | Responsibility |
| --- | --- |
| [`main.cpp`](../main.cpp) | Application startup, profile selection, profile lock, single-instance IPC and command dispatch. |
| [`Manager`](../src/Manager.h) | Catalog operations and worker lifecycle, discovery review, launch coordination, availability and process polling. |
| [`Database`](../src/Database.h) | SQLite connection, catalog reads/writes, transactions, metadata, removal undo and path relocation. |
| [`Application`](../src/Application.h) | Target resolution, discovery, launch identities, launch modes and platform-specific application behavior. |
| [`Heartbeat`](../src/Heartbeat.h), [`WindowManager`](../src/WindowManager.h) | Process identity/monitoring and native window operations. |
| [`FileManager`](../src/FileManager.h) | File indexing and queries, saved files and asynchronous file work. |
| [`ProfileSchema`](../src/ProfileSchema.h), [`ProfileBackup`](../src/ProfileBackup.h), [`ProfileSettings`](../src/ProfileSettings.h) | Schema validation/migration and profile backup, recovery and settings handling. |
| [`SearchQuery`](../src/SearchQuery.h), [`LaunchIdentity`](../src/LaunchIdentity.h), [`LaunchArguments`](../src/LaunchArguments.h) | Shared search matching and launch identity/argument rules. |
| [`DesktopService`](../desktop/DesktopService.h) | Qt-facing coordination, operation state, settings, selection, scenarios and signals consumed by the UI. |
| [`LibraryModel`](../desktop/LibraryModel.h) | Application model and filtered/sorted views. |
| [`MainWindow`](../desktop/MainWindow.h), [`LibraryViews`](../desktop/LibraryViews.h), [`SettingsView`](../desktop/SettingsView.h), [`editors`](../desktop/editors.h) | Windows, navigation, library views, settings and editing forms. |
| [`SearchResults`](../desktop/SearchResults.h), [`SearchOverlay`](../desktop/SearchOverlay.h), [`SearchTools`](../desktop/SearchTools.h) | Search presentation shared by the library and compact launcher. |
| [`PlatformIntegration`](../desktop/PlatformIntegration.h) | Desktop integration, including global shortcuts and appearance-related platform behavior. |
| [`UpdateChecker`](../desktop/UpdateChecker.h), [`GithubTokenStore`](../desktop/GithubTokenStore.h) | Release queries/downloads and OS-backed credential storage. |

Platform-specific backend code lives in `src/Windows/` and `src/Linux/`.
`desktop/WindowsIntegration.cpp` and `desktop/LinuxIntegration.cpp` provide the
corresponding desktop integration. New backend behavior belongs with its existing
owner rather than in a second desktop-only catalog or process implementation.

## Threading and operation flow

`DesktopService` creates a `Manager` and moves it to a dedicated `QThread`.
Catalog operations are queued to that worker; results and catalog snapshots return
through Qt signals. `Manager` owns the `Database`. Widgets consume the desktop
service and models rather than opening a competing catalog connection.

Discovery and launch-profile work use separate thread pools. `FileManager` owns
its scan and query pools. Request IDs and generation/cancellation tokens allow
consumers to discard stale work. These mechanisms are part of the operation
contract: closing a preview, changing a query or cancelling a scenario must not
apply an old result to the new state.

Discovery and JSON import prepare records for review before catalog writes.
`Database::import_records` owns the transaction and conflict policy. Cancellation
is checked before commit; once commit begins, cancellation is no longer accepted.
Startup inventory checks reuse discovery and the same review/write path, with
quieter presentation than a manual scan.

Scenario scheduling and per-step state live in `DesktopService`. Validation and
launching reuse `Manager` and `Application`, including launch profiles and native
target behavior. Steam executable metadata helps display and monitor a game;
launching remains a Steam operation.

## Profiles and persistence

Profile selection happens before the desktop service starts. Each profile has an
instance lock and an IPC identity derived from its canonical path. A second
invocation forwards its command to the existing instance for that profile.
`--check-startup` uses a temporary profile.

SQLite stores catalog data; INI settings store preferences. Schema handling and
profile backup/restore are separate from application-only JSON import/export.
JSON is useful for exchanging catalog entries, while a full profile backup
preserves a broader set of data. Credentials use the OS credential store and are
not part of a portable profile backup. See [profiles and recovery](profile.md)
for storage locations and backup operations, and [verification](testing.md#startup-checks)
for startup checks.

The file index is managed by `FileManager` separately from the application
catalog. Saved-file state is synchronized with profile settings. Shared types and
limits live in [`CatalogTypes.h`](../src/CatalogTypes.h) and
[`OperationTypes.h`](../src/OperationTypes.h).

## UI extension boundary

[`UiRegistry.h`](../desktop/UiRegistry.h) defines named widget factories, default
regions, replacements, styles and the `UiPlugin` interface. Plugins are native
code loaded into the application process. IID and Qt metadata checks reject some
incompatible modules; they do not provide isolation or a stable compiler/runtime
ABI. Only trusted modules built for a compatible environment should be enabled.

The registry remains at a stable address during plugin configuration. Failed
configuration restores its previous contents. Optional plugin name, version and
author metadata can be inspected without loading a disabled module.

## Build and delivery boundaries

[`CMakeLists.txt`](../CMakeLists.txt) is the source of truth for dependencies,
targets and platform selection. Qt is dynamically linked; SQLite is compiled
from a hash-checked amalgamation. Packaging scripts and configuration live in
`packaging/`; hosted automation lives in `.github/`.

Build, package, startup and interactive UI checks establish different facts.
See [BUILDING.md](../BUILDING.md) and the [release guide](releasing.md) for the
commands and supported package baseline. The [design guide](design.md) describes
the interface direction and distinguishes implementation from earlier concepts.
