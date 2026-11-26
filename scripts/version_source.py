#!/usr/bin/env python3
"""Read the product version and check or regenerate compatibility build inputs.

Only build/version.json is edited when changing the product version. Python is
used by this maintenance command and the existing package builder; the product,
CMake configuration and retained MSBuild build do not require a Python runtime.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\Z")
SOURCE = Path("build/version.json")
TEMPLATE = Path("build/windows/Version.props.in")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate product version field: {key}")
        result[key] = value
    return result


def _read(path: Path, maximum: int) -> bytes:
    if not path.is_file() or path.is_symlink() or path.stat().st_size > maximum:
        raise ValueError(f"Missing, linked or oversized version input: {path}")
    return path.read_bytes()


def _inputs(root: Path):
    source = _read(root / SOURCE, 4096)
    value = json.loads(source.decode("utf-8"), object_pairs_hook=_unique_object)
    if (not isinstance(value, dict) or set(value) != {"schemaVersion", "version"}
            or type(value["schemaVersion"]) is not int or value["schemaVersion"] != 1
            or not isinstance(value["version"], str)):
        raise ValueError("build/version.json requires schemaVersion 1 and one version string")
    version = value["version"]
    match = VERSION.fullmatch(version)
    if not match or any(int(part) > 65535 for part in match.groups()):
        raise ValueError("build/version.json requires canonical MAJOR.MINOR.PATCH components from 0 to 65535")
    template = _read(root / TEMPLATE, 16 * 1024)
    substitutions = {
        "HUHU_PRODUCT_VERSION": version,
        "HUHU_VERSION_SOURCE_SHA256": hashlib.sha256(source).hexdigest(),
        "HUHU_VERSION_TEMPLATE_SHA256": hashlib.sha256(template).hexdigest(),
        **dict(zip(("HUHU_VERSION_MAJOR", "HUHU_VERSION_MINOR", "HUHU_VERSION_PATCH"), match.groups())),
    }
    props = template.decode("utf-8")
    for name, replacement in substitutions.items():
        props = props.replace("@" + name + "@", replacement)
    if re.search(r"@[A-Z_]+@", props):
        raise ValueError("Unknown version template substitution")
    return version, {Path("VERSION"): (version + "\n").encode("utf-8"),
                     Path("build/windows/Version.props"): props.encode("utf-8")}


def read_version(root: Path = ROOT) -> str:
    """Return the sole source value only after its generated inputs match."""
    root = Path(root)
    version, generated = _inputs(root)
    for relative, expected in generated.items():
        actual = _read(root / relative, 32 * 1024)
        if actual != expected:
            raise ValueError(f"Stale generated {relative}; run python scripts/version_source.py --write")
    return version


def write_generated(root: Path = ROOT) -> str:
    root = Path(root)
    version, generated = _inputs(root)
    for relative, contents in generated.items():
        target = root / relative
        if target.is_symlink():
            raise ValueError(f"Refusing to replace a linked generated version file: {relative}")
        if not target.exists() or target.read_bytes() != contents:
            target.write_bytes(contents)
    return version


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true", help="reject missing or stale generated files (default)")
    action.add_argument("--write", action="store_true", help="regenerate VERSION and build/windows/Version.props")
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        version = write_generated(args.root) if args.write else read_version(args.root)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"Version source error: {error}\n")
    print(version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
