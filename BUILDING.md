# Building XLibrary

Start with the guide for your task:

- [Build from source](docs/building.md): dependencies, Windows/Linux commands, CMake presets and CLion.
- [Package and release](docs/releasing.md): ZIP/installer, AppImage/DEB, compatibility baseline and GitHub Draft recovery.
- [Testing](docs/testing.md): existing automated checks and manual verification.
- [CI](docs/ci.md): workflow coverage, diagnostics and the branch rules template.

XLibrary uses C++23 and Qt Widgets 6.5+. Public Linux packages target Debian 13 x86_64 with system Qt 6.8.2 and glibc 2.41; Windows CI and release packaging use Qt 6.8.3. The full application version comes from [VERSION](VERSION).
