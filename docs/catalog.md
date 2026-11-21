# Catalog management

[Documentation index](README.md) · [Search and shortcuts](usage.md) · [Complete backups](profile.md)

## Discovering and adding applications

Discovery finds Windows system apps, registry and Start menu entries, Linux desktop entries, and installed Steam games.
Use **Find installed applications** beside **Add** to start discovery.
To add your own application, use **Add** to choose or drop an application file or shortcut. You can also drop it onto the main window.
Review the suggested name, target and launch options, then confirm with **Add**.
The editor accepts EXE, LNK, BAT and CMD on Windows, and desktop entries, AppImages and supported executables on Linux.
Select several application files to review a batch import.
If your selection also contains documents, review the **My files** draft first. Adding or skipping the files continues
to the application review. Cancelling the file draft stops the entire selection.

Discovery and imports open a review window before changes. For discovery, review names and paths, then choose **Add selected**.
JSON/file previews also show sources, arguments, and working directories; choose **Import selected** to save those entries.
Cancelling imports nothing. Search and select/clear controls affect visible results, retaining hidden selections.
Rejected files have a reason.

At normal startup, Fora.Deck checks selected discovery sources in the background. The **New applications** window offers new configurations;
existing ones retain their names, tags, favorites, history, and launch options. Their discovered locations and sources can be refreshed.
Startup checks do not show manual scan progress or scan-result notifications; actual catalog changes produce a brief count notification.
The `--check-startup` maintenance command uses a temporary profile and does not scan the computer.

## Exclusions and missing applications

Discovery can remember unchecked configurations. Applications removed manually are also remembered and are not offered again,
including other discovered launch modes for the same target. Cancelling the review does not remember unchecked entries.
Use **Settings → Library → Reset discovery exclusions** to make excluded entries eligible again.

**Ask before removing uninstalled applications from the library** is enabled by default in **Settings → Library**.
Review the names and paths, remove checked entries, or choose **Keep all**. Disabling the option removes confirmed missing entries automatically.
Each removal candidate shows the specific missing file, or the missing Steam manifest and installation folder /
Linux desktop entry and executable. The reason comes from the same verified-drive checks used to select candidates.

Removal requires evidence from the same local volume where a target was previously available and a fresh check before deletion.
Unavailable or changed volumes, unobserved paths, access errors, broken links, and uncertain shell targets are kept for manual review.
Windows additionally excludes removable and network drives from automatic removal; Linux checks supported local filesystem types.
Steam removal requires both manifest and installation folder to be missing; a missing Linux desktop entry also needs its recorded executable to be missing.
Confirmed uninstall removal supports undo and does not prevent a later reinstall from being discovered.

**Automatically remove unavailable applications without icons** is a separate **Settings → Library** option, enabled by default.
During startup and manual scans it removes unavailable catalog entries whose icon is missing, including stale Windows app entries.
Running applications and entries marked **Drive unavailable** are kept. The removal supports undo and allows later rediscovery.
Availability refreshes only update statuses. For moved targets, use [path recovery](profile.md#moved-applications).

## Duplicates and scan reports

Open **Library actions → Library health** (also available in **Settings → Library**) for a current overview
of unavailable applications, entries on unavailable drives, pending removal candidates and launch failures
recorded during this session. A successful launch clears that application's failure. The dialog also shows
the last operation error in this session, per-source file index diagnostics and the last discovery report.
Use **Refresh availability**, **Locate application…**, **Recover moved folder…** or **Refresh file index**
from the overview. Automatic cleanup runs during application scans.

Open **Last scan details** in the library or **Settings → Library** to read or copy the most recent report.
It separates additions, duplicates, skipped entries, and errors.

Duplicate detection compares launch configurations, not just names. The same executable with different arguments or effective working directories
can have separate entries; manual addition rejects an existing configuration. Symbolic links share a canonical target while retaining invocation paths.
An omitted Windows working directory and the executable's folder are equivalent. A shortcut incorrectly using its EXE as its working directory
is normalized to that folder.

Discovery prefers a matching Start menu shortcut over an App Paths record, preserving shortcut arguments.
Equivalent same-name cards can be consolidated while retaining tags, aliases, favorites, launch history, sources, and scenario steps;
cards remain separate if combined metadata would exceed catalog limits. Discovery preserves user metadata and never launches imported apps.
Cancellation before transaction commit rolls back additions; cancellation is unavailable once commit starts.

## Steam games

Steam discovery reads local `libraryfolders.vdf`, `appmanifest_*.acf`, and cached `appinfo.vdf` metadata across local libraries.
Game cards can show executable paths and cached icons, but games launch through `steam://run/<appid>`.
Moving a game and rescanning preserves its card ID, tags, and favorites. Rescan to enrich older cards.

If executable metadata is missing or unsupported, the installation folder is shown without guessing between executable files.
Steam files are not changed; no Web API or login is required.

## JSON import and export

Use JSON to transfer application cards. Format 1 includes names, typed launch targets, arguments, working directories,
browser defaults, search aliases, favorites, and assigned tags. Executable targets use absolute paths; URI, AUMID, and Steam targets use validated identifiers.
Older catalogs remain supported. Linux executable paths do not need a filename extension.

Import validates the whole file before writing. Invalid input reports a record, field, and reason and leaves the catalog unchanged.
Limits are 32 MiB and 50,000 applications. Optional fields can be omitted; supplied fields must have their expected JSON types.

Conflicts are skipped by default. Explicit replacement changes the matching entry's name, aliases, tags, and favorite,
while retaining its ID, history, and scenario links. For repeated configurations within one file, the first selected entry wins with skip;
the last selected entry wins with replacement.

JSON does not transfer the complete profile. Use a [`.xlprofile` backup](profile.md) for settings, scenarios, global hotkeys,
unused tags, saved filter records, saved file paths, and usage history.

## Bulk editing and removal

Use Ctrl/Shift to select several library entries in the list or cards. With two or more selected entries,
the footer shows the count and actions for tags, removal and clearing the selection.
Open **Tags** from the footer or the application's context menu. Check tags to assign them, uncheck tags to remove them,
or enter new tags separated by commas. A dash marks a tag assigned to only some of the selected applications.
Leave the dash in place to keep those assignments. Choose **Save** to apply changes.
Use the **Favorites** submenu to add or remove all selected applications from favorites.

Removal confirms the entry count and deletes catalog entries only. **Undo removal** restores the last deletion in the current session,
including metadata and scenario steps. Undo is available from **Library actions** after the notification disappears,
and directly from the notification banner while it is visible.
It refuses to overwrite conflicting new entries or linked scenarios that have changed since removal.
