# Testing and manual verification

These are existing checks and release scenarios. Run the relevant checks for the changed behavior and respect any task-specific restrictions. A passing build, offline test suite and startup check are separate evidence; none alone proves the complete desktop workflow.

## Existing automated checks

CTest is enabled by default with `BUILD_TESTING=ON` and requires Qt Test matching the application Qt. Tests use temporary profiles/local fixtures; they do not require real GitHub/Steam access or the user's catalog. Linux portal testing runs inside its own `dbus-run-session`.

```sh
cmake --preset linux-debug -DBUILD_TESTING=ON
cmake --build --preset linux-debug --parallel 4
ctest --test-dir cmake-build-linux-debug --output-on-failure
```

The same commands apply to `linux-release`, `linux-clang-debug` and `linux-clang-release` with their matching directories. For an already [configured Windows build](building.md#windows), including multi-config MSVC:

```powershell
cmake --build build-msvc --config Release --parallel 4
ctest --test-dir build-msvc -C Release --output-on-failure
```

Use `-DBUILD_TESTING=OFF` for an application-only build. Tests and Qt Test are not installed with the application. Current targets and environment settings are in [tests/CMakeLists.txt](../tests/CMakeLists.txt); [CI](ci.md) runs them across its build matrix.

## Startup checks

`--check-startup` creates the application/window and catalog in a temporary profile, then exits. It must not modify the normal/portable profile and does not perform the startup network update check.

```sh
./cmake-build-linux-debug/XLibrary --check-startup
QT_QPA_PLATFORM=xcb xvfb-run -a dbus-run-session -- ./cmake-build-linux-debug/XLibrary --check-startup
QT_QPA_PLATFORM=offscreen ./cmake-build-linux-debug/XLibrary --check-startup
```

For Xvfb install `xvfb`, `xauth`, `dbus-x11` and Qt xcb runtime dependencies. Offscreen startup cannot prove a visible window, hotkeys, tray integration, multi-monitor behavior or a real Wayland session.

## Windows distribution and profile

Use the [Windows packager](releasing.md#windows-packages), unpack its entire ZIP to a writable directory with Unicode/spaces, then check the unpacked application root:

```powershell
./.github/scripts/check_package.ps1 -PackagePath 'path/to/unpacked/XLibrary' -Compiler mingw
Get-FileHash 'path/to/package.zip' -Algorithm SHA256
```

For MSVC use `-Compiler msvc`. Compare hashes with the output's `SHA256SUMS.txt`. The script checks dependencies, metadata, licenses and compiler runtime, then starts a copy with Qt/toolchain paths removed, from another working directory. It checks portable/installed startup modes and preservation of sentinel profile files; the original unpacked package is not modified. Logs go to `build/package-check/`.

Before public distribution, use a clean Windows system without Qt or the compiler toolchain and a backed-up/test profile:

1. Launch portable normally. Add an application, tag and scenario, change a setting, exit fully through the tray menu and restart. Check persistence under `data/` beside the executable.
2. Move the complete portable directory to another Unicode/space path. Check launch and persistence independent of the current working directory.
3. Run `--check-startup` and confirm existing data is unchanged. With write permission removed from a disposable portable directory, normal startup must report failure instead of silently selecting another profile.
4. Install the setup EXE without administrator privileges. Confirm the installed directory has no `portable.flag` and uses the installed profile locations in the [profile guide](profile.md). Check persistence, upgrade and uninstall without losing the user profile.
5. Verify upgrade from an older MinGW installation and the [update scenarios](#updates). A successful installer launch is not proof of completed installation or preserved data.

Portable and installed profiles are separate. Back up and stop the application before manual profile moves or upgrade checks.

## Linux packages and desktop

Verify checksums from the package directory with `sha256sum --check SHA256SUMS.txt`. Test AppImage offscreen and through X11/Xvfb, then interactively in the intended desktop. Check normal launch/restart, profile persistence, moving the AppImage together with `<filename>.data`, and that `--check-startup` leaves existing data unchanged.

Install, launch and remove the DEB in a **clean Debian 13 without Qt SDK or build packages**; use `QT_QPA_PLATFORM=offscreen XLibrary --check-startup` for the isolated startup step. Build-container checks do not prove this environment. See [the compatibility baseline](releasing.md#linux-compatibility-baseline).

Check discovery, launching, process status, tray behavior and autostart on the target desktop. X11 hotkeys/window activation and Wayland compositor restrictions need their own sessions; an offscreen run cannot establish them. Check DPI and multiple monitors where relevant.

## Import and scanning

Use a disposable profile or copy. Catalog JSON format 1 contains applications and assigned tags; it is not a full profile backup. See [catalog management](catalog.md).

1. On Linux, export an entry such as `/usr/bin/sleep`, import it into an empty profile and compare path, arguments, working directory, favorite state and tags.
2. In a multi-entry JSON file, make the second `favorite` a string or its path relative. The error should identify the entry/field and import nothing, including the first entry. Also check invalid tags, NUL, empty names, invalid root JSON, unsupported version and wrong `applications` type.
3. Scan twice and inspect added/duplicate/skipped/error counts. Open and copy details from the library and settings; reasons should identify sources and full paths.
4. Cancel discovery and, separately, candidate saving before commit. Compare data with the original: no partial additions should remain. Retry immediately. Cancellation is unavailable during commit; a late request must not be reported as a rollback.

## Search and files

Use a disposable profile and a small folder of documents, images, unsupported executables, and hidden/build subdirectories.

1. Check names, aliases, tag combinations, exclusions, initials, typo suggestions, and English/Russian layout correction in both search surfaces.
   Compare match categories and positions before checking favorites and usage ordering. Clear the query and sidebar filters when checking the full catalog.
2. Enable Basic file search, add an additional recursive folder and file/folder exclusions, save, and refresh the index. Check plain names, extension filters, `*.pdf` and `report-??.txt` masks;
   excluded files must stay absent, and an empty query must not show indexed files.
3. Switch file search Off during scanning, then back to Basic or Full. Off must stop indexing; enabled modes must resume it. Change the query and folder scope while work is pending;
   old results must not overwrite the new query or scope. Check Basic depth limits and recursive standard/additional folders.
   Confirm Full is marked not recommended. The Files switch must appear in both search surfaces only in Basic and Full.
   Toggle it with a query and a pending file request: off shows only applications, on shows only files, and both windows stay synchronized.
   In file mode, the main workspace hides application rows and keeps the details pane with “Nothing to display here :(”.
   Check the same placeholder with an empty application list; switching modes must not move the Files switch.
   Disable indexing while files are selected:
   both switches must disappear and application search must return. Re-enable indexing and check the switch again.
   Search during scanning and keep a query open: progress and application discovery must not move its results.
   Restart after a completed scan: the cached index must be searchable without another scan before six hours; manual refresh must bypass the interval.
4. Save a result to My files, restart, and check persistence. Removing it from My files must keep the disk file.
   Open files to check usage ordering, then clear usage history. Restore a full profile backup and confirm saved paths and history return.
5. Check keyboard result actions, calculator copying, and direct folder browsing separately from indexed search.
   Exit while scanning and restart; verify shutdown and subsequent indexing without using the normal profile as a fixture.

## Updates

Use [the update guide](updates.md) for channels, installer eligibility and expected behavior. Check both manual requests and the normal startup notification; `--check-startup` skips networking.

1. Verify current/newer local versions never offer a downgrade. Stable excludes GitHub prereleases; Preview includes them. Version ordering follows SemVer, not publication dates. Change channels during a request and confirm the previous channel's installer cannot be used.
2. On an older installed Windows copy, check version confirmation, cancellation, retry, progress and closing settings during the request. Cancel/failure should leave XLibrary open and remove the partial download.
3. Accept a valid update: the ordinary interactive Inno Setup wizard should use the prior installation directory and XLibrary should exit normally. Confirm completed installation, preserved profile and new version startup.
4. For portable, Linux, or a release without an eligible installer/digest, verify the confirmed action opens the specific release page.
5. Check offline operation, timeout, API limit, malformed metadata, size/hash mismatch and failure to start the installer. Errors must be visible and retry remain available.
6. Repeat checks without a token inside 60 seconds, then with a token inside 5 seconds. Results, errors and cancellations retain the interval across settings close/reopen; reading the cache must not extend it. After expiration, a new request should run. Token/channel changes must not reuse an unrelated cached result.

## Optional local profiling

The existing timing tool is opt-in, is not a CTest test and is not installed:

```sh
cmake --preset linux-debug -DXLIBRARY_BUILD_BENCHMARKS=ON
cmake --build --preset linux-debug --target poll_benchmark
QT_LOGGING_RULES=xlibrary.poll.debug=true ./cmake-build-linux-debug/tests/poll_benchmark
# The same logging category is available in the application:
QT_LOGGING_RULES=xlibrary.poll.debug=true ./cmake-build-linux-debug/XLibrary
```

Multi-config Windows builds place the executable under `tests/<configuration>/`. Do not run profiling when the task excludes benchmarks.
