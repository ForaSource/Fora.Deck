# File Search Implementation Plan

> **For agentic workers:** Execute inline using superpowers:executing-plans. User authorized implementation without further approvals.

**Goal:** Indexed working-file search across all accessible volumes, independent of the application catalog.
**Architecture:** FileManager owns an independent SQLite cache and saved paths on its worker thread; cancellable scans and queries publish bounded results. DesktopService exposes it to both search surfaces and settings.
**Tech Stack:** C++23, Qt 6.5+, SQLite, Qt Widgets.
**Spec:** docs/superpowers/specs/2026-11-08-file-search-design.md

## Global Constraints
- Version 0.8.0; Windows and Linux; no new dependencies.
- Per AGENTS.md, do not add tests without explicit request. Run existing tests and manual checks instead.
- User authorized commit and push to v0.8.0-update, no force push or release publication.

## Review Focus
- Mounted volumes and nested roots: no duplicate scans, links or pseudo-filesystem traversal.
- Query cancellation and shutdown: stale results cannot replace new queries; no dangling worker callbacks.
- Extension/layout search: explicit extension restricts matches, alternate keyboard layout works.
- Empty search and saved files: no file pollution in applications; saved entries survive restart.
- Changing roots during scanning: old cache records are excluded, saved paths remain independent.

### Task 1: Backend
- [x] Create src/FileManager.h/.cpp and register in CMakeLists.txt; independent worker thread, scan cancellation, streaming SQLite index, filters, all-volume discovery, custom roots, persistence and ranked queries.
- [x] Verify compilation and existing SearchQuery checks; manually exercise indexing, extensions, exclusions and saved records.

### Task 2: Interface
- [x] Integrate FileManager with DesktopService; add shared animated switch in SearchTools.
- [x] Extend SearchResults with asynchronous file matches, paths and file actions. Add switches in SearchOverlay/LibraryViews with independent defaults.
- [x] Add file settings (roots, status, rescan) and separate saved-files view, translations and README.
- [x] Build and run existing CTest suite and offscreen startup/UI checks.

### Task 3: Review and delivery
- [x] Independent whole-change review; address material issues and repeat affected checks.
- [x] Verify diff, version, commit files explicitly, push HEAD:v0.8.0-update.

## Ledger
- Existing checkout is untouched; isolated worktree /workspace/file-search starts at 0e315a6.
- No new test files per repository instructions; use existing checks plus manual executable/UI exercises.

## Verification record
- Debug build: CMake/Ninja, GCC 14, Debian 13 Qt 6.8.2; successful.
- Existing CTest suite: 6/6 passed. The portal test needs local D-Bus sockets and passed outside the restricted command sandbox.
- `XLibrary --check-startup`: exit 0 with the offscreen Qt platform.
- Real Qt UI on Xvfb: `.txt` and `report.txt` match; `jnxtn` finds `Отчет.txt`; empty search shows no files; full/quick switch defaults verified visually.
- Fixture index contains report.txt and Отчет.txt, excluding node_modules, hidden directory, executable, MZ content disguised as PDF and Office lock filename.
- Shift+F10 and arrow navigation expose file save action; saved file appears in My files and survives a graceful exit and restart.
- Settings changed from one custom folder to all drives; index grew from 2 to 504 to 1071 entries while file results were switched off. Graceful exit during indexing returned exit 0.
- Independent review found six material issues; fixed all: draining saved mutations and syncing settings at shutdown, excluding nested remote mounts, progress refresh without cancelling queries/resetting selection, short SQLite write batches, keyboard file actions and preserving direct-path opening behavior.
- Saved paths are profile settings as well as an index-side table; the existing profile backup format includes them without a schema change. Reset preserves saved paths.
- Windows build/runtime and release packaging were not executed in this Linux environment. Network and removable-drive disconnection edge cases were reviewed statically, not reproduced.
- No new test files were created per AGENTS.md. Feature validation used the running application and a temporary profile.
