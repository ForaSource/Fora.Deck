# Building from source

Run commands from the repository root. Use separate build directories for each Qt version, compiler and operating system. Build output and release packages belong outside the source directories.

## Requirements

| Component | Requirement |
| --- | --- |
| Language and build system | C++23 compiler, CMake 3.21+, Ninja for the supplied presets |
| GUI | Qt 6.5+ Widgets, Concurrent, Network and LinguistTools (`lrelease`); Qt Quick/QML is not required |
| Windows | Windows 10/11 x64, matching dynamic Qt and MSVC or MinGW toolchain |
| Linux | Linux with `/proc`, Qt DBus, `pkg-config`, libsecret development files; Xlib development files enable X11 hotkeys/window activation |
| Existing tests | Matching Qt Test; Linux also needs `dbus-run-session` |
| Packaging | See [release requirements](releasing.md) |

CTest is enabled by default. Use `-DBUILD_TESTING=OFF` if you only need the application. SQLite and QtKeychain 0.17.0 are downloaded with pinned hashes during configuration; QtKeychain is linked statically. Network access is required unless verified sources are provided through `FETCHCONTENT_SOURCE_DIR_SQLITE3` and `FETCHCONTENT_SOURCE_DIR_QTKEYCHAIN`.

The authoritative options and dependencies are in [CMakeLists.txt](../CMakeLists.txt), [CMakePresets.json](../CMakePresets.json) and [cmake/qtkeychain.cmake](../cmake/qtkeychain.cmake).

## Windows

Windows CI uses Qt **6.8.3**, MSVC 2022 x64 and MinGW **13.1.0**. Match `mingw_64` with the corresponding MinGW, or `msvc2022_64` with Visual Studio 2022 / Build Tools and Desktop development with C++. Do not mix compiler ABIs or architectures. Packaging and package checks require **PowerShell 7** (`pwsh`).

Example MinGW setup in PowerShell, with paths adjusted to your installation:

```powershell
$qt = 'C:/Qt/6.8.3/mingw_64'
$compiler_bin = 'C:/Qt/Tools/mingw1310_64/bin'
$env:PATH = "$compiler_bin;$qt/bin;$env:PATH"
cmake -S . -B cmake-build-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_PREFIX_PATH=$qt" `
  "-DCMAKE_C_COMPILER=$compiler_bin/gcc.exe" `
  "-DCMAKE_CXX_COMPILER=$compiler_bin/g++.exe"
cmake --build cmake-build-debug --parallel 4
./cmake-build-debug/XLibrary.exe
```

Configure a separate Release directory for distribution:

```powershell
cmake -S . -B cmake-build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qt" `
  "-DCMAKE_C_COMPILER=$compiler_bin/gcc.exe" `
  "-DCMAKE_CXX_COMPILER=$compiler_bin/g++.exe" `
  -DXLIBRARY_NATIVE_CPU=OFF
cmake --build cmake-build-release --parallel 4
```

For MSVC, open Developer PowerShell for VS 2022 x64, then start PowerShell 7 with the inherited environment:

```powershell
$qt = 'C:/Qt/6.8.3/msvc2022_64'
$env:PATH = "$qt/bin;$env:PATH"
cmake -S . -B build-msvc -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_PREFIX_PATH=$qt" -DXLIBRARY_NATIVE_CPU=OFF
cmake --build build-msvc --config Debug --parallel 4
cmake --build build-msvc --config Release --parallel 4
```

Development launches use Qt/compiler DLLs from the environment. Build does not deploy DLLs, run `windeployqt` or create an installer. Use the [packager](releasing.md#windows-packages) for a transferable application.

## Linux

A C++23-capable toolchain is required; GCC 13+ or Clang 17+ with a suitable standard library is a practical starting point. For Debian 13, install its system Qt **6.8.2**:

```sh
sudo apt-get update
sudo apt-get install build-essential clang cmake ninja-build qt6-base-dev \
  qt6-wayland qt6-tools-dev qt6-l10n-tools libx11-dev libsecret-1-dev \
  pkg-config dbus-x11
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 4
./cmake-build-linux-debug/XLibrary
```

Other distributions need equivalent development packages. An alternative Linux Qt SDK can be selected with `-DCMAKE_PREFIX_PATH=/path/to/Qt/gcc_64` for development; public DEB packages require the [Debian system Qt baseline](releasing.md#linux-compatibility-baseline). Windows Qt cannot be used for Linux builds.

The supplied Linux configure/build presets are `linux-debug`, `linux-release`, `linux-clang-debug` and `linux-clang-release`. Each has its own `cmake-build-<preset>` directory:

```sh
cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
cmake --install cmake-build-linux-release --prefix "$HOME/.local"
```

If you override the build path with `cmake --preset linux-release -B custom-build`, build with `cmake --build custom-build`; the build preset still refers to its original directory.

Installation adds the executable, desktop entry, icon and license. The installed application needs the selected Qt runtime and applicable Xlib libraries; a separate Qt SDK may require library search configuration. Development does not require installation. See [usage](usage.md) and [profiles](profile.md) for desktop limitations and data locations.

## CLion and presets

Open the root `CMakeLists.txt`, load CMake presets and enable the required profile in **Settings → Build, Execution, Deployment → CMake**. Use a matching Windows toolchain for `windows-debug`/`windows-release`, or Linux tools and Linux Qt for Linux profiles. Select target `XLibrary` to build/run/debug.

On Windows with WSL, select a WSL toolchain. If Linux presets are hidden by the host OS condition, create a WSL CMake profile with its own directory and no Windows Qt prefix. Keep `TMPDIR` on the Linux filesystem, normally `/tmp`; Windows-mounted directories may not support Unix-domain sockets. Do not carry Windows PATH or Qt plugin environment variables into Linux runs.

Store personal paths in the ignored `CMakeUserPresets.json`. For Windows development launches, add the selected Qt/compiler `bin` directories to the run configuration's PATH. Set `QT_PLUGIN_PATH` there only if needed; validate deployed packages independently of the IDE.

## Analysis and translations

Own application/core sources use `/W4` on MSVC or `-Wall -Wextra -Wpedantic` elsewhere. Optional clang-tidy needs CMake **3.27+**, Ninja/Makefiles and a compatible `clang-tidy`:

```sh
cmake --preset linux-clang-debug -DXLIBRARY_ENABLE_CLANG_TIDY=ON
cmake --build --preset linux-clang-debug --parallel 4
```

Analysis runs while compiling own sources and excludes dependencies/generated sources. Release builds use O2 and IPO/LTO when supported; leave `XLIBRARY_NATIVE_CPU=OFF` for distributed binaries.

English is the base language. `translations/xlibrary_ru.ts` and `translations/qt_widgets_ru.ts` are compiled by `lrelease` and embedded; runtime does not need separate XLibrary translation files. Use `QCoreApplication::translate` with context `XLibrary` for application messages and update the Russian catalog.

For existing automated commands, startup checks and manual verification, see [testing](testing.md).
