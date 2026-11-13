# Interface design

Fora.Deck uses a quiet desktop interface with neutral graphite or soft light
surfaces, restrained coral accents and original application icons. This guide records the
design direction and its current implementation boundaries. It is not a claim
that every screen has passed visual acceptance on every desktop environment.

## Layout and hierarchy

The full library places navigation and tags on the left, search above the main
list, and details for the selected application on the right. Related rows share
a surface; spacing and thin dividers separate groups. Primary launch actions use
coral and placement for emphasis. In the inspector, launch precedes tags and technical details;
browser launch-profile controls stay next to it. Removal is separated from the other actions.
Status color belongs to a small indicator or
message, with text describing the state.

When file search is enabled and selected, the main Files workspace replaces the application
list with a file-only result list that fills the available height.
The details pane keeps its width and shows “Details will appear here.” in file mode or when no application is selected,
so switching between applications and files does not move the search controls.
Application-specific sections, including tags, remain available separately.
Both search fields show the shared Files switch only while indexing mode is Basic or Full.
The switch selects applications or files exclusively; indexing mode belongs to Settings.

The compact launcher has a search field, a button to open the full library and
the results relevant to the query. Its height follows its contents. Application
search shows up to five application results; file/path and calculator results
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
| Shell and side panes | `#181A1D` | `#F5F6F8` |
| Main work surface and dialogs | `#202328` | `#FFFFFF` |
| Fields and secondary surfaces | `#2A2E34` | `#EDF0F4` |
| Neutral hover | `#2D323A` | `#E5EAF0` |
| Selection | `#392B29` | `#FBE8E2` |
| Decorative separators | `#343A43` | `#DEE3E9` |
| Field borders | `#778191` | `#7A8595` |
| Main text | `#F2F3F5` | `#20242B` |
| Secondary text | `#AEB5BF` | `#56606D` |
| Small accent text and focus | `#F16647` | `#B63F2B` |

Primary buttons use coral `#F16647` with dark `#181A1D` text and icons;
hover uses `#FF795C`, press uses `#E45E40`, and unavailable controls use neutral colors.
Selection is flat, with a separate two-pixel keyboard focus border. Borders reserve
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
