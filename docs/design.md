# Interface design

Fora.Deck uses a quiet desktop interface with neutral graphite or soft light
surfaces, restrained coral accents and original application icons. This guide records the
design direction and its current implementation boundaries. It is not a claim
that every screen has passed visual acceptance on every desktop environment.

## Layout and hierarchy

The full library places navigation and tags on the left, search above the main
list or compact cards, and details for the selected application on the right. Both views share the same
model, filtering, sorting and selection. Related rows share
a surface; spacing and thin dividers separate groups. Primary launch actions use
coral and placement for emphasis. In the inspector, launch precedes tags and technical details;
browser launch-profile controls stay next to it. Removal is separated from the other actions.
Status color belongs to a small indicator or
message, with text describing the state.

When file search is enabled and selected, the main Files workspace replaces the application
list with a file-only workspace that fills the available height. Files reuse the application row/card delegate,
header spacing, tag chips and selection treatment. A count, sorting and list/cards controls sit above the results;
keyboard hints and file actions sit below. The last column identifies the containing folder.
The details pane keeps its width when switching between applications and files, preserving the search layout.
Selected files show their type, size, modification date, tags, path and default opening application, with a small
application icon when available. The same inspector covers saved files; **Details** opens it in a dialog on narrow windows.
An empty selection shows a prompt.
Favorites and sidebar tags apply to the selected result type; running applications, scenarios and saved files
remain separate sections. An empty file query shows ranked indexed files. File metadata remains separate from
application metadata and from the saved-file collection.
Both search surfaces show the shared **Applications / Files** segments only while indexing mode is Basic or Full.
The segments select applications or files exclusively; indexing mode belongs to Settings.
Changing result type fades between snapshots with a small directional slide over 220 ms. Rapid reversals capture
the displayed frame; resizing cancels the transition. Disabled interface or system animations switch immediately.

Navigation groups library, favorites and running applications above scenarios and saved files;
tags have their own searchable group. Settings and help sit at the bottom.
The library header contains sorting, the list/cards choice and a menu for library operations.
Removal appears in the inspector, application menu and bulk footer instead of the permanent header.
The ordinary footer shows keyboard hints and details; selecting several applications replaces it with bulk actions.
Toolbars wrap controls when the available width or text scale no longer permits one row.
The central workspace and inspector have rounded corners without a surrounding decorative border.
Their transition surfaces use the content background so headers and footers do not form dark rectangular blocks.
On Windows 11, native title bars follow the selected theme in the main window and child dialogs.
Dialog captions match their content surface; the main caption matches the shell.
Native window buttons and window-management behavior are retained. Native frames are refreshed when
windows open, native handles change or the palette changes.

The compact launcher has a search field, a button to open the full library and
the results relevant to the query on an opaque theme-colored surface. Its height follows its contents and the
available screen space. Settings selects 3–10 visible results (default: 5); additional matching applications
remain accessible by scrolling or keyboard navigation. Compact rows omit paths and show right-aligned
Alt+1–Alt+9 / Alt+0 hints for the first ten actionable results. File/path and calculator results
use the shared search presentation. In application mode, an empty query can show favorite or previously
launched applications; file mode prompts for a filename. Opening the library carries the query into the full window.

Settings, editors, scenario views, menus and onboarding should use the same
typography, icons and control styles as the library. Their headings and spacing
provide hierarchy without introducing large decorative cards. Scenario state
belongs next to the step it describes; confirmations and detailed reports use
dialogs.

## Shared implementation

| Owner | Design responsibility |
| --- | --- |
| [`appearance`](../desktop/appearance.h) | Theme palettes, style sheet, icon painting, shared selection painting and scaled dimensions. |
| [`UiHelpers`](../desktop/UiHelpers.h) | Shared UI helpers, animated reveal and page transitions. |
| [`SelectionButton`](../desktop/SelectionButton.h) | Consistent selectable navigation/category controls. |
| [`LibrarySplitter`](../desktop/LibrarySplitter.h) | Library pane layout. |
| [`NotificationBanner`](../desktop/NotificationBanner.h) | In-window messages with optional action, dismissal and automatic countdown. |
| [`SearchOverlay`](../desktop/SearchOverlay.h) | Compact search surface, placement and reveal/height animation. |
| [`PlatformIntegration`](../desktop/PlatformIntegration.h) | Platform appearance, composition and system animation preferences. |

[`appearance.cpp`](../desktop/appearance.cpp) owns the shared palette and styles;
that code is the source of truth. The current values include:

| Role | Dark | Light |
| --- | --- | --- |
| Shell and navigation | `#191B20` | `#F4F5F7` |
| Main work surface and dialogs | `#22262D` | `#FFFFFF` |
| Inspector | `#242930` | `#F7F8FA` |
| Fields | `#2C323B` | `#E9EDF2` |
| Neutral hover | `#2B313A` | `#E7EBF0` |
| Selection | `#3B2B28` | `#FBE8E2` |
| Decorative separators | `#353D48` | `#DCE2E9` |
| Field borders | `#768190` | `#7A8595` |
| Main text | `#F2F4F7` | `#1E242C` |
| Secondary text | `#AEB7C4` | `#566170` |
| Small accent text and focus | `#F16647` | `#B63F2B` |

Primary buttons use coral `#F16647` with dark `#181A1D` text and icons;
hover uses `#FF795C`, press uses `#E45E40`, and unavailable controls use neutral colors.
Selection is flat, with a short accent marker and a separate two-pixel keyboard focus border. Borders reserve
space so receiving focus does not move the control. Green, amber and red describe
status alongside text, rather than identifying ordinary navigation sections.

The application loads local Inter Regular/Medium/SemiBold (400/500/600), including
Cyrillic, from embedded resources. If loading fails, Qt's sans-serif fallback is used.
The base text is 14 logical pixels, captions 12, page headings 24 and group headings 16.
Application rows retain their 52-pixel base height. Use `appearance::scaled` for dimensions
affected by the text-scale preference; larger fonts and stored pane widths must remain usable.
Preserve original application icon colors and use the existing 24×24 vector icon system,
with rounded 1.75-pixel strokes, for Fora.Deck controls.

The product mark is a geometric D with a displaced panel; the wordmark keeps `Fora` and
`Deck` in the same color and weight, with a coral dot. Ordinary titles and accessible
names use the exact text `Fora.Deck`. [Brand assets](../assets/README.md) document the
vector originals, raster exports and unchanged technical filenames. The public author
name is ForaSource; this does not rename the executable, profile, IPC or plugin identifiers.

## Interaction and feedback

- Keep keyboard focus distinguishable from hover and selection. A selected row
  should retain its appearance when it receives focus.
- Button hover changes the background without adding an outline. The selected search segment has a border;
  a separate focus outline appears when navigating buttons by keyboard, rather than after a mouse click.
- Preserve readable names and paths. Use wrapping or elision with access to the
  complete value; do not shrink individual long labels below the common scale.
- Keep loading, unavailable, empty and error states explicit. A filtered-out
  selection must not leave unrelated details visible.
- Use the common `NotificationBanner` for new in-window notifications. Its
  default duration is seven seconds, with a text action when needed, a close
  control and a time bar. Its surface is quiet, with an information icon, accent link
  and thin coral countdown. Longer intervals need a specific reason.
- Reserve dialogs for confirmation, input and details. Field errors should stay
  near the field and remain understandable without relying on color alone.
- Keep motion brief and tied to a state change. Both the application animation
  preference and supported system preferences must be respected.

The current overlay uses a 200 ms reveal and a separate 140 ms height transition.
These are implementation values, not the earlier mockup's timing targets. Text
and icons remain sharp while the surrounding surface changes.

## Material and responsive design goals

Glass is subordinate to readability. A smoky neutral tint, subtle edge and soft
shadow may give depth where the platform supports them. Text and icons must not
blur or refract. An opaque surface must remain a complete, usable presentation.
The result varies with the window system and compositor; the design does not
promise identical native blur on Windows, X11 and Wayland.

Use a compact spacing rhythm, generally based on four and eight logical pixels.
Additional window width should primarily benefit content rather than inflate
padding. At smaller widths or larger text scales, allow forms to wrap or scroll
and keep actions reachable. Do not scale an entire window as an image.

When changing a screen, inspect long names and tags, empty and populated states,
keyboard focus, light/dark themes, disabled animations and increased text/display
scales. Material changes also need light, dark and detailed backgrounds. These
are acceptance goals for the affected change, not a record of checks already
performed.

## Earlier concepts

The original visual specification contains precise mockup sizes, a more elaborate
Liquid Glass material, possible narrow-window layouts and additional component
states. Those ideas remain design references, not automatically implemented
features. In particular, dedicated glass-density controls, an accessibility
contrast mode and every proposed responsive breakpoint must not be advertised
without checking the corresponding implementation.

Current behavior is documented in the [user guide](usage.md), and ownership in the
[architecture guide](architecture.md).
