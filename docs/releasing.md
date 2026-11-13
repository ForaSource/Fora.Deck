# Packaging and releases

Run commands from the repository root after [configuring a Release build](building.md). Build directories hold CMake files and binaries; output directories hold transferable packages. The packagers accept absolute paths or paths relative to the repository. Quote paths with spaces. Output directories must not already exist; choose a new output path when rebuilding.

## Versions and metadata

[VERSION](../VERSION) is the single source of the full SemVer string: `major.minor.patch[-prerelease][+build]`. It is retained in the application, metadata and Windows/AppImage names. Windows numeric resources and Inno Setup use `major.minor.patch.0`, with each component at most 65535, plus a full product version string. Rebuilding after a version change reconfigures CMake.

DEB uses a SemVer-to-Debian conversion from [cmake/version.cmake](../cmake/version.cmake): prereleases sort before the final release and build metadata does not affect ordering. Use `debian_version` in `build-info.json` to find the exact DEB filename; it may differ from the SemVer string.

Public packages should come from the intended commit with a clean working tree and `source_dirty=false`. `build-info.json` records version, platform, architecture, Qt/compiler versions, configuration, source commit, dirty state and UTC packaging time. This records build inputs; it does not promise identical archive bytes between builds. Leave `XLIBRARY_NATIVE_CPU=OFF` for distributed binaries.

## Combined packaging from Windows

Use PowerShell 7, a configured Windows Release build, Inno Setup 6.3+, and Docker Desktop running Linux x86_64 containers:

```powershell
pwsh -File ./packaging/package_project.ps1
pwsh -File ./packaging/package_project.ps1 -WhatIf
```

The default Windows build directory is `cmake-build-release`; set `-WindowsBuildPath` to select another. `-Platform Windows` or `-Platform Linux` builds one platform. `-OutputRoot release/rebuild` chooses a different root without changing the version. `-ReleaseNotes path.md` includes supplied notes as `RELEASE_NOTES.md`.

The result is `release/v<VERSION>/`, with `windows-mingw/` (or `windows-msvc/`), `linux-x86_64/` and one root `SHA256SUMS.txt`. This manifest replaces platform manifests and covers every output file, including build metadata and supplied notes, using relative `/` paths. From the release directory on Linux, verify it with `sha256sum --check SHA256SUMS.txt`.

Linux packaging builds the `linux-packaging` target in [Dockerfile](../Dockerfile) as `xlibrary-linux-packaging:debian13`. The source mount `/src` is read-only, `/build` is fresh inside the container, and `/output` receives packages. Tools and Qt sources persist in volume `xlibrary-linux-packaging-cache-debian13`. Docker readiness/image preparation precedes Windows packaging. Successful containers are removed; failed containers remain with `/build` and `/tmp` for `docker logs`/`docker cp`. After diagnosis, remove only that named container with `docker container rm <name>`.

An explicitly configured Debian 13 WSL environment is an alternative:

```powershell
pwsh -File ./packaging/package_project.ps1 -Platform Linux `
  -LinuxEnvironment WSL -WslDistribution Debian
```

WSL requires a configured Release build with system Qt 6.8.2, `pkg-config` and libsecret development files. Only WSL mode accepts `-WslDistribution` and `-LinuxBuildPath` (default `cmake-build-linux-release`); the latter may be an absolute Linux path or a path relative to the repository. The script validates the selected distribution and configured Qt before Windows packaging. `-WhatIf` reports the intended output without checking toolchain readiness or creating files.

## Windows packages

The existing [Windows packager](../packaging/build_release.ps1) builds Release, installs into staging, adds compiler runtime/licenses, checks startup and writes a portable ZIP. Add `-Installer` for Inno Setup:

```powershell
./packaging/build_release.ps1 -BuildPath cmake-build-release
# Alternatively, create ZIP and installer in one invocation:
./packaging/build_release.ps1 -BuildPath cmake-build-release -Installer `
  -InnoCompiler 'C:/Program Files (x86)/Inno Setup 6/ISCC.exe' `
  -OutputPath release/windows-installer
```

Choose one command per output directory. The default is `release/v<VERSION>-<compiler>/`, containing `XLibrary-<VERSION>-windows-x64-<compiler>-portable.zip`, optional `*-setup.exe`, `build-info.json` and `SHA256SUMS.txt`. The release workflow builds Windows assets with **MinGW**, preserving the toolchain-specific installer selection used by existing clients. MSVC packaging remains supported locally.

Close Fora.Deck if it is running from the selected build directory, including its tray process, before packaging: Windows locks that executable. A copy running from a different directory does not block rebuilding. Configure/build/install output is saved to `<BuildPath>/packaging-release.log`; failed staging remains under `build/release-<id>`.

MSVC needs an x64 developer shell or `-MsvcRuntimePath <CRT directory>`; the default CRT path is `VCToolsRedistDir/x64/Microsoft.VC143.CRT`. MinGW runtime DLLs are located using the configured `g++.exe`; licenses are searched in its installation. Use `-CompilerLicensePath` for a nonstandard layout. Missing mandatory runtime/license files stop packaging.

`cmake --install <build> --config Release --prefix <staging>` alone deploys the executable, Qt and license; compiler runtime and distribution materials are added by the packager. For validation on Windows without development paths, use [package checks and manual scenarios](testing.md#windows-distribution-and-profile).

## Linux compatibility baseline

Public Linux packages target **Debian 13 x86_64**, its system **Qt 6.8.2** and **glibc 2.41**. DEB must use distribution-managed shared Qt, not a Qt SDK, testing/unstable or a newer distribution. Package ownership alone does not establish compatibility. Qt newer than 6.8.2 is rejected for DEB.

Do not lower generated DEB dependencies: `dpkg-shlibdeps` derives them from the actual linked symbols. A DEB requiring newer Qt must be rebuilt with the baseline system Qt in a clean directory. Changing Qt or the base distribution always requires a clean build directory. Windows CI's Qt 6.8.3 is not the DEB baseline.

AppImage includes Qt, its X11/Wayland plugins and the C++ runtime; DEB uses system dependencies and installs `/usr/bin/XLibrary`, a desktop entry and icon. Both inherit ABI requirements from the build system. AppImage does not replace glibc, graphics drivers or the window server. Compatibility with older distributions or other DEB-based systems needs separate verification. A baseline change must update packaging, CI and this guide together.

## Linux packages

For native Debian 13 packaging, install the [build dependencies](building.md#linux) plus Python **3.11+**, `curl`, `file`, `dpkg-dev` and `desktop-file-utils`. Enable matching `deb-src` repositories for exact system Qt source packages. The Docker target already supplies these requirements.

```sh
sudo apt-get install python3 curl file dpkg-dev desktop-file-utils
# For the standard Debian 13 deb822 sources file:
sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
sudo apt-get update
cmake --preset linux-release -DCMAKE_PREFIX_PATH=/usr
python3 packaging/build_release.py
```

The [Linux packager](../packaging/build_release.py) uses `cmake-build-linux-release` and writes `release/v<VERSION>-linux/` by default. Use `--build-path` and `--output-path` for custom directories, or `--format appimage` / `--format deb` for one format. It builds Release, verifies native x86_64 and portable CPU settings, validates the extracted DEB and AppImage startup, and writes packages, `build-info.json` and SHA256 sums. Staging uses Linux `/tmp` to preserve permissions and links, even with sources on a WSL Windows mount; failed staging remains for diagnosis.

The tool/source cache is `$XDG_CACHE_HOME/XLibrary/packaging`, falling back to `~/.cache/XLibrary/packaging`; `--cache-path` overrides it. Keep it on a Linux filesystem in WSL so tools can be marked executable.

Find the generated names in the output directory, then run or install the selected file:

```sh
./XLibrary-<VERSION>-linux-x86_64.AppImage
# If FUSE is unavailable:
./XLibrary-<VERSION>-linux-x86_64.AppImage --appimage-extract-and-run
sudo apt install ./xlibrary_<debian_version>_amd64.deb
XLibrary
sudo apt remove xlibrary
```

Replace placeholders with the actual filenames. AppImage's writable profile is `<AppImage filename>.data`; move it with the unchanged filename. DEB uses the normal XDG profile. See [profile handling](profile.md).

Before publication, install the DEB in a **clean Debian 13 without Qt SDK or build packages** and run `QT_QPA_PLATFORM=offscreen XLibrary --check-startup`. Checks inside the build container do not replace this requirement.

## Dependency materials

The application is MIT licensed; dependencies retain their own licenses. Do not remove distribution `licenses/`, `sources/` or `THIRD_PARTY_NOTICES.txt` from bundled packages. Qt stays dynamically replaceable. Check applicable redistribution terms, including the compiler runtime and QtKeychain notices.

Inter Regular/Medium/SemiBold are embedded font resources under SIL OFL 1.1. CMake installs the
original [font license](../assets/fonts/OFL.txt) as `licenses/inter/OFL.txt` on Windows and
`share/licenses/XLibrary/inter/OFL.txt` on Linux. The Debian copyright file also includes its full
text. Keep these notices in ZIP, installer, AppImage and DEB distributions; see the
[font provenance](../assets/fonts/README.md). Product and author labels are Fora.Deck and ForaSource;
technical package filenames, the `XLibrary` CMake target and installer AppId retain their existing values.

[qt_sources.json](../packaging/qt_sources.json) pins hashes for supported official Qt source archives; unknown versions/modules require verified manifest entries matching the binaries. Windows accepts `-QtSourceManifest` and `-QtSourceCache` (default `build/windows-packaging-cache`), including through the combined packager. Cached archives are verified again. Windows source downloads use `curl.exe`, at most three attempts of 300 seconds; license extraction uses the configured `cmake -E tar`, with a 120-second limit per archive and diagnostics in the packaging log. SBOM files are included when the Qt installation supplies them.

Linux system Qt sources, including distribution patches, come from matching `apt-get source --download-only` packages; official SDK Qt uses the source manifest. AppImage tools are pinned and hashed in [appimage_tools.json](../packaging/appimage_tools.json). Custom Qt builds need corresponding sources and a description of changes.

## GitHub release Draft

[Prepare Release Draft](../.github/workflows/release.yml) runs manually from **main**. It reads `VERSION`, requires an unused `v<full-version>` tag and builds both platforms at one immutable source SHA. A prerelease suffix sets GitHub's prerelease flag; build metadata alone does not.

Windows uses MinGW 13.1, Qt 6.8.3 and Inno Setup 6.3.3. Linux uses Debian 13 system Qt. Platform jobs check packages, startup and installation/removal. Draft assembly checks names, hashes, versions, architecture, Release, source SHA and `source_dirty=false`, including ZIP/AppImage embedded metadata and the actual DEB version.

Only after both platforms pass does it create the tag and an **unpublished Draft**, containing seven assets:

- MinGW portable ZIP and setup EXE;
- Linux AppImage and DEB;
- `build-info-windows.json` and `build-info-linux.json`;
- one `SHA256SUMS.txt` covering those six files.

To prepare a release:

1. Merge the intended version/source changes through the normal review process and obtain successful CI checks.
2. In Actions → **Prepare Release Draft** → **Run workflow**, select **main**. Record the run and source SHA. The workflow does not change `VERSION`.
3. After all jobs pass, inspect the Draft's tag/SHA, prerelease flag, notes and seven assets. Download the assets and verify `sha256sum --check SHA256SUMS.txt` from their directory (or compare `Get-FileHash` on Windows).
4. Complete the [manual distribution checks](testing.md), including clean Windows/Debian installation, profile preservation and update from an older MinGW installation. Review notes for personal data before publication.
5. The owner publishes explicitly with **Publish release** after verification. A successful Draft job does not publish.

Release preparations share a concurrency group; the active run is not automatically cancelled. Artifacts are retained for 14 days. Ordinary CI does not create tags or releases.

### Recovery after a failure

A build failure before Draft creation leaves no tag from that run. A failure after tag creation can leave a tag without a Draft, or a partial Draft. Nothing is automatically deleted, overwritten or published. A full rerun rejects an existing tag; rerunning only Draft also rejects recreating it.

Inspect the actual GitHub state, tag SHA and assets. The owner can finish and verify the unpublished Draft from the original run's artifacts, or explicitly remove that unpublished Draft and its tag as separate operations before starting again. Do not publish partial output or delete published/other people's tags.

**Re-run jobs** uses the old run's source commit. After fixing a workflow/packager on main, use a new **Run workflow**. If main has moved, that new run builds the new SHA: verify the version and decide whether to finish the original Draft or prepare a new version.
