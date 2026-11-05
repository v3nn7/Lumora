#!/usr/bin/env python3
"""Build the Linux AppImage of the Hub from its installed tree (cmake --install --component hub).

  appimage.py --install-dir dist/hub --version 0.2.0 --out out \\
              --appimagetool appimagetool-x86_64.AppImage --runtime runtime-x86_64

The AppDir is the installed tree under usr/ (bin/lumora-hub with RPATH $ORIGIN/../lib and a
qt.conf, so it needs no LD_LIBRARY_PATH), plus AppRun, the desktop entry and the icon. Editors
are not AppImages: the Hub installs them as ZIP packages, which allows file-level delta updates.
With --appdir-only it stops after assembling the AppDir (to check the layout without the tools).
"""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_ID = "lumora-hub"

DESKTOP = """[Desktop Entry]
Type=Application
Name=Lumora Hub
GenericName=Game Engine Launcher
Comment=Install Lumora editor versions and open your projects
Exec=lumora-hub
Icon=lumora-hub
Terminal=false
Categories=Graphics;3DGraphics;
Keywords=game;engine;3d;vulkan;editor;
StartupWMClass=lumora-hub
X-AppImage-Version={version}
"""

APPRUN = """#!/bin/sh
# Lumora Hub AppImage entry point. The binary finds its libraries and Qt plugins relative to
# itself (RPATH and qt.conf), so nothing else needs to be set up here.
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/lumora-hub" "$@"
"""


def assemble(install_dir: Path, appdir: Path, version: str) -> None:
    if not (install_dir / "bin" / "lumora-hub").is_file():
        sys.exit(f"error: {install_dir} has no bin/lumora-hub (run cmake --install --component hub first)")
    if appdir.exists():
        shutil.rmtree(appdir)
    shutil.copytree(install_dir, appdir / "usr", symlinks=True)

    apprun = appdir / "AppRun"
    apprun.write_text(APPRUN)
    apprun.chmod(apprun.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    desktop = DESKTOP.format(version=version)
    (appdir / f"{APP_ID}.desktop").write_text(desktop)
    applications = appdir / "usr" / "share" / "applications"
    applications.mkdir(parents=True)
    (applications / f"{APP_ID}.desktop").write_text(desktop)

    icon = ROOT / "packaging" / "lumora-hub.png"
    shutil.copyfile(icon, appdir / f"{APP_ID}.png")
    icons = appdir / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps"
    icons.mkdir(parents=True)
    shutil.copyfile(icon, icons / f"{APP_ID}.png")
    (appdir / ".DirIcon").symlink_to(f"{APP_ID}.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--appimagetool", type=Path, help="appimagetool executable (AppImage)")
    parser.add_argument("--runtime", type=Path, help="AppImage type 2 runtime (runtime-x86_64)")
    parser.add_argument("--appdir-only", action="store_true")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    appdir = args.out / "LumoraHub.AppDir"
    assemble(args.install_dir.resolve(), appdir, args.version)
    if args.appdir_only:
        print(f"AppDir assembled at {appdir}")
        return
    if not args.appimagetool or not args.runtime:
        sys.exit("error: --appimagetool and --runtime are required (or pass --appdir-only)")

    target = args.out / f"lumora-hub-{args.version}-linux-x64.AppImage"
    env = dict(os.environ, ARCH="x86_64", VERSION=args.version,
               APPIMAGE_EXTRACT_AND_RUN="1")  # CI runners have no FUSE
    subprocess.run([str(args.appimagetool.resolve()), "--no-appstream", "--runtime-file", str(args.runtime.resolve()),
                    str(appdir), str(target)], check=True, env=env)
    shutil.rmtree(appdir)
    print(f"{target.name}: {target.stat().st_size / 1048576:.1f} MiB")


if __name__ == "__main__":
    main()
