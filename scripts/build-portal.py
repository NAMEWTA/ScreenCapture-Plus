#!/usr/bin/env python3
"""Build the pinned libportal and its Qt 6 backend in an isolated workspace."""

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess


def run(*arguments, cwd=None):
    subprocess.run(arguments, cwd=cwd, check=True)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/portal-validation")
    options = parser.parse_args()
    specification = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]["libportal"]
    patches = []
    for item in specification["patches"]:
        patch = root / item["path"]
        if hashlib.sha256(patch.read_bytes()).hexdigest() != item["sha256"]:
            raise SystemExit(f"libportal patch differs from the reviewed dependency lock: {patch.name}")
        patches.append(patch)

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
    run("git", "config", "core.autocrlf", "false", cwd=source)
    run("git", "remote", "add", "origin", specification["repository"], cwd=source)
    run("git", "fetch", "--depth=1", "--no-tags", "origin", specification["commit"], cwd=source)
    run("git", "-c", "advice.detachedHead=false", "checkout", "--detach", "FETCH_HEAD", cwd=source)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if actual != specification["commit"]:
        raise SystemExit(f"Unexpected libportal revision: {actual}")
    for patch in patches:
        run("git", "apply", "--check", str(patch), cwd=source)
        run("git", "apply", str(patch), cwd=source)
    native_build = runpy.run_path(str(root / "build/package/source_receipts.py"))["NativeBuild"]
    receipt = native_build(workspace / "source-receipt", "linux", __file__, "libportal", source, specification, prefix)
    for tool in ("cc", "c++", "ar"):
        receipt.observe_tool(tool)
    configuration_index = receipt.run(["meson", "setup", str(build), str(source), "--prefix=" + str(prefix), "--libdir=lib",
        "-Dbackend-qt6=enabled", "-Dbackend-qt5=disabled", "-Dbackend-gtk3=disabled",
        "-Dbackend-gtk4=disabled", "-Dintrospection=false", "-Dvapi=false", "-Ddocs=false",
        "-Dtests=false", "-Dportal-tests=false"], cwd=root)
    receipt.run(["meson", "compile", "-C", str(build)], cwd=root)
    receipt.run(["meson", "install", "-C", str(build)], cwd=root)
    build_records = [*sorted((build / "meson-info").glob("*.json")), build / "meson-private/cmd_line.txt", build / "build.ninja"]
    build_records += [build / name for name in ("compile_commands.json", "config.h") if (build / name).is_file()]
    source_receipt = receipt.finish(configure_index=configuration_index, configuration_files=build_records)
    metadata = {
        "repository": specification["repository"], "commit": actual,
        "patches": specification["patches"],
        "qtVersion": json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]["qt"]["version"],
        "prefix": str(prefix),
        "sourceBuildReceipt": source_receipt,
    }
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"libportal prefix: {prefix}")
    print(f"Set PKG_CONFIG_PATH={prefix / 'lib/pkgconfig'} before configuring Portal validation.")


if __name__ == "__main__":
    main()
