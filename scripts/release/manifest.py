#!/usr/bin/env python3
"""Maintain and sign the release feed (manifest.json) the Lumora Hub reads.

  manifest.py keygen
      Prints a new signing key (keep it secret: GitHub secret LUMORA_SIGNING_KEY) and its
      public key (build the Hub with -DLUMORA_RELEASE_PUBLIC_KEY=<public>).

  manifest.py update --manifest manifest.json --version 0.4.0 [--channel stable]
                     [--notes "..."] --package dist/*.zip.json [--url-prefix editor/0.4.0/]
      Adds or replaces a release (packages from package.py fragments) and keeps releases
      sorted newest first. A missing manifest file starts a new feed.

  manifest.py sign --manifest manifest.json          (key from $LUMORA_SIGNING_KEY)
      Writes manifest.json.sig: base64 Ed25519 signature of the file's exact bytes.

  manifest.py verify --manifest manifest.json --public-key <base64>
"""
import argparse
import base64
import datetime
import json
import os
import re
import sys

VERSION = re.compile(r"^\d{1,6}\.\d{1,6}\.\d{1,6}(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$")


def ed25519():
    try:
        from cryptography.hazmat.primitives.asymmetric import ed25519 as module
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        sys.exit("error: needs the 'cryptography' package (pip install cryptography)")
    return module, serialization


def version_key(version: str):
    core, _, pre = version.partition("-")
    numbers = tuple(int(x) for x in core.split("."))
    if not pre:
        return numbers, (1,)
    parts = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split("."))
    return numbers, (0, parts)


def private_key(module):
    seed = os.environ.get("LUMORA_SIGNING_KEY", "")
    try:
        raw = base64.b64decode(seed, validate=True)
    except ValueError:
        raw = b""
    if len(raw) != 32:
        sys.exit("error: LUMORA_SIGNING_KEY must hold a base64 32-byte Ed25519 seed (see 'keygen')")
    return module.Ed25519PrivateKey.from_private_bytes(raw)


def public_b64(key, serialization) -> str:
    raw = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return base64.b64encode(raw).decode()


def cmd_keygen(_args):
    module, serialization = ed25519()
    key = module.Ed25519PrivateKey.generate()
    seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    print("LUMORA_SIGNING_KEY (secret) =", base64.b64encode(seed).decode())
    print("LUMORA_RELEASE_PUBLIC_KEY   =", public_b64(key, serialization))


def cmd_update(args):
    if not VERSION.match(args.version):
        sys.exit(f"error: '{args.version}' is not a valid version (MAJOR.MINOR.PATCH[-pre])")
    if os.path.exists(args.manifest):
        with open(args.manifest) as f:
            feed = json.load(f)
        if feed.get("format") != "lumora.releases" or feed.get("version") != 1:
            sys.exit(f"error: {args.manifest} is not a Lumora release feed")
    else:
        feed = {"format": "lumora.releases", "version": 1, "releases": []}

    packages = {}
    for path in args.package:
        with open(path) as f:
            fragment = json.load(f)
        packages[fragment["platform"]] = {
            "url": args.url_prefix + fragment["file"],
            "size": fragment["size"],
            "sha256": fragment["sha256"],
        }
    if not packages:
        sys.exit("error: no packages given")
    release = {
        "version": args.version,
        "channel": args.channel,
        "date": datetime.date.today().isoformat(),
        "notes": args.notes,
        "packages": packages,
    }
    releases = [r for r in feed["releases"] if r.get("version") != args.version] + [release]
    releases.sort(key=lambda r: version_key(r["version"]), reverse=True)
    feed["releases"] = releases[: args.keep]
    with open(args.manifest, "w") as f:
        json.dump(feed, f, indent=2)
        f.write("\n")
    print(f"{args.manifest}: {len(feed['releases'])} releases, added {args.version} ({', '.join(sorted(packages))})")


def cmd_sign(args):
    module, serialization = ed25519()
    key = private_key(module)
    with open(args.manifest, "rb") as f:
        signature = key.sign(f.read())
    with open(args.manifest + ".sig", "w") as f:
        f.write(base64.b64encode(signature).decode() + "\n")
    print(f"signed {args.manifest} (public key {public_b64(key, serialization)})")


def cmd_verify(args):
    module, _ = ed25519()
    public = module.Ed25519PublicKey.from_public_bytes(base64.b64decode(args.public_key))
    with open(args.manifest, "rb") as f:
        data = f.read()
    with open(args.manifest + ".sig") as f:
        signature = base64.b64decode(f.read().strip())
    try:
        public.verify(signature, data)
    except Exception:  # cryptography raises InvalidSignature
        sys.exit("error: signature does NOT match")
    print("signature OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("keygen").set_defaults(run=cmd_keygen)
    update = sub.add_parser("update")
    update.add_argument("--manifest", required=True)
    update.add_argument("--version", required=True)
    update.add_argument("--channel", default="stable")
    update.add_argument("--notes", default="")
    update.add_argument("--package", nargs="+", default=[], help="package.py *.zip.json fragments")
    update.add_argument("--url-prefix", default="", help="prepended to package file names (relative to the feed)")
    update.add_argument("--keep", type=int, default=50, help="releases kept in the feed")
    update.set_defaults(run=cmd_update)
    sign = sub.add_parser("sign")
    sign.add_argument("--manifest", required=True)
    sign.set_defaults(run=cmd_sign)
    verify = sub.add_parser("verify")
    verify.add_argument("--manifest", required=True)
    verify.add_argument("--public-key", required=True)
    verify.set_defaults(run=cmd_verify)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
