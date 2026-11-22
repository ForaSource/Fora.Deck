# Native UI plugins

[Documentation index](README.md) · [Architecture](architecture.md) · [Profile backups](profile.md)

## Enabling modules

Place a compatible native module in `plugins/` beside the executable, then explicitly enable it in **Settings → Plugins**.
Enable/disable changes take effect after restarting Fora.Deck. Settings shows available modules, metadata, and compatibility or load errors.
Reading embedded metadata does not execute disabled modules.

Plugins run native code inside Fora.Deck with the application's access to the profile and system. There is no sandbox.
Enable modules from trusted sources and keep profile backups. A native crash, invalid callback, or undefined behavior can still terminate or corrupt the process.
Plugin binaries are not included in profile backups.

## Compatibility and interface contract

The contract is defined by [UiRegistry.h](../desktop/UiRegistry.h). Modules implement `UiPlugin::configure(UiRegistry &)`
to replace registered widget factories or append styles. Build against matching Fora.Deck headers, architecture, compiler, and runtime ABI.
Qt also applies its own platform/build checks.

Before instantiation, Fora.Deck requires Qt's embedded IID `org.xlibrary.desktop.UiPlugin/1.0`, a matching Qt major version,
and a required Qt minor version no newer than the running application. These checks cannot prove binary compatibility.
Existing IID 1.0 modules do not need custom metadata. Optional custom `MetaData` strings `name`, `version`, and `author`
(also accepted as `Name`, `Version`, and `Author`) appear in settings; absent names fall back to filenames.

`configure()` receives the live registry at a stable address so callbacks can retain its reference.
A C++ exception during configuration restores the registry's prior factories and styles and allows startup to continue.
This rollback cannot undo a module's other side effects or recover from a native crash.
Loaded libraries remain mapped while their callbacks may exist.

## Safe mode

Start `XLibrary --safe-mode` to use the built-in interface without loading user modules.
Open **Settings → Plugins**, disable the faulty module, save, and restart normally.
Safe mode lists saved enabled modules so they can be disabled without loading them.
