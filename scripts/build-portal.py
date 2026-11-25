#!/usr/bin/env python3
"""Build the pinned libportal and its Qt 6 backend in an isolated workspace."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def run(*arguments, cwd=None):
    subprocess.run(arguments, cwd=cwd, check=True)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/portal-validation")
    options = parser.parse_args()
    specification = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]["libportal"]
    patch = root / specification["patch"]
    if hashlib.sha256(patch.read_bytes()).hexdigest() != specification["patchSha256"]:
        raise SystemExit("libportal patch differs from the reviewed dependency lock")

    workspace = options.workspace.resolve()
    # Never reset or clean an existing source checkout or a caller's files.
    # Each reproducible dependency build starts from an empty private workspace.
    workspace.mkdir(parents=True, exist_ok=True)
    if any(workspace.iterdir()):
        raise SystemExit(f"Use an empty build workspace; existing files retained: {workspace}")
    source = workspace / "source"
    build = workspace / "build"
    prefix = workspace / "install"
    run("git", "init", str(source))
    run("git", "remote", "add", "origin", specification["repository"], cwd=source)
    run("git", "fetch", "--depth=1", "--no-tags", "origin", specification["commit"], cwd=source)
    run("git", "-c", "advice.detachedHead=false", "checkout", "--detach", "FETCH_HEAD", cwd=source)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if actual != specification["commit"]:
        raise SystemExit(f"Unexpected libportal revision: {actual}")
    run("git", "apply", "--check", str(patch), cwd=source)
    run("git", "apply", str(patch), cwd=source)
    run("meson", "setup", str(build), str(source), "--prefix=" + str(prefix), "--libdir=lib",
        "-Dbackend-qt6=enabled", "-Dbackend-qt5=disabled", "-Dbackend-gtk3=disabled",
        "-Dbackend-gtk4=disabled", "-Dintrospection=false", "-Dvapi=false", "-Ddocs=false",
        "-Dtests=false", "-Dportal-tests=false")
    run("meson", "compile", "-C", str(build))
    run("meson", "install", "-C", str(build))
    metadata = {
        "repository": specification["repository"], "commit": actual,
        "patchSha256": specification["patchSha256"],
        "qtVersion": json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]["qt"]["version"],
        "prefix": str(prefix),
    }
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"libportal prefix: {prefix}")
    print(f"Set PKG_CONFIG_PATH={prefix / 'lib/pkgconfig'} before configuring Portal validation.")


if __name__ == "__main__":
    main()
