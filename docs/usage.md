# User guide

[Documentation index](README.md) · [Catalog management](catalog.md) · [Profiles and recovery](profile.md)

## First launch

Choose discovery, manual entry, or complete profile restoration on the welcome screen. A short tour introduces the interface.
Reopen the welcome screen in **Settings → General** and the tour in **Settings → Quick help**.

Choose the interface language in **Settings → General**, then restart. The system language is used by default when supported,
with English as the fallback. Appearance settings control the theme, text size, and interface animations.

## Application search

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

Tag conditions are exact and all must match. An unknown required tag gives no matches;
an unknown exclusion hides nothing. If an app is missing from results, clear the query and check the selected section and sidebar tag.

## Quick search and keyboard shortcuts

The compact launcher shows up to five matching applications. Use ↑/↓ and Enter to select and launch.
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
Built-in global shortcuts require XLibrary to be running; Wayland starts with unassigned global bindings.

Application and scenario editors also accept a global hotkey. Their assignments appear in the corresponding keyboard settings tabs.
The editor shows a persistent-ID command for use with a system shortcut: `XLibrary --launch-id ID` or `XLibrary --scenario-id ID`.
Deleting an application or scenario removes its assignment. Complete profile backups retain assignments.

## File search and My files

File search is **Off** by default. Enable it in **Settings → File search** to reveal the **Files** switch beside
the search field in the main window and quick search. The switch selects either applications (off) or files (on);
the two result types are never mixed. Both windows share the selection, starting with applications.
Selecting files opens the main **Files** workspace instead of the application list.
The details pane keeps its width and shows “Nothing to display here :(” in file mode or when no application is selected.
Favorites, running applications, tags, scenarios and My files remain available as separate sections.
Opening one of these sections returns search to applications. Disabling file search hides the switch and shows applications.
An empty query prompts for a filename. The file workspace uses the full available height.

Search supports filenames, layout correction, typo suggestions, and extension filters such as `.pdf` or `report.pdf`.
Filename masks use `*` for any number of characters and `?` for one: `*.pdf`, `report-??.txt`.
Other mask characters are literal. Matching files show their folder to distinguish identical names.
Saved files and frequently opened files receive priority within comparable search matches.

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
Indexing and querying use separate background workers: the rest of the application and searches remain available.
Index progress does not replace the visible results; discovered files appear on the next query.

Supported formats include documents, spreadsheets, images, design files, and media. Unsupported, hidden, temporary and executable
files remain excluded. Directory traversal does not follow symbolic links or Windows reparse points; unavailable locations and
virtual/network filesystems are skipped. Search is by filename; document contents are not indexed or uploaded.

Right-click a result, or use Shift+F10, to open it, open its folder, copy its path, or **Save to My files**.
The **My files** section retains saved paths after restart, independently of the index's folder scope.
Removing a saved entry keeps the file on disk. Unavailable saved files are marked accordingly.
Use **Settings → Library → Clear usage history** to clear application and file usage counts and recent launch history.

## Calculator and folder browsing

Enter arithmetic such as `2+3*4`, `sqrt(81)`, `√(16)`, `2^8`, `2**8`, or `(12+6)/3`.
Enter copies the result. Operators include `+ - * / % ^`; powers associate to the right.
Division by zero, non-real results, and incomplete expressions do not produce a number.

With the **Files** switch on, both search surfaces accept absolute folder paths, `~/`, `%USERPROFILE%`, and `$HOME/`.
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

## Linux setup

The DEB targets Debian 13 x86_64 and installs system dependencies. Install the downloaded package with `sudo apt install ./<package>.deb`,
replacing the placeholder with its filename. The AppImage includes Qt but still needs a compatible x86_64 Linux system;
the public baseline is Debian 13, Qt 6.8.2, and glibc 2.41. Mark it executable with `chmod +x <package>.AppImage`, then run it.
See [profile storage](profile.md#storage-modes) before moving or replacing an AppImage.

On Wayland, configure a binding in XLibrary to request permission through the GlobalShortcuts portal,
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
