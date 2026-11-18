# User guide

[Documentation index](README.md) · [Catalog management](catalog.md) · [Profiles and recovery](profile.md)

## First launch

Choose discovery, manual entry, or complete profile restoration on the welcome screen. A short tour introduces the interface.
Reopen the welcome screen in **Settings → General** and the tour in **Settings → Quick help**.

Choose the interface language in **Settings → General**, then restart. The system language is used by default when supported,
with English as the fallback. Appearance settings control the theme, text size, and interface animations.

## Application search

The library offers **List** and **Cards** views of the same filtered and sorted applications.
Switching views keeps the selected applications. Use Ctrl/Shift for multiple selection;
the footer then offers bulk editing, removal and clearing the selection.
Right-click an application or use its actions menu for launch options and editing.
**Library actions** contains availability refresh, discovery, the last scan report and undo removal.
**Add application** opens the editor; its adjacent menu also offers discovery.
Settings and quick help are at the bottom of navigation.
On narrower windows the inspector is hidden; **Details** opens the selected application's dialog.

Search by application name, executable filename, or aliases added in **Edit application**.
Search handles case, repeated spaces, Unicode normalization, initials such as `vsc`, word prefixes,
limited typo correction, and English/Russian keyboard layout mistakes: `пщщпду` can find `google`.

Whole words rank before prefixes, substrings, abbreviations, and typo suggestions. Within matching groups,
favorites, usage count, and recent use help order results. Match category and position in the name determine relevance;
otherwise equivalent literal matches precede layout-corrected matches.
The selected sort mode breaks remaining ties in the library.

| Query | Result |
| --- | --- |
| `code` | Apps matching “code” in their name, filename, or aliases. |
| `#dev` | Apps tagged `dev`. |
| `#work #chat` | Apps with both tags. |
| `#work -#games` | Apps tagged `work`, excluding those tagged `games`. |
| `#dev code` | Apps tagged `dev` that also match “code”. |

In **My tags**, click a tag to filter by it. Click a selected tag again to remove that filter;
click another tag to switch to it. Hold **Ctrl** while clicking to add or remove individual tags.
When several sidebar tags are selected, applications must have all of them.

Tag conditions are exact and all must match. An unknown required tag gives no matches;
an unknown exclusion hides nothing. If an app is missing from results, clear the query and check the selected section and sidebar tag.

## Running applications

The **Running applications** section lists monitored catalog entries. Running entries show current CPU and RAM
usage in the list, cards and details. Values refresh about every 2.5 seconds. CPU is a percentage of total logical
processor capacity measured between samples; the first reading needs two samples. RAM is the sum of resident
memory (Windows working sets or Linux RSS) of matching processes; shared pages can be counted more than once.
An em dash means a measurement is unavailable. Monitoring covers processes matched by the existing application
identity, rather than every descendant or helper executable.

**Settings → General → Running applications** has independent **Show memory usage** and **Show CPU usage**
switches, both enabled by default. Save settings to apply them without restarting; disabling a metric also stops
collecting it. Process detection continues so the running section and window switching still work.

Use **Terminate** in details, quick search or the application's context menu to force terminate its currently matching
processes. A confirmation warns about unsaved work. All matching instances are affected, including those opened
outside Fora.Deck; entries sharing the same executable can refer to the same processes. The catalog entry is kept.
Access failures appear in the notification banner. Fora.Deck refuses to terminate itself through this action.
On Linux, safe termination requires kernel support for `pidfd_open` and `pidfd_send_signal` (Linux 5.3+).

## Quick search and keyboard shortcuts

Choose 3–10 visible results in **Settings → Appearance → Visible results in quick search** (default: 5).
The compact launcher keeps matching applications available beyond these visible rows; scroll with the mouse wheel
or use ↑/↓ to reach them, and Enter to launch. The panel height is limited by the available screen space.
Quick-search rows show the name and a shortcut on the right: Alt+1–Alt+9 opens the first nine results,
and Alt+0 opens the tenth. Paths are available in tooltips and result actions.
Tab or Shift+F10 opens the selected result's actions; use ↑/↓ and Enter inside that menu, and Escape to return.
Actions include browser profiles, new instances, window switching, folder/path operations, favorites, editing, and removal where applicable.
Windows executable entries also offer an explicit administrator launch.

Escape or the global shortcut dismisses quick search. The button on the right opens the full library with the same query.

| Default shortcut | Action |
| --- | --- |
| Ctrl+Alt+Space | Open quick search. |
| Ctrl+Alt+Shift+Space | Open the full library. |
| Ctrl+K | Focus search inside the library. |
| Enter | Launch the selected app or switch to it, according to its launch settings. |
| Ctrl+Enter | Launch a new instance. |
| Alt+Enter | Open the selected application's folder. |
| Ctrl+Shift+C | Copy the selected application's path. |
| Delete | Remove the selected catalog entry after confirmation; files remain on disk. |

Change or clear bindings in **Settings → Keyboard shortcuts**. Both Enter keys are supported; text editing keys keep their usual meaning
inside text fields. Escape clears library search and cannot be reassigned. Duplicate bindings are rejected when saving.
Built-in global shortcuts require Fora.Deck to be running; Wayland starts with unassigned global bindings.

Application and scenario editors also accept a global hotkey. Their assignments appear in the corresponding keyboard settings tabs.
The editor shows a persistent-ID command for use with a system shortcut: `XLibrary --launch-id ID` or `XLibrary --scenario-id ID`.
Deleting an application or scenario removes its assignment. Complete profile backups retain assignments.

## File search and My files

File search is **Off** by default. Enable it in **Settings → File search** to reveal the **Applications / Files** choice
in the main window and quick search. Select **Applications** or **Files**;
the two result types are never mixed. Both windows share the selection, starting with applications.
Selecting files opens the main **Files** workspace instead of the application list.
Selecting a file in the list, cards or **My files** shows its type, size, modification date, tags and path in the details pane.
**Opens with** identifies the system's default application, with its icon when available; an unresolved association is shown explicitly.
Use **Open**, **Open folder** or **Copy path** in the pane. On narrower windows, **Details** opens the same information in a dialog.
The pane keeps its width when switching between applications and files and prompts for a selection when empty.
Favorites and sidebar tags filter the selected result type. Running applications, scenarios and My files return
search to applications. Disabling file search hides this choice and shows applications.
An empty file query shows indexed files. The main workspace shows
up to 200 results with a count, sorting, list/cards views and file actions, using the same layout as applications.
Switching between applications and files uses a brief fade and slide, respecting disabled interface/system animations.
The file list, cards and quick search load icons in the background. Images show small previews when their format
is supported by Qt or the Windows thumbnail provider. Other files use their associated application's icon on Windows
(or its Shell file icon for packaged applications), and the system file-type icon on Linux. Unavailable icons/previews
use the generic file symbol.

The default **Recommended** file order groups extensions alphabetically (ignoring case), with the newest additions
first inside each group. This also applies to quick search and to the presentation of matching search results.
Choose **By extension** or **By date added** in the sort menu, or click the corresponding list header;
name, recent opens, favorites and tag sorting remain available. An explicit saved sort choice is retained.
The added date records first discovery in the index, or the first save to My files. Refreshing the index preserves it.
For an older index, the last recorded scan date is used because its original discovery date was not stored.
Unindexed files have no added date; rebuilding the index starts these dates again.

Click a file's star or use its context menu to change its favorite status. **Edit tags** accepts comma-separated tags;
`#tag` includes a tag and `-#tag` excludes it, including alongside filename masks and extension filters.
File favorites and tags are saved in the profile and included in complete profile backups.
**Save to My files** remains a separate action; removing a favorite does not remove a saved file.

Search supports filenames, layout correction, typo suggestions, and extension filters such as `.pdf` or `report.pdf`.
Filename masks use `*` for any number of characters and `?` for one: `*.pdf`, `report-??.txt`.
Other mask characters are literal. Matching files show their folder to distinguish identical names.
Relevance selects the bounded set of matches before the chosen display order is applied. Favorites, saved files
and opening history break ties in the default order; their dedicated sort modes can give them higher priority.

In **Settings → File search**, select a mode and choose **Save**:

- **Off** stops indexing and automatic refreshes.
- **Basic** indexes Desktop, Documents and Downloads recursively. Other accessible local drives are checked at their root
  and one directory level below it (`max-depth=1`).
- **Full (not recommended)** checks all accessible local drives recursively, including system, hidden and build folders.
  This can take a long time.
- **Additional recursive folders** extend the recursive scope in either enabled mode.
- **Excluded folders and files** are checked after selecting the scope. Exclusions always win, including inside additional folders;
  excluding a folder prunes its whole subtree. They also hide matching saved paths from indexed search without deleting saved entries.

The index stores paths and metadata locally and survives restart. A completed index refreshes automatically no sooner than
six hours later; restarting the application does not reset that age. Changing scope builds the newly selected index.
**Refresh file index** bypasses the interval manually. Apply settings before using it.
The diagnostics below the status show each source's last attempt, last complete scan and unavailable folders
(up to 200 paths displayed per source). These records survive restart. A partial or interrupted scan does not
advance the last complete scan time; a refresh warning means an empty result is not proof that a file is absent.
While file search is enabled, a background check runs about every 30 seconds when no scan is active.
When an unavailable source returns on the recorded local volume, only that source is scanned; newly mounted
local drives are scanned to the depth selected by Basic/Full mode. Other sources and their timestamps are retained.
This recovery can run before the regular six-hour refresh. It does not monitor every file change.
An unrelated volume at the same mount path remains unavailable until a manual refresh explicitly indexes it.
On Linux, source identity uses the filesystem UUID and subvolume where exposed by the system.
Without an exposed UUID, cached deletions are kept conservatively and source identity is limited to the current boot;
use a manual refresh after reboot if the source is reported unavailable.
Indexing and querying use separate background workers: the rest of the application and searches remain available.
Index progress does not replace the visible results; discovered files appear on the next query.

Supported formats include documents, spreadsheets, images, design files, and media. Unsupported, hidden, temporary and executable
files remain excluded. Directory traversal does not follow symbolic links or Windows reparse points; unavailable locations and
virtual/network filesystems are skipped. Search is by filename; document contents are not indexed or uploaded.

Right-click a result, or use Shift+F10, to open it, open its folder, copy its path, or **Save to My files**.
The **My files** section retains saved paths after restart, independently of the index's folder scope.
Removing a saved entry keeps the file on disk. Unavailable saved files are marked accordingly.
Indexed matches from unavailable sources are also retained and marked **Source or file unavailable**;
they cannot be opened until a subsequent query finds them available again. A scan only removes missing
cached files after completely reading their parent directory on the recorded local volume. Unreadable
or disconnected locations retain their cached matches. Changing indexing scope still removes ordinary
cached entries outside that scope; saved paths are retained.
Use **Settings → Library → Clear usage history** to clear application and file usage counts and recent launch history.

## Calculator and folder browsing

Enter arithmetic such as `2+3*4`, `sqrt(81)`, `√(16)`, `2^8`, `2**8`, or `(12+6)/3`.
Enter copies the result. Operators include `+ - * / % ^`; powers associate to the right.
Division by zero, non-real results, and incomplete expressions do not produce a number.

With **Files** selected, both search surfaces accept absolute folder paths, `~/`, `%USERPROFILE%`, and `$HOME/`.
Folder browsing requires an enabled file-search mode. Results show direct children, including hidden entries;
a partial final filename filters that folder. Enter on a folder browses it; Enter on a file opens its system handler.
Folder results include working files and folders; applications remain in application search.
Directory reads run in the background, but slow disks or network shares can delay results.

## Launch scenarios and browser profiles

Create a scenario in **Scenarios**, add applications in launch order, and set optional delays or launch profiles.
Supported browsers offer normal, private, and detected local profiles in their launch options and scenario steps.

Review missing targets and invalid options before running. Each scenario step shows waiting, delay, launching, success, error, or skipped.
Success means the launch command was accepted; it does not mean the application's window is ready.
Stopping skips remaining steps and leaves already opened applications running.

## Workspaces

Open **Workspaces** next to **Scenarios**. Choose **New workspace**, enter a name and add up to 32 library applications,
files, folders or HTTP(S) links. Drag items or use **Move up** and **Move down** to set launch order. Clear an item's
checkbox to keep it in the workspace and skip it during runs. A supported browser can use the application default,
the configured invocation, a normal or private window, or a specific detected profile.

On Windows or a compatible local X11 session, **Save current desktop** opens a preview. Select one window per library
application, continue to the editor and review the layout before saving. An unresolved window can be represented
manually by adding its intended library application. Window placement is expressed as percentages of each monitor's
usable area. When a saved monitor is disconnected, Fora.Deck tries the primary monitor.

Select a saved workspace and choose **Run**. Review any preflight warnings and confirm. **Last run** shows each
resource's outcome, including a missing target, an accepted open request, a reused window or verified positioning.
When multiple windows match the same launch configuration, Fora.Deck skips the item while reuse is enabled.
Clear the reuse option to request a fresh launch. Browsers and some application launchers can still forward the
request to a running process. **Cancel run** stops pending work and leaves opened applications running.

Wayland supports manual workspaces and launching resources. Windows and X11 support capture, reuse and placement
when the window owner and geometry can be verified. Browser URLs and profiles are explicit launch inputs.
The operating system and each application manage the contents of opened windows.

**Duplicate** opens an editable copy. **Delete** removes the workspace and its layout from the profile.
Complete backups include workspaces. **Export run diagnostics** writes the last run's status, counts and timing to a
JSON file you choose. See the [support and pilot matrix](workspaces.md).

## Linux setup

The DEB targets Debian 13 x86_64 and installs system dependencies. Install the downloaded package with `sudo apt install ./<package>.deb`,
replacing the placeholder with its filename. The AppImage includes Qt but still needs a compatible x86_64 Linux system;
the public baseline is Debian 13, Qt 6.8.2, and glibc 2.41. Mark it executable with `chmod +x <package>.AppImage`, then run it.
See [profile storage](profile.md#storage-modes) before moving or replacing an AppImage.

On Wayland, configure a binding in Fora.Deck to request permission through the GlobalShortcuts portal,
or add a system shortcut running `XLibrary --overlay`. Use the full executable or AppImage path when it is not on PATH.
`XLibrary` opens the full library; both commands also address an already running instance using the same profile.

| Desktop | Where to add a system shortcut |
| --- | --- |
| KDE Plasma | System Settings → Keyboard → Shortcuts → Add New → Command or Script. |
| GNOME | Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts. |
| Xfce/X11 | Settings → Keyboard → Application Shortcuts. |

Desktop labels can vary. Portal registration needs a compatible desktop backend and user approval; a refused binding can remain saved but inactive.
Clear or replace it to request another binding. A notification explains registration failures or lost sessions.
X11 uses native hotkeys. Wayland activation and fullscreen stacking follow compositor policy; if activation is refused, click the search field
or use a system shortcut. See the [verification guide](testing.md) for desktop checks that need a real interactive session.
