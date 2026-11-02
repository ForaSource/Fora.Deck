#!/usr/bin/env python3
"""Validate release inputs and assemble an allowlisted, verified upload directory."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile


def read_version(path):
    version = path.read_text(encoding="utf-8").strip()
    number = r"(0|[1-9][0-9]*)"
    identifier = r"[0-9A-Za-z-]+"
    match = re.fullmatch(
        rf"{number}\.{number}\.{number}(?:-({identifier}(?:\.{identifier})*))?"
        rf"(?:\+{identifier}(?:\.{identifier})*)?", version, flags=re.ASCII)
    if not match:
        raise ValueError("VERSION must contain one valid SemVer version")
    if any(int(part) > 65535 for part in match.group(1, 2, 3)):
        raise ValueError("Windows version components must be <= 65535")
    prerelease = match.group(4)
    if prerelease and any(part.isdigit() and len(part) > 1 and part.startswith("0")
                          for part in prerelease.split(".")):
        raise ValueError("Numeric prerelease identifiers must not have leading zeroes")
    return version, bool(prerelease)


def check_info(info, version, commit, platform):
    expected = {"version": version, "source_commit": commit, "source_dirty": False,
                "configuration": "Release"}
    if platform == "windows":
        expected.update(architecture="x64", compiler="mingw", qt_version="6.8.3",
                        channel="pre-release" if read_version(Path("VERSION"))[1] else "stable")
    else:
        expected.update(architecture="x86_64", system="Linux", qt_version="6.8.2", format="all")
    for key, value in expected.items():
        if key not in info or type(info[key]) is not type(value) or info[key] != value:
            raise ValueError(f"{platform}: unexpected {key}: {info.get(key)!r}; expected {value!r}")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_debian_version():
    # Reuse the project's encoding, including mixed prerelease identifiers and build metadata.
    with tempfile.TemporaryDirectory(prefix="xlibrary-version-") as temporary:
        directory = Path(temporary)
        result = directory / "version.txt"
        script = directory / "version.cmake"
        source = Path("cmake/version.cmake").resolve().as_posix()
        script.write_text(f'include([==[{source}]==])\n'
                          f'file(WRITE [==[{result.as_posix()}]==] "${{XLIBRARY_DEBIAN_VERSION}}")\n',
                          encoding="utf-8")
        subprocess.run(["cmake", "-P", str(script)], check=True)
        return result.read_text(encoding="utf-8")


def check_sums(directory, names):
    entries = {}
    for line in (directory / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        if not match or match[2] in entries:
            raise ValueError(f"Invalid or duplicate checksum entry: {line!r}")
        entries[match[2]] = match[1]
    if set(entries) != set(names):
        raise ValueError(f"Unexpected package list in {directory}/SHA256SUMS.txt")
    for name, expected in entries.items():
        path = directory / name
        if not path.is_file() or path.is_symlink() or path.stat().st_size == 0 or digest(path) != expected:
            raise ValueError(f"Missing, empty or corrupt package: {path}")


def assemble(artifacts, output, version, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected a full source commit SHA")
    windows = artifacts / "windows"
    linux = artifacts / "linux"
    info = {platform: json.loads((directory / "build-info.json").read_text(encoding="utf-8-sig"))
            for platform, directory in (("windows", windows), ("linux", linux))}
    for platform, metadata in info.items():
        check_info(metadata, version, commit, platform)
    debian_version = read_debian_version()
    # The encoded Debian version comes from cmake/version.cmake, not a second SemVer mapping.
    if info["linux"]["debian_version"] != debian_version:
        raise ValueError("Linux metadata Debian version differs from VERSION/CMake")
    windows_names = [f"XLibrary-{version}-windows-x64-mingw-{suffix}"
                     for suffix in ("portable.zip", "setup.exe")]
    linux_names = [f"XLibrary-{version}-linux-x86_64.AppImage", f"xlibrary_{debian_version}_amd64.deb"]
    check_sums(windows, windows_names)
    check_sums(linux, linux_names)
    root = windows_names[0].removesuffix(".zip")
    with zipfile.ZipFile(windows / windows_names[0]) as archive:
        if any(name.lower().endswith((".pdb", ".ilk", ".obj")) for name in archive.namelist()):
            raise ValueError("Debug/intermediate files in portable ZIP")
        embedded = json.loads(archive.read(f"{root}/build-info.json").decode("utf-8-sig"))
        if embedded != info["windows"]:
            raise ValueError("Portable metadata differs from the verified installer metadata")
    deb = linux / linux_names[1]
    for field, expected in (("Package", "xlibrary"), ("Version", debian_version), ("Architecture", "amd64")):
        actual = subprocess.check_output(["dpkg-deb", "--field", str(deb), field], text=True).strip()
        if actual != expected:
            raise ValueError(f"DEB {field} mismatch: {actual!r}")
    output.mkdir(parents=True, exist_ok=False)
    for directory, names in ((windows, windows_names), (linux, linux_names)):
        for name in names:
            shutil.copyfile(directory / name, output / name)
    for platform, metadata in info.items():
        (output / f"build-info-{platform}.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    files = sorted(output.iterdir())
    (output / "SHA256SUMS.txt").write_text(
        "".join(f"{digest(path)}  {path.name}\n" for path in files), encoding="ascii")
    check_sums(output, [path.name for path in files])
    print(f"Verified four packages from {commit}; upload directory: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("version", "assemble"))
    parser.add_argument("--artifacts", type=Path, default=Path("artifacts"))
    parser.add_argument("--output", type=Path, default=Path("release-upload"))
    options = parser.parse_args()
    version, prerelease = read_version(Path("VERSION"))
    if options.command == "version":
        outputs = f"version={version}\ntag=v{version}\nprerelease={str(prerelease).lower()}\n"
        print(outputs, end="")
        if "GITHUB_OUTPUT" in os.environ:
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
                stream.write(outputs)
    else:
        assemble(options.artifacts, options.output, version, os.environ["SOURCE_SHA"])


if __name__ == "__main__":
    main()
