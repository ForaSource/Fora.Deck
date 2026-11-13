# Profiles and recovery

[Documentation index](README.md) · [Catalog transfer](catalog.md#json-import-and-export) · [Plugin safe mode](plugins.md#safe-mode)

## Storage modes

**Settings → Profile** shows the actual database and settings paths, schema/product versions, storage mode, and last error.
Use these displayed paths when locating your profile; operating-system configuration can change the defaults.

Fora.Deck retains XLibrary's technical application and organization names. The rebrand does not create
a new profile folder, rename `xlibrary.db`, or change the `.xlprofile` format or credential-store identity.
Do not rename profile directories just to match the new display name.

| Mode | Storage |
| --- | --- |
| Installed | The operating system's per-user data folders. The catalog is normally `%LOCALAPPDATA%\XLibrary\xlibrary.db` on Windows or `${XDG_DATA_HOME:-~/.local/share}/XLibrary/xlibrary.db` on Linux. |
| Portable folder | A `portable.flag` beside the executable selects its adjacent `data/` directory. Keep that directory during upgrades. |
| AppImage | A normally launched AppImage uses `<AppImage filename>.data` beside the image. Keep the corresponding directory when replacing or renaming the image. |

The file index is a separate, rebuildable `file-index.sqlite` cache. Saved file paths and usage history are durable profile data;
the index is not a copy of the documents themselves.

## Complete backups

Choose **Settings → Profile → Back up profile…** to create a `.xlprofile` file.
It includes application cards and launch configurations, aliases, all tags, scenarios, settings, hotkey assignments,
saved filter records, saved file paths, discovery sources, and launch/file usage history.
This transfers more than the [JSON catalog format](catalog.md#json-import-and-export).

Backups contain private paths and arguments. They retain saved paths rather than the documents at those paths;
copy documents separately when moving computers. Native plugin binaries and OS-stored GitHub tokens are not included.
The local token remains local.

## Restoring a profile

1. Choose **Settings → Profile → Restore profile…** and select the backup.
2. Review the preview. Fora.Deck checks the format, schema, date, contents, and file fingerprint before replacement.
3. Confirm restoration and restart Fora.Deck when prompted to apply all settings.

Restoration replaces the current profile. Fora.Deck preserves its previous database, journals, settings, and a complete backup when readable.
An interrupted replacement rolls back at the next startup. Keep your own separate backups as well.

Schema upgrades are sequential and transactional. Before migration, an existing database receives a timestamped
`.before-schema-v<target>.*.bak` snapshot. A database from an unsupported newer schema is refused without changing it.
The schema number is independent of the product version; see **Settings → Profile** for the current values.

## Moved applications

Use **Locate application…** in an unavailable application's details to select its replacement.
For a directory move, use **Settings → Profile → Recover moved folder…** to preview candidate replacements and apply selected valid entries together.
Card IDs, tags, favorites, history, arguments, and scenario references are retained.

## Diagnostics and reset

The profile page can export diagnostics for a bug report. The export omits catalog contents, arguments, settings values, tokens, and raw errors,
but includes local profile paths. Inspect the file before sharing it if those paths are private.

The confirmed preference reset in settings retains the catalog and saved file paths. Restart afterwards.
**Settings → Library → Clear usage history** separately clears application/file usage counts and recent launch history.

If a native interface module prevents normal startup, use [safe mode](plugins.md#safe-mode) to disable it.
