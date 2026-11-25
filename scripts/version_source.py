#!/usr/bin/env python3
"""Read the sole product version for Python packaging and verification tools.

CMake consumes the same build/version.json directly. The retained Windows
build generates its resource properties under artifacts through build.ps1.
No second checked-in VERSION or generated properties file is required here.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\Z")
SOURCE = Path("build/version.json")


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


def read_version(root: Path = ROOT) -> str:
    root = Path(root)
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
    return version


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate the canonical product version (default)")
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        version = read_version(args.root)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"Version source error: {error}\n")
    print(version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
