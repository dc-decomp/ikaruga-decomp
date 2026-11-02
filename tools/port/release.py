#!/usr/bin/env python3
"""Builds Ikaruga.app for a release and checks that it holds nothing of the machine that built it.

    tools/port/release.py [--out build-dist] [--macos 15.0]

The build is a clean one in its own directory, for macOS --macos and later. An API that is newer
than that version fails the build (-Werror=unguarded-availability-new). Homebrew's SDL3 runs only on
the macOS that Homebrew built it for, so the script builds SDL3 itself, from the release tarball
that Homebrew's formula uses, checked by its SHA-256.

The compiler writes the repository's path as `.` (-ffile-prefix-map), and strip removes the debug
map, which names each object file by its path. The global symbols stay, for the crash handler's
backtrace. SDL3 goes into the bundle (Contents/Frameworks), with the licenses of the port,
dream-recomp and SDL3 in Contents/Resources. The bundle gets an ad-hoc signature: a Developer ID
would name its owner.
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import os
import plistlib
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDL_VERSION = "3.4.18"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}.tar.gz"
# The checksum that Homebrew's sdl3 formula gives for this tarball.
SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
JOBS = str(os.cpu_count() or 4)


def run(*cmd: str | Path, capture: bool = False) -> str:
    done = subprocess.run(
        [str(c) for c in cmd],
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return done.stdout or ""


def quiet(*cmd: str) -> str:
    done = subprocess.run(cmd, text=True, capture_output=True)
    return done.stdout.strip() if done.returncode == 0 else ""


def options(macos: str, extra: str = "") -> list[str]:
    flags = f"-ffile-prefix-map={ROOT}=. {extra}".strip()
    return [
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={macos}",
        *(f"-DCMAKE_{lang}_FLAGS={flags}" for lang in ("C", "CXX", "OBJC", "OBJCXX")),
    ]


def build_sdl(out: Path, macos: str) -> Path:
    work = out / "sdl3"
    work.mkdir(parents=True, exist_ok=True)
    tarball = work / Path(SDL_URL).name
    if not tarball.exists():
        urllib.request.urlretrieve(SDL_URL, tarball)
    if hashlib.sha256(tarball.read_bytes()).hexdigest() != SDL_SHA256:
        sys.exit(
            f"release: {tarball.name} does not have the SHA-256 of Homebrew's formula"
        )
    source = work / f"SDL3-{SDL_VERSION}"
    if not source.exists():
        with tarfile.open(tarball) as t:
            t.extractall(work, filter="data")
    prefix = work / "install"
    # SDL checks at run time which of the newer APIs the system has, so its build keeps the warning.
    run(
        "cmake",
        "-S",
        source,
        "-B",
        work / "build",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DSDL_TEST_LIBRARY=OFF",
        *options(macos),
    )
    run("cmake", "--build", work / "build", "-j", JOBS)
    run("cmake", "--install", work / "build")
    return prefix


def build(out: Path, macos: str, sdl: Path) -> Path:
    run(
        "cmake",
        "-S",
        ROOT / "port",
        "-B",
        out,
        f"-DSDL3_DIR={sdl}/lib/cmake/SDL3",
        *options(macos, "-Werror=unguarded-availability-new"),
    )
    run("cmake", "--build", out, "--target", "bundle", "-j", JOBS)
    return out / "Ikaruga.app"


def bundle_sdl(app: Path, sdl: Path) -> None:
    program = app / "Contents/MacOS/ikaruga"
    links = run("otool", "-L", program, capture=True).splitlines()[1:]
    linked = next(line.split()[0] for line in links if "libSDL3" in line)
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)
    library = frameworks / Path(linked).name
    shutil.copyfile((sdl / "lib" / library.name).resolve(), library)
    library.chmod(0o755)
    run("install_name_tool", "-id", f"@rpath/{library.name}", library)
    run("install_name_tool", "-change", linked, f"@rpath/{library.name}", program)
    commands = run("otool", "-l", program, capture=True)
    for rpath in re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (\S+)", commands):
        run("install_name_tool", "-delete_rpath", rpath, program)
    run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", program)
    resources = app / "Contents/Resources"
    resources.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "LICENSE", resources / "LICENSE")
    shutil.copyfile(ROOT / "dream-recomp/LICENSE", resources / "LICENSE-dream-recomp")
    shutil.copyfile(
        sdl.parent / f"SDL3-{SDL_VERSION}/LICENSE.txt", resources / "LICENSE-SDL3.txt"
    )
    run("strip", "-S", "-x", program)
    run("strip", "-S", "-x", library)
    run("codesign", "--force", "--sign", "-", library)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--strict", "--deep", app)


def oldest_macos(binary: Path) -> str:
    commands = run("otool", "-l", binary, capture=True)
    return re.search(
        r"cmd LC_BUILD_VERSION\n(?:.*\n){2}\s+minos (\S+)", commands
    ).group(1)


def machine_terms() -> dict[str, str]:
    terms = {
        "user name": getpass.getuser(),
        "full name": quiet("id", "-F"),
        "home directory": str(Path.home()),
        "host name": socket.gethostname().split(".")[0],
        "computer name": quiet("scutil", "--get", "ComputerName"),
        "local host name": quiet("scutil", "--get", "LocalHostName"),
        "git name": quiet("git", "config", "--global", "user.name"),
        "git mail": quiet("git", "config", "--global", "user.email"),
        "repository path": str(ROOT),
    }
    platform = quiet("ioreg", "-rd1", "-c", "IOPlatformExpertDevice")
    for key, label in (
        ("IOPlatformSerialNumber", "serial number"),
        ("IOPlatformUUID", "hardware UUID"),
    ):
        if m := re.search(rf'"{key}" = "([^"]+)"', platform):
            terms[label] = m.group(1)
    return {label: value for label, value in terms.items() if len(value) >= 4}


PATHS = re.compile(
    rb"/Users/|/opt/homebrew|/usr/local/Cellar|/private/|/var/folders/|/Volumes/"
)


def scan(files: list[Path]) -> int:
    terms = machine_terms()
    found = 0
    for path in files:
        data = path.read_bytes()
        lower = data.lower()
        for label, value in terms.items():
            for encoding in ("utf-8", "utf-16-le"):
                if value.lower().encode(encoding) in lower:
                    print(f"  {path.name}: the {label} ({encoding})")
                    found += 1
        for m in sorted(set(PATHS.findall(data))):
            print(f"  {path.name}: the absolute path {m.decode()}")
            found += 1
    print(
        f"scan: {len(files)} files, {len(terms)} terms of this machine, {found} found"
    )
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build-dist")
    ap.add_argument(
        "--macos", default="15.0", help="the oldest macOS that the program runs on"
    )
    args = ap.parse_args()
    out = args.out.resolve()
    sdl = build_sdl(out, args.macos)
    app = build(out, args.macos, sdl)
    bundle_sdl(app, sdl)
    for binary in (
        app / "Contents/MacOS/ikaruga",
        app / "Contents/Frameworks/libSDL3.0.dylib",
    ):
        if (minos := oldest_macos(binary)) != args.macos:
            sys.exit(f"release: {binary.name} needs macOS {minos}, not {args.macos}")
    version = plistlib.loads((app / "Contents/Info.plist").read_bytes())[
        "CFBundleShortVersionString"
    ]
    archive = out / f"Ikaruga-{version}-macOS-arm64.zip"
    archive.unlink(missing_ok=True)
    # A zip keeps each time twice, in UTC and in the local time, which would give the time zone.
    os.environ["TZ"] = "UTC"
    run(
        "ditto",
        "-c",
        "-k",
        "--norsrc",
        "--noextattr",
        "--noqtn",
        "--noacl",
        "--keepParent",
        app,
        archive,
    )
    files = sorted(p for p in app.rglob("*") if p.is_file()) + [archive]
    if scan(files):
        return 1
    print(f"release: {archive}, for macOS {args.macos} and later on Apple silicon")
    return 0


if __name__ == "__main__":
    sys.exit(main())
