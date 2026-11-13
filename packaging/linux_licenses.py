"""Collect notices and matching Qt source for the libraries in an AppDir."""

import hashlib
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path, PurePosixPath


def _run(*args, cwd=None, optional=False):
    result = subprocess.run(args, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode and not optional:
        raise RuntimeError(f"{args[0]} failed: {result.stderr.strip()}")
    return result.stdout.strip() if result.returncode == 0 else ""


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _owner(path):
    paths = [path, path.resolve()]
    for candidate in list(paths):
        if str(candidate).startswith(("/lib/", "/lib64/")):
            paths.append(Path("/usr" + str(candidate)))
        elif str(candidate).startswith(("/usr/lib/", "/usr/lib64/")):
            paths.append(Path(str(candidate)[4:]))
    for candidate in paths:
        for line in _run("dpkg-query", "-S", str(candidate), optional=True).splitlines():
            package, separator, owned_path = line.partition(": /")
            if separator and "/" + owned_path == str(candidate):
                return package
    return None


def _package(package):
    fields = _run("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t"
                  "${source:Package}\t${source:Version}", package).split("\t")
    if len(fields) != 4 or not all(fields):
        raise RuntimeError(f"Cannot identify source package for {package}")
    return dict(zip(("package", "version", "source_package", "source_version"), fields))


def _native_source(info, cache):
    package, version = info["source_package"], info["source_version"]
    directory = cache / "native-sources" / re.sub(r"[^A-Za-z0-9.+_-]", "_", f"{package}_{version}")
    directory.mkdir(parents=True, exist_ok=True)
    try:
        _run("apt-get", "source", "--download-only", "--only-source",
             f"{package}={version}", cwd=directory)
    except RuntimeError as error:
        raise RuntimeError(f"Cannot obtain exact Qt source {package}={version}. Enable matching "
                           f"deb-src repositories and run apt-get update. {error}") from error
    descriptions = list(directory.glob("*.dsc"))
    if len(descriptions) != 1:
        raise RuntimeError(f"Expected one source description for {package}={version}")
    description = descriptions[0]
    text = description.read_text(encoding="utf-8")
    if not re.search(rf"^Source: {re.escape(package)}$", text, re.M) or not re.search(
            rf"^Version: {re.escape(version)}$", text, re.M):
        raise RuntimeError(f"Source description does not match {package}={version}")
    checksum_section = re.search(r"^Checksums-Sha256:\n((?:[ \t].*\n)+)", text, re.M)
    if not checksum_section:
        raise RuntimeError(f"Missing source checksums: {description.name}")
    files = [description]
    for checksum, size, name in re.findall(r"^ ([a-fA-F0-9]{64}) (\d+) (\S+)$",
                                          checksum_section[1], re.M):
        if Path(name).name != name:
            raise RuntimeError("Unsafe source archive filename")
        path = directory / name
        if not path.is_file() or path.stat().st_size != int(size) or _sha256(path) != checksum.lower():
            raise RuntimeError(f"Source checksum mismatch: {name}")
        files.append(path)
    if len(files) == 1:
        raise RuntimeError(f"No source archives listed for {package}")
    return files


def _qt_module(path):
    name = path.name
    if name.startswith("libQt6"):
        module = name[6:].split(".so", 1)[0]
        if module in {"Svg", "SvgWidgets"}:
            return "qtsvg"
        if "Wayland" in module:
            return "qtwayland"
        if module in {"Core", "Gui", "Widgets", "Network", "Concurrent", "DBus", "Xml",
                      "Sql", "OpenGL", "OpenGLWidgets", "PrintSupport", "Test", "XcbQpa",
                      "EglFSDeviceIntegration", "EglFsKmsSupport"}:
            return "qtbase"
        raise RuntimeError(f"Add Qt source/licensing support for {name}")
    if name in {"libqsvg.so", "libqsvgicon.so"}:
        return "qtsvg"
    if "wayland" in name or path.parent.name.startswith("wayland-"):
        return "qtwayland"
    if path.parent.name == "imageformats":
        return "qtbase" if name in {"libqgif.so", "libqico.so", "libqjpeg.so"} else "qtimageformats"
    if path.parent.name in {"platforms", "platformthemes", "platforminputcontexts", "tls",
                            "networkinformation", "xcbglintegrations", "egldeviceintegrations",
                            "sqldrivers", "styles", "generic"}:
        return "qtbase"
    raise RuntimeError(f"Add Qt plugin source/licensing support for {path.name}")


def _sdk_source(module, version, cache, manifest, license_directory):
    checksum = manifest.get(version, {}).get(module, "")
    if not re.fullmatch(r"[a-fA-F0-9]{64}", checksum):
        raise RuntimeError(f"Add verified SHA256 for {module} {version} to packaging/qt_sources.json")
    name = f"{module}-everywhere-src-{version}.tar.xz"
    url = f"https://download.qt.io/archive/qt/{version.rsplit('.', 1)[0]}/{version}/submodules/{name}"
    archive = cache / name
    if not archive.exists():
        temporary = cache / (name + ".part")
        _run("curl", "--fail", "--location", "--retry", "3", "--proto", "=https",
             "--tlsv1.2", "--output", str(temporary), url)
        if _sha256(temporary) != checksum.lower():
            raise RuntimeError(f"Qt source checksum mismatch: {name}")
        temporary.replace(archive)
    if _sha256(archive) != checksum.lower():
        raise RuntimeError(f"Qt source checksum mismatch: {name}")
    count = 0
    with tarfile.open(archive, "r:xz") as source:
        for member in source:
            parts = PurePosixPath(member.name).parts
            if len(parts) < 3 or parts[:2] != (name[:-7], "LICENSES"):
                continue
            if ".." in parts or not member.isfile():
                if member.issym() or member.islnk():
                    raise RuntimeError(f"License archive contains a link: {member.name}")
                continue
            destination = license_directory / module / Path(*parts[2:])
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as stream, destination.open("wb") as output:
                shutil.copyfileobj(stream, output)
            count += 1
    if not count:
        raise RuntimeError(f"Qt license texts are missing: {name}")
    return archive, {"module": module, "upstream": url, "sha256": checksum.lower()}


def collect_licenses(appdir: Path, config: dict, cache: Path, repository: Path) -> dict:
    """Fail packaging when a bundled library cannot be assigned its own notices."""
    destination = appdir / "usr/share/XLibrary"
    licenses, sources = destination / "licenses", destination / "sources"
    for directory in (licenses, sources, cache):
        directory.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(repository / "LICENSE", licenses / "XLibrary-MIT.txt")
    qt_libs = Path(_run(config["qmake"], "-query", "QT_INSTALL_LIBS"))
    qt_plugins = Path(_run(config["qmake"], "-query", "QT_INSTALL_PLUGINS"))
    distro_qt = _owner(Path(config["qt_core_library"])) is not None
    original_files = {}
    for root in (qt_libs, qt_plugins):
        for path in root.rglob("*.so*"):
            original_files.setdefault(path.name, path)
    for line in _run("ldconfig", "-p").splitlines():
        if " => " in line:
            name, path = line.strip().split(" => ", 1)
            original = Path(path)
            for basename in (name.split()[0], original.name, original.resolve().name):
                original_files.setdefault(basename, original)
    packages, libraries, qt_packages, modules = {}, [], {}, {"qtbase"}
    bundled = sorted(set((appdir / "usr/lib").rglob("*")) |
                     set((appdir / "usr/plugins").rglob("*")))
    for path in bundled:
        if not path.is_file():
            continue
        if not path.resolve().is_relative_to(appdir.resolve()):
            raise RuntimeError(f"Bundled library escapes AppDir: {path.name}")
        if path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        original = original_files.get(path.name)
        if original is None:
            raise RuntimeError(f"Cannot locate origin/license for bundled library {path.name}")
        is_qt = original.is_relative_to(qt_libs) or original.is_relative_to(qt_plugins)
        is_qt = is_qt and (path.name.startswith("libQt6") or original.is_relative_to(qt_plugins))
        record = {"file": path.relative_to(appdir).as_posix()}
        if is_qt and not distro_qt:
            module = _qt_module(original)
            modules.add(module)
            record.update(qt_module=module, qt_version=config["qt_version"])
        else:
            package = _owner(original)
            if not package:
                raise RuntimeError(f"No Debian copyright owner for bundled library {path.name}")
            if package not in packages:
                packages[package] = _package(package)
            info = packages[package]
            copyright_file = Path("/usr/share/doc") / package.split(":")[0] / "copyright"
            if not copyright_file.is_file():
                raise RuntimeError(f"Missing copyright for {package}")
            package_directory = licenses / "debian" / package
            package_directory.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(copyright_file, package_directory / "copyright")
            record.update(info)
            if is_qt:
                qt_packages[info["source_package"]] = info
        libraries.append(record)
    source_records = []
    if distro_qt:
        if not qt_packages:
            raise RuntimeError("No bundled Qt libraries were identified")
        for info in qt_packages.values():
            for archive in _native_source(info, cache):
                shutil.copyfile(archive, sources / archive.name)
                source_records.append({"file": archive.name, "sha256": _sha256(archive),
                                       "source_package": info["source_package"],
                                       "source_version": info["source_version"]})
    else:
        manifest = json.loads((repository / "packaging/qt_sources.json").read_text(encoding="utf-8"))
        for module in sorted(modules):
            archive, info = _sdk_source(module, config["qt_version"], cache, manifest, licenses)
            shutil.copyfile(archive, sources / archive.name)
            source_records.append(dict(info, file=archive.name))
    shutil.copytree("/usr/share/common-licenses", licenses / "common-licenses", dirs_exist_ok=True)
    notices = ("Fora.Deck by ForaSource is distributed under the MIT license; see licenses/XLibrary-MIT.txt.\n"
               "Inter is distributed under SIL OFL 1.1; see ../licenses/XLibrary/inter/OFL.txt.\n"
               "SQLite is statically linked and in the public domain: https://sqlite.org/copyright.html\n"
               "Qt is dynamically linked under LGPLv3. Exact corresponding source is in sources/; "
               "see THIRD_PARTY_MANIFEST.json for source versions and SHA256 values.\n"
               "Extract this AppImage with --appimage-extract and replace its Qt shared libraries "
               "and plugins with compatible modified builds, then run squashfs-root/AppRun.\n"
               "Reverse engineering to debug modifications of the LGPL libraries is permitted.\n"
               "Debian library copyright notices are in licenses/debian/; their referenced "
               "/usr/share/common-licenses texts are included in licenses/common-licenses/.\n"
               "GCC runtime libraries use the GCC Runtime Library Exception; see their copyright notices.\n")
    (destination / "THIRD_PARTY_NOTICES.txt").write_text(notices, encoding="utf-8")
    result = {"qt_version": config["qt_version"], "qt_source_kind": "debian" if distro_qt else "upstream",
              "sources": source_records, "bundled_libraries": libraries}
    (destination / "THIRD_PARTY_MANIFEST.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result
