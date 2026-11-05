#!/usr/bin/env python3
"""Pack an installed editor or Hub (cmake --install output) into a release ZIP.

The archive keeps Unix permissions (the executable bit) and symbolic links (Qt's soname links),
uses fixed timestamps so identical input gives an identical archive, and is accompanied by a
JSON fragment with its size and SHA-256 for manifest.py.

For delta updates it also writes the package's file list (<zip>.files.json: path, size, SHA-256
and mode of every file) and copies each file into <out>/blobs/<sha256>. Blobs are content
addressed, so a file that did not change between releases is uploaded and downloaded once.

  package.py --install-dir dist/editor --product editor --version 0.4.0 --platform linux-x64 --out out
"""
import argparse
import hashlib
import json
import os
import stat
import sys
import zipfile

FIXED_TIME = (2020, 1, 1, 0, 0, 0)


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def add_tree(archive: zipfile.ZipFile, root: str, entries: list, blobs: str) -> int:
    count = 0
    for folder, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        rel_folder = os.path.relpath(folder, root)
        for name in sorted(files) + sorted(d for d in dirs if os.path.islink(os.path.join(folder, d))):
            path = os.path.join(folder, name)
            arcname = name if rel_folder == "." else f"{rel_folder}/{name}".replace(os.sep, "/")
            info = zipfile.ZipInfo(arcname, FIXED_TIME)
            info.create_system = 3  # Unix: external_attr carries the mode
            if os.path.islink(path):
                target = os.readlink(path)
                if os.path.isabs(target) or ".." in target.split("/"):
                    sys.exit(f"error: link {arcname} -> {target} would point outside the package")
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(info, target)
                entries.append({"path": arcname, "link": target})
            else:
                mode = os.stat(path).st_mode
                info.external_attr = (stat.S_IFREG | (0o755 if mode & 0o111 else 0o644)) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(path, "rb") as f:
                    data = f.read()
                archive.writestr(info, data, compresslevel=9)
                digest = sha256_of(data)
                entries.append({"path": arcname, "size": len(data), "sha256": digest,
                                "mode": 0o755 if mode & 0o111 else 0o644})
                blob = os.path.join(blobs, digest)
                if not os.path.exists(blob):
                    with open(blob, "wb") as out:
                        out.write(data)
            count += 1
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True, help="e.g. linux-x64, windows-x64")
    parser.add_argument("--product", choices=["editor", "hub"], default="editor")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    executable = f"bin/lumora-{args.product}" + (".exe" if args.platform.startswith("windows") else "")
    if not os.path.isfile(os.path.join(args.install_dir, executable)):
        sys.exit(f"error: {args.install_dir} has no {executable} (run cmake --install first)")

    os.makedirs(args.out, exist_ok=True)
    name = f"lumora-{args.product}-{args.version}-{args.platform}.zip"
    zip_path = os.path.join(args.out, name)
    blobs = os.path.join(args.out, "blobs")
    os.makedirs(blobs, exist_ok=True)
    entries = []
    with zipfile.ZipFile(zip_path, "w") as archive:
        files = add_tree(archive, args.install_dir, entries, blobs)
    listing = json.dumps({"format": "lumora.files", "version": 1, "files": entries}, indent=1).encode()
    with open(zip_path + ".files.json", "wb") as f:
        f.write(listing)

    digest = hashlib.sha256()
    with open(zip_path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    fragment = {
        "platform": args.platform,
        "file": name,
        "size": os.path.getsize(zip_path),
        "sha256": digest.hexdigest(),
        "files": {"file": name + ".files.json", "size": len(listing), "sha256": sha256_of(listing)},
    }
    with open(zip_path + ".json", "w") as f:
        json.dump(fragment, f, indent=2)
    print(f"{name}: {files} files, {fragment['size'] / 1048576:.1f} MiB, sha256 {fragment['sha256']}")


if __name__ == "__main__":
    main()
