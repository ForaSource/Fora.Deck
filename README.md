<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/github_header_dark.png">
    <source media="(prefers-color-scheme: light)" srcset="assets/github_header_light.png">
    <img src="assets/github_header_light.png" alt="XLibrary — Application launcher and organizer for Windows and Linux" width="100%">
  </picture>
</p>

<p align="center"><strong>Your apps, organized your way.</strong></p>

<p align="center">
  <a href="https://github.com/Lappland-Julia/XLibrary/actions/workflows/windows-build.yml"><img src="https://github.com/Lappland-Julia/XLibrary/actions/workflows/windows-build.yml/badge.svg" alt="Windows build"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-68717C" alt="License: MIT"></a>
  <a href="https://github.com/Lappland-Julia/XLibrary/actions/workflows/linux-build.yml"><img src="https://github.com/Lappland-Julia/XLibrary/actions/workflows/linux-build.yml/badge.svg" alt="Linux build"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%2F%20Linux-68717C" alt="Platform: Windows and Linux">
</p>

<p align="center">
  <a href="#features">Features</a> ·
  <a href="#search">Search</a> ·
  <a href="#development">Development</a> ·
  <a href="#contributing">Contributing</a> ·
  <a href="https://github.com/Lappland-Julia/XLibrary/issues">Feedback</a>
</p>

## XLibrary

XLibrary is an open-source application launcher and organizer for Windows and Linux. Bring your everyday tools, games, and portable apps into one library, organize them with tags, and launch them from a single place.

An app can belong to more than one part of your day. Give your browser both `#work` and `#personal`, keep development tools under `#dev`, or create a launch scenario that opens everything you need for a project.

> **Under active development.** Features and interfaces are still evolving. Early feedback and contributions are welcome.

<p align="center">
  <h3 align="center"><a href="https://github.com/Lappland-Julia/XLibrary/releases">Download Latest Release</a></h3>
</p>

## Features

| | What you can do |
| --- | --- |
| **One application library** | Discover Windows system apps, registry and Start menu entries, Linux desktop entries, and installed Steam games across local libraries. |
| **Tags and filters** | Assign multiple tags to an app and combine them with text search and exclusions. |
| **Favorites and recent launches** | Keep frequently used apps close and sort your library by last launch. |
| **Running apps** | See what is running, switch to an existing window when available, or open a new instance. |
| **Launch scenarios** | Open a group of apps in a defined order, with optional delays between steps. |
| **Quick search** | Open a compact launcher with Ctrl+Alt+Space, calculate expressions, or browse a folder directly. |
| **Desktop integration** | Assign global hotkeys to the library, quick search, individual applications, and launch scenarios. |
| **Browser profiles** | Choose normal, private, or detected local profiles for Chrome, Edge, Firefox and Brave, including in scenarios. |
| **Appearance** | Choose a light, dark, or system theme and adjust the text size. |
| **Local storage** | Keep your catalog in SQLite. Import and export application entries, tags, and favorites as JSON. |

## Import and discovery

JSON format 1 stores application names, typed launch targets, launch arguments, working directories,
browser defaults, aliases, favorites, and assigned tags. Executable targets use absolute paths;
system URI, AUMID and Steam targets use their own validated identifiers. Older catalogs remain supported.
Linux executable paths do not need a filename extension. Optional
fields may be omitted; supplied fields must have the expected JSON types. Import validates the
entire file before writing and reports the record number, field, and reason on invalid input.
An import error leaves the catalog unchanged. Files are limited to 32 MiB and 50,000 applications.

Discovery, JSON import, and files dropped onto the main window open a preview with checkboxes,
search, **Select all**, **Clear selection**, and a selected count. Review names, sources, targets,
arguments, and working directories before choosing **Import selected**. Select/clear applies to
visible search results; hidden rows keep their selection. Cancelling the preview imports nothing.
Windows accepts EXE, LNK, BAT, and CMD files; Linux accepts desktop entries and executable files.
Rejected files are listed with a reason. Discovery can remember unchecked configurations;
**Settings → Library → Reset discovery exclusions** makes them eligible again.

JSON conflicts are skipped by default. The explicit replacement policy replaces the matching
entry's name, search aliases, assigned tags, and favorite while preserving its ID, history,
and scenario links.
Discovery always preserves existing metadata. For repeated configurations within one JSON file,
the first selected entry wins with skip; the last selected entry wins with replacement.

Discovery keeps a report in the library and in Settings → Library. Open **Last scan details** to
read or copy it. The summary separates additions, duplicates, skipped entries, and errors.
Repeated launch configurations are skipped; existing entries keep their metadata and collect
discovery sources. Names may repeat, and one EXE may have multiple argument/working-directory
profiles. Symlinks share a canonical target while retaining their invocation paths.
On Windows, an omitted working directory and the executable's own folder describe the same
launch configuration. Discovery prefers a Start menu shortcut over an App Paths entry with
the same name and executable, keeping the shortcut's arguments. Reimporting that shortcut
also consolidates existing same-name cards with equivalent launch configurations, preserving
tags, aliases, favorites, launch history, sources, and scenario steps. Cards stay separate if
their combined metadata exceeds catalog limits. Different arguments or effective working
directories remain separate profiles. Manual addition rejects a configuration already in the library.
Cancelling discovery before its transaction is committed rolls back all additions. Once commit
starts, cancellation is unavailable. JSON import and discovery never launch applications.

Windows discovery includes available Settings pages, File Explorer, Calculator, Microsoft Store,
Task Manager, Control Panel and Windows Terminal. Unavailable components are skipped.
Steam discovery reads local `libraryfolders.vdf` and `appmanifest_*.acf` files, including additional
libraries. Games launch through `steam://run/<appid>`; moving a game and rescanning preserves its
card ID and metadata. Steam files are never changed, and no Web API or login is required.

JSON export transfers application cards. Use a complete `.xlprofile` backup for scenarios,
global hotkey assignments, unused tags, saved filters, settings, and launch history.

Use Ctrl/Shift to select several library entries. The selection bar shows the count, clears the
selection, and opens a bulk editor with searchable tags, usage counts, and favorite state.
Mixed values remain unchanged until explicitly checked or unchecked. Bulk removal asks for the
entry count and deletes only catalog entries. **Undo removal** restores the last deletion in the
current session, including metadata and scenario steps. It refuses to overwrite changed linked
scenarios or conflicting new entries. Undo is also offered in the shared notification banner.

Application details show discovery sources and offer path/argument copying. Availability refreshes
periodically without removing entries. Unavailable Windows drives and previously observed Linux
mounts are distinguished from missing files; an unknown missing location is kept as unavailable.

## Search and saved filters

Search matches names, filenames, and user-defined aliases from **Edit application**.
Ties keep a stable sort order.
Case, repeated spaces, Unicode normalization, and English/Russian keyboard layout are
handled by the same cached search rules in the library and quick launcher. Initials (`vsc`),
word prefixes (`vscode`) and limited typo correction (`discrod`, `chrme`, `stema`) are supported.
Exact names and aliases rank first, followed by exact filenames, prefixes, abbreviations,
substrings, layout correction and fuzzy matches. Tags stay exact, and the entered query is preserved.
Aliases also travel with JSON catalogs; older catalogs without aliases remain importable.

Use `#tag` to require a tag and `-#tag` to exclude it; every condition must match.
The tag selector near search inserts suggestions. An incomplete `#` shows a prompt;
an unknown required tag produces no matches, and unknown exclusions hide nothing.
The active summary shows the query, section, selected tag, and sort order.
**Clear all filters** returns to the full library with the default sort, including from **No results**.

**Save filter** stores the current query, exclusions, section, selected tag, and sorting.
Choose it from **Saved filters** to apply it; the adjacent actions rename or remove it.
Renaming a tag updates saved and active queries. Removing a tag removes its conditions
from saved and active filters, as explained in the confirmation. Saved filters are stored
in the profile database and included in complete `.xlprofile` backups and their preview.

## Profile backup and recovery

**Settings → Profile** creates a complete `.xlprofile` backup with applications, launch
configurations, search aliases, saved filters, all tags, scenarios, settings, discovery sources,
and launch history. Before restoration, XLibrary checks the format, schema, date, contents, and file fingerprint and shows
a preview. Replacing the profile preserves its previous database, journals, settings, and a
complete backup when readable. An interrupted replacement rolls back at the next startup;
after a successful restore, restart XLibrary to apply all settings.

Backups contain private paths and arguments. Native plugin binaries and OS-stored GitHub tokens
are not transferred; the local token remains local. JSON catalog import/export remains separate.
Schema migrations are sequential and transactional; an unsupported newer schema is refused
without changing it. Existing profiles receive a `.before-schema-v4.*.bak` snapshot before migration.

Start `XLibrary --safe-mode` to use the built-in interface without loading user plugins. Disable
the faulty module in **Settings → Plugins**, save, and restart. The profile page shows database
and settings locations, schema/product versions, portable/installed mode, and the last error.
Its diagnostic export excludes catalog contents, arguments, settings values, tokens, and raw errors.

For a missing application, use **Locate application…** in its details. **Settings → Profile →
Recover moved folder…** previews replacements for a moved directory before applying selected
valid entries together. Card IDs, tags, favorites, history, arguments, and scenario references
are preserved.

Backend code lives in `src/` and is built into `xlibrary_core`. `Database` owns the
connection, schema migrations, cards and their relations. `Manager` owns the catalog,
worker operations, request results and launch history. `Application` handles discovery,
resolution and launch on both platforms; `Heartbeat` monitors running applications.
`WindowManager` provides theme policy and native window activation. The desktop layer
owns widgets, models, UI state, scenario timers and native interface integration, and
connects these to the backend through `DesktopService`. Linux user arguments share
one core parser; freedesktop `Exec` retains its separate standard format.

## Updates

XLibrary checks this project's GitHub Releases at startup. If a newer version is available,
an in-app notification links to **Settings → Update**, where you can confirm the update.
You can also use **Settings → Update → Check for updates** at any time.
The notification stays visible while checking and then shows the result, including when
the latest version is already installed, no releases are found, or the check fails or is cancelled.
You can optionally enter your own GitHub Personal Access Token in the same section.
For this public repository, a token without additional permissions is sufficient.
Click **Save token** to store it in Windows Credential Manager or the Linux secret store
(Secret Service/KWallet); **Delete token** restores unauthenticated checks. These two
buttons take effect immediately, independently of the settings dialog's Save/Cancel buttons.
The token is sent only to `api.github.com`, including subsequent release-list pages, and
is never attached to installer downloads. It is not written to `settings.ini` or catalog exports.
If the secret store is unavailable or locked, XLibrary reports the error and never falls back
to plaintext storage. A saved token belongs to the OS user and local profile path; enter it
again after moving a portable profile. A token is optional; checks work without one.

Check results are cached locally for **60 seconds without a token** or **5 seconds with
a token**, measured from completion. Repeated clicks return the cached result without
new GitHub requests. Errors and cancellations use the same interval. The cache stays in
memory for the application session, including when Settings is closed and reopened.
Unauthenticated checks and each personal token have separate cache entries; tokens
themselves are not stored in the cache. Reading a cached result does not extend its lifetime.

Checks run at startup and on request and include published pre-releases. Versions are compared using
Semantic Versioning; drafts and tags without a supported version are ignored.

On Windows x64, an installer-managed copy can download the matching MinGW or MSVC installer
after confirmation. XLibrary verifies its size and GitHub-provided SHA-256 before starting
the installation wizard in the existing installation directory, then exits normally. The
wizard completes the update and offers to reopen XLibrary; the library and settings stay
in the existing profile. Downloads can be cancelled; closing Settings cancels checks and downloads
started from that dialog. The automatic startup check continues if Settings is closed.

Portable copies, Linux installations, and releases without a matching installer and SHA-256
open the release in your browser after confirmation. Follow its update instructions and keep
your portable data folder; on Linux, use the appropriate package or AppImage workflow.
No downloads or installations happen in the background.

## Search

Search by app name or executable filename, then narrow the results with your own tags.
Exact names rank first, followed by prefixes and other matches. Russian/English keyboard
layout mistakes work in both directions: `пщщпду` also finds `google`. Within each match
class, the literal spelling takes priority over the corrected layout.

| Query | Result |
| --- | --- |
| `code` | Apps with “code” in their name or filename |
| `#dev` | Apps tagged `dev` |
| `#work #chat` | Apps with both tags |
| `#work -#games` | Apps tagged `work`, excluding those tagged `games` |
| `#dev code` | Apps tagged `dev` that also match “code” |

Press <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>Space</kbd> for quick search, or
<kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>Shift</kbd> + <kbd>Space</kbd> for the full library.
<kbd>Ctrl</kbd> + <kbd>K</kbd> focuses search inside the library. These shortcuts can be changed
in settings. Upgrading moves the former default full-library shortcut to Ctrl+Alt+Shift+Space;
other custom bindings are preserved. On Wayland, assign `XLibrary --overlay` or `XLibrary`
in your desktop's shortcut settings; these commands also address an already running instance.

Quick search opens with a 200 ms reveal and shows up to five matching applications.
Use ↑/↓ and Enter to launch. Tab or Shift+F10 opens the selected application's actions;
↑/↓ and Enter select and run an action, Escape returns to search. Actions include browser profiles,
new instances, window switching, folder/path operations, favorites, editors, hotkeys and confirmed removal,
where applicable. Windows executable entries also offer an explicit administrator launch.
Use Escape or the global shortcut again to dismiss search, and the
small button on the right to open the full library with the same query. System animation
preferences are respected. XLibrary must be running for its built-in global shortcuts.

Both search fields also support:

- Arithmetic such as `2+3*4`, `sqrt(81)`, `√(16)`, `2^8`, `2**8`, and `(12+6)/3`.
  Operators include `+ - * / % ^`; powers associate to the right. Enter copies the result.
  Division by zero, non-real results, and incomplete expressions do not produce a number.
- Absolute folder paths, `~/`, `%USERPROFILE%`, and `$HOME/`. Results contain only direct
  children, including hidden entries, and can be scrolled. A partial final filename filters
  that folder. Enter on a folder browses it; Enter on a file opens it with its system handler.
  Registered applications use their library launch configuration. Directory reads happen in
  the background; slow disks or unavailable network shares can delay their results.

For the selected application:

| Shortcut | Action |
| --- | --- |
| <kbd>Enter</kbd> | Launch or switch to the running application, according to the launch settings |
| <kbd>Ctrl</kbd> + <kbd>Enter</kbd> | Launch a new instance |
| <kbd>Alt</kbd> + <kbd>Enter</kbd> | Open the application folder |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>C</kbd> | Copy the application path |
| <kbd>Delete</kbd> | Remove the entry from the library after confirmation; application files are kept |

Change or clear these bindings in Settings → Keyboard shortcuts. Switch between the **Global shortcuts**, **Navigation**, and **Application actions** tabs at the top. Both main and numeric keypad Enter are supported. Text editing keys keep their usual behavior in text fields. Use ↑/↓ in search to select an application; Escape clears the search and cannot be reassigned. Duplicate shortcuts are rejected when saving settings, and the tab containing the conflicting field opens automatically.

Application and scenario editors also accept a global hotkey. The **Applications** and **Scenarios**
keyboard settings tabs list these assignments. Conflicts and OS registration failures preserve the
previous binding. Deleting a target removes its binding; full profile backups retain assignments.
On Wayland, use compositor shortcuts for `XLibrary --launch-id ID` or `XLibrary --scenario-id ID`;
the editor shows the persistent ID command. XLibrary does not intercept unsupported Wayland keys.

Quick search uses normal window-manager activation on Linux. For Plasma/Wayland, assign
`XLibrary --overlay` directly in System Settings → Keyboard → Shortcuts so the compositor can
authorize activation. Existing-instance commands forward an available XDG activation token.
Focus and fullscreen stacking remain subject to compositor policy. A banner explains refused
Wayland activation; XLibrary does not repeatedly force focus or permanently pin the overlay above windows.

## Development

XLibrary is built with **C++23**, **Qt 6 Widgets**, and **SQLite**. It targets **Windows 10/11 and Linux**. The interface supports English and Russian. It follows the system's preferred supported language by default and falls back to English. Choose a language in Settings → General and restart XLibrary to apply it. The application translation is embedded in the executable.

Building from source requires a C++23 compiler, **CMake 3.21+**, and **Qt 6.5+** with the Widgets, Concurrent, and Network modules, and the LinguistTools build tools (plus DBus on Linux). CMake downloads SQLite during the initial configuration. See [Building on Windows and Linux](BUILDING.md) for CLion presets, GCC/Clang and MSVC/MinGW builds, portable packages, installers, and release verification.

Planned work includes further search improvements and extensibility. These are development directions rather than a release schedule.

## Contributing

Help shape a library you would want to use every day. Bug reports, interface feedback, documentation improvements, and code contributions are all welcome.

- **Report a bug:** [open an issue](https://github.com/Lappland-Julia/XLibrary/issues/new) with steps to reproduce, your OS and desktop session, and screenshots where useful.
- **Suggest an improvement:** [share your idea](https://github.com/Lappland-Julia/XLibrary/issues) and describe the workflow it would help with.
- **Contribute code:** submit a pull request with a focused change. Please discuss larger features in an issue first.

If you would like to follow the project, star the repository or watch it for updates.

## License

XLibrary is released under the [MIT License](LICENSE).
