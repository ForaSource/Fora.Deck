# Workspaces 2.0 design and verification

Workspaces save up to 32 ordered catalog applications, files, folders and HTTP(S)
URLs in the active profile. Scenarios retain their existing sequencing and launch
behavior. Both use the application launch pipeline and only one automation run
can be active at a time.

## Persistence and ownership

The catalog worker performs workspace CRUD. Schema 10 adds workspace, item and
placement tables through the existing migration, rollback and pre-migration
backup. Complete profile backups include these tables. Catalog JSON is
unchanged. A deleted application clears its link and leaves the item and display
name available for repair. A display name never becomes an executable target.
Updates preserve assigned item IDs and replace the ordered contents atomically.

## Capture and execution

Windows and X11 backends enumerate eligible foreign windows asynchronously.
Capture only associates a window with a catalog target when executable identity
is strong and the configuration is unambiguous. Candidates without a match
are available for review. Saving requires explicit selection and confirmation.

Preflight prepares a report before the run starts. Invalid items can be skipped
after reviewing the report. The coordinator runs one item at a time. It records
launch acceptance separately from verified window readiness and reports a
timeout as unverified. Multiple equally suitable windows cause a skip when
reuse is enabled. Opting out of reuse requests a fresh launch and permits
placement only on a newly identified window. Profiles and custom arguments
restrict reuse where window identity cannot prove the selected configuration.
Cancellation invalidates pending work using its run ID and leaves opened
applications running. Runs are transient and do not resume on restart.

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
