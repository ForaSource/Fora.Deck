#!/usr/bin/env python3
"""Build Linux release packages without changing the installed system."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from linux_licenses import collect_licenses


def run(arguments, **kwargs):
    print("+", " ".join(map(str, arguments)), flush=True)
    return subprocess.run(list(map(str, arguments)), check=True, **kwargs)


def capture(arguments, **kwargs):
    return subprocess.check_output(list(map(str, arguments)), text=True, **kwargs).strip()


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_cache(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def download_tool(cache, name, entry):
    target = cache / name
    if not target.exists():
        temporary = target.with_suffix(target.suffix + ".download")
        run(["curl", "--fail", "--location", "--proto", "=https", "--retry", "3",
             "--connect-timeout", "20", "--max-time", "300", entry["url"], "--output", temporary])
        if sha256(temporary) != entry["sha256"]:
            raise RuntimeError(f"Checksum mismatch: {name}")
        temporary.replace(target)
    if sha256(target) != entry["sha256"]:
        raise RuntimeError(f"Cached tool checksum mismatch: {target}")
    target.chmod(0o755)
    return target


def startup_check(binary, stage):
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith(("QT_", "QML", "FORADECK_")) or key in ("LD_LIBRARY_PATH", "APPDIR", "APPIMAGE"):
            environment.pop(key, None)
    environment.update(QT_QPA_PLATFORM="offscreen", APPIMAGE_EXTRACT_AND_RUN="1", TMPDIR="/tmp")
    run([binary, "--check-startup"], cwd=stage, env=environment, timeout=60)


def build_appimage(repository, build, stage, cache, config, info):
    tools_manifest = json.loads((repository / "packaging/appimage_tools.json").read_text())
    tools = {name: download_tool(cache, name, entry) for name, entry in tools_manifest.items()}
    appdir = stage / "AppDir"
    run(["cmake", "--install", build, "--config", "Release", "--prefix", appdir / "usr"])
    qmake = config["qmake"]
    plugins = Path(capture([qmake, "-query", "QT_INSTALL_PLUGINS"]))
    environment = os.environ.copy()
    environment.update(APPIMAGE_EXTRACT_AND_RUN="1", QMAKE=qmake,
                       PATH=f"{cache}:/usr/local/bin:/usr/bin:/bin")
    extra_platforms = [p.name for p in (plugins / "platforms").glob("*.so")
                       if "wayland" in p.name or p.name == "libqoffscreen.so"]
    environment["EXTRA_PLATFORM_PLUGINS"] = ";".join(extra_platforms)
    cpp_libraries = [Path(capture([config["cxx_compiler"], f"-print-file-name={name}"]))
                     for name in ("libstdc++.so.6", "libgcc_s.so.1")]
    # QtKeychain loads libsecret dynamically, so linuxdeploy needs an explicit library.
    runtime_libraries = [*cpp_libraries, Path(config["secret_library"])]
    if any(not path.is_absolute() or not path.is_file() for path in runtime_libraries):
        raise RuntimeError("Cannot locate the C++ runtime libraries or libsecret")
    runtime_arguments = [value for library in runtime_libraries for value in ("--library", library)]
    run([tools["linuxdeploy-x86_64.AppImage"], "--appdir", appdir, "--plugin", "qt",
         "--custom-apprun", repository / "packaging/AppRun", *runtime_arguments], env=environment)
    # Qt Wayland loads shell/graphics integrations via dlopen; the platform plugin alone is insufficient.
    for name in ("wayland-shell-integration", "wayland-graphics-integration-client", "wayland-decoration-client"):
        if (plugins / name).is_dir():
            shutil.copytree(plugins / name, appdir / "usr/plugins" / name, dirs_exist_ok=True)
    run([tools["linuxdeploy-x86_64.AppImage"], "--appdir", appdir,
         "--deploy-deps-only", appdir / "usr/plugins"], env=environment)
    # A Qt plugin may wrap AppRun; preserve the environment before any changes.
    shutil.copyfile(repository / "packaging/AppRun", appdir / "AppRun")
    # The directory inside squashfs is read-only; the profile lives next to the AppImage.
    (appdir / "usr/bin/portable.flag").touch()
    (appdir / "AppRun").chmod(0o755)
    info["dependencies"] = collect_licenses(appdir, config, cache, repository)
    shutil.copyfile(repository / "packaging/appimage_runtime_license.txt",
                    appdir / "usr/share/Fora.Deck/licenses/AppImage-runtime-MIT.txt")
    (appdir / "usr/share/Fora.Deck/build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    image = stage / f"Fora.Deck-{config['version']}-linux-x86_64.AppImage"
    run([tools["appimagetool-x86_64.AppImage"], "--no-appstream", "--runtime-file",
         tools["runtime-x86_64"], "--mksquashfs-opt", "-processors", "--mksquashfs-opt", "2",
         appdir, image], env=environment)
    image.chmod(0o755)
    startup_check(image, stage)
    # The startup check must not create or modify the persistent portable profile.
    if Path(str(image) + ".data").exists():
        raise RuntimeError("Startup check unexpectedly wrote an AppImage portable profile")
    return image


def build_deb(build, stage):
    packages = stage / "deb"
    packages.mkdir()
    run(["cpack", "--config", build / "CPackConfig.cmake", "-G", "DEB", "-B", packages], cwd=build)
    results = list(packages.glob("*.deb"))
    if len(results) != 1:
        raise RuntimeError("CPack must produce exactly one DEB")
    package = results[0]
    run(["dpkg-deb", "--info", package])
    extracted = stage / "deb-extracted"
    run(["dpkg-deb", "--extract", package, extracted])
    if list(extracted.rglob("portable.flag")):
        raise RuntimeError("The installed DEB must not enable portable mode")
    run(["desktop-file-validate", extracted / "usr/share/applications/io.github.ForaSource.ForaDeck.desktop"])
    startup_check(extracted / "usr/bin/foradeck", stage)
    return package


def main():
    parser = argparse.ArgumentParser(description="Build Fora.Deck AppImage and/or DEB on Linux x86_64")
    parser.add_argument("--build-path", default="cmake-build-linux-release")
    parser.add_argument("--output-path")
    parser.add_argument("--format", choices=("all", "appimage", "deb"), default="all")
    cache_home = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
    parser.add_argument("--cache-path", default=str(cache_home / "Fora.Deck/packaging"))
    options = parser.parse_args()
    if sys.platform != "linux" or platform.machine() not in ("x86_64", "amd64"):
        parser.error("Run this packager on Linux x86_64 (including Ubuntu WSL)")
    repository = Path(__file__).resolve().parent.parent
    build = (repository / options.build_path).resolve()
    cache = (repository / options.cache_path).resolve()
    values = read_cache(build)
    if Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != repository:
        raise RuntimeError("Build directory must belong to this Fora.Deck checkout")
    if values.get("CMAKE_BUILD_TYPE") != "Release" and "Release" not in values.get("CMAKE_CONFIGURATION_TYPES", "").split(";"):
        raise RuntimeError("Configure a Release build directory first")
    run(["cmake", "-S", repository, "-B", build])
    config = json.loads((build / "linux-packaging-config.json").read_text())
    if config["system_name"] != "Linux" or config["pointer_size"] != "8" or config["system_processor"] not in ("x86_64", "amd64"):
        raise RuntimeError("Distribution requires a native Linux x86_64 target")
    if config["native_cpu"] != "OFF":
        raise RuntimeError("Distribution requires FORADECK_NATIVE_CPU=OFF")
    output = (repository / (options.output_path or f"release/v{config['version']}-linux")).resolve()
    if output.exists():
        raise RuntimeError(f"Output already exists; choose another --output-path: {output}")
    cache.mkdir(parents=True, exist_ok=True)
    run(["cmake", "--build", build, "--config", "Release", "--parallel", "4"])
    info = {"version": config["version"], "debian_version": config["debian_version"],
            "system": "Linux", "architecture": "x86_64", "configuration": "Release",
            "qt_version": config["qt_version"], "compiler": config["compiler_id"],
            "compiler_version": config["compiler_version"], "format": options.format,
            "source_commit": capture(["git", "-C", repository, "rev-parse", "HEAD"]),
            "source_dirty": bool(capture(["git", "-C", repository, "status", "--porcelain"])),
            "built_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
    # /tmp preserves Unix permissions and symlinks, including when running from WSL on NTFS.
    stage = Path(tempfile.mkdtemp(prefix="foradeck-package-", dir="/tmp"))
    try:
        packages = []
        if options.format in ("all", "deb"):
            packages.append(build_deb(build, stage))
        if options.format in ("all", "appimage"):
            packages.append(build_appimage(repository, build, stage, cache, config, info))
        output.mkdir(parents=True, exist_ok=False)
        for package in packages:
            shutil.copy2(package, output / package.name)
        (output / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
        (output / "SHA256SUMS.txt").write_text("".join(f"{sha256(output / p.name)}  {p.name}\n" for p in packages))
        print(f"Packages verified: {output}")
    except BaseException:
        print(f"Packaging failed; staging preserved for diagnostics: {stage}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(stage)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
