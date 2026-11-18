# Workspaces 2.0 design and verification

Workspaces save up to 32 ordered catalog applications, files, folders and HTTP(S)
URLs in the active profile. Scenarios retain their existing sequencing and launch
behavior. Both use the application launch pipeline and only one automation run
can be active at a time.

## Persistence and ownership

The catalog worker performs workspace CRUD. Schema 10 adds workspace, item and
placement tables through the existing migration, rollback and pre-migration
backup. Complete profile backups include these tables. A deleted application clears its link and leaves the item and display
name available for repair. A display name never becomes an executable target.
Updates preserve assigned item IDs and replace the ordered contents atomically.

## Capture and execution

Windows and X11 backends enumerate eligible foreign windows asynchronously.
Capture preselects a unique catalog match by verified executable path. If
several library entries match the same executable, the preview requires an
explicit choice. An unmatched window can open the application editor with
its verified executable path or offer an existing library entry through the
manual picker. Saving the detected file to the library selects it in the
preview. A different manual target has no captured placement. Saving the
workspace requires confirmation.

Preflight prepares a report before the run starts. Invalid items can be skipped
after reviewing the report. The coordinator runs one item at a time. It records
launch acceptance separately from verified window readiness and reports a
timeout as unverified. Multiple equally suitable windows cause a skip when
reuse is enabled. Opting out of reuse requests a fresh launch and permits
placement only on a newly identified window. Profiles and custom arguments
restrict reuse where window identity cannot prove the selected configuration.
Cancellation invalidates pending work using its run ID and leaves opened
applications running. Each restart begins a fresh run session. After a fresh
launch, placement requires both the executable identity and the process ID
returned by the launch pipeline.

## Platform capabilities

| Session | Capture | Reuse | Placement |
| --- | --- | --- | --- |
| Windows | Best effort | Verified identity | Best effort with read-back |
| X11 | EWMH-dependent | Verified identity | EWMH-dependent with read-back |
| Wayland | Unavailable | Unavailable | Launch only |

Placement is stored in fractions of a native monitor work area. If the saved
monitor is absent, the backend uses the primary work area. The destination is
clamped before submitting the request. A matching window is revalidated before
a native action. Minimized, protected or otherwise unverifiable windows are
handled conservatively.

## Verification

Use temporary profiles for migration, backup and application-opening tests.
Exercise the coordinator with a fake backend and controlled launch replies to
test ambiguity, cancellation, readiness and placement independently of window
manager policy. Offscreen startup checks cover initialization. The interactive
matrix and pilot are required for release readiness.


### Interactive matrix

| Session | Checks |
| --- | --- |
| Windows 11, one monitor | Create, edit, capture, run, reuse a unique window and cancel during readiness. |
| Windows 11, two monitors | Capture and restore at 100%, 125% and 150% scaling. Move the displays, disconnect the saved monitor and verify placement on the primary. |
| Windows 10 | Check installed and portable profiles, repeat run, minimized windows and a protected window. |
| XFCE on X11 | Capture, activation, move and resize, maximized restore, WM refusal and monitor fallback. |
| KDE on Wayland | Create and run an application, file, folder and URL. Check the capture and placement explanatory state. |

On each session, check EN and RU, light and dark palettes, keyboard ordering and selection, large text and narrow
windows. Use a disposable v9 profile with an app and scenario. Check the pre-migration snapshot, restart with the
migrated profile, export a complete backup, delete a catalog application, and restore the backup. The workspace
should show its missing application after deletion and regain the link and layout after restore.

### Five-person opt-in pilot

Use test packages from the `rebranding` commit under review and assign one tester to each matrix row.
Record the commit, OS/session, display count and scale for each participant, plus any unrepresented environment.
Use a separate test profile or export a full backup first. Ask each tester to share only observations and any
run diagnostics they choose to export. The export contains status codes and durations. Paths, window titles,
workspace names, and URL contents stay local.

1. Day 1. Ask each participant to create a workspace with three or four resources and run it. Record the time to
   the first useful result, whether help was required, and any error.
2. Days 2–3. Repeat with one app running already. Count expected and unexpected duplicates and placement attempts.
3. Days 4–5. Move windows, remove a test resource, cancel and rerun, and disconnect a display where applicable.
4. Days 6–7. Record voluntary use across the week. Ask which step was unclear, what opened unexpectedly, whether
   the layout helped, and what change would make workspaces worth using regularly.

| Release gate | Record |
| --- | --- |
| At least 4 of 5 finish onboarding unaided within 15 minutes | Times and assistance per tester |
| At least 3 of 5 use workspaces voluntarily on 3 different days | Days per tester |
| At least 80% of testable Windows/X11 placement attempts are acceptable | Successes, denominator and refusal reason |
| Zero data loss, unexpected termination, unsafe moves, critical crashes, corruption or unintended launches | Incident count, details and disposition |

Collect raw counts and qualitative observations. The sample size is five. Review the feedback and repeat the
affected checks before integrating the single v0.9.0 pull request. Pilot observations are pending.
