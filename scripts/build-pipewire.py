#!/usr/bin/env python3
"""Build the locked PipeWire client, SPA plugins and modules in a private prefix."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(*arguments, cwd=None, env=None):
    subprocess.run(arguments, cwd=cwd, env=env, check=True)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/pipewire-validation")
    parser.add_argument("--jobs", type=int, default=2)
    options = parser.parse_args()
    if options.jobs < 1:
        parser.error("--jobs must be positive")
    lock = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))
    specification = lock["qtMigration"]["pipewire"]
    patches = []
    for item in specification["patches"]:
        patch = root / item["path"]
        if hashlib.sha256(patch.read_bytes()).hexdigest() != item["sha256"]:
            raise SystemExit(f"PipeWire patch differs from the reviewed dependency lock: {patch.name}")
        patches.append(patch)
    workspace = options.workspace.resolve()
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
        raise SystemExit(f"Unexpected PipeWire revision: {actual}")
    for patch in patches:
        run("git", "apply", "--check", str(patch), cwd=source)
        run("git", "apply", str(patch), cwd=source)
    configuration = specification["mesonOptions"]
    run("meson", "setup", str(build), str(source), "--prefix=" + str(prefix),
        "--libdir=lib", *configuration)
    run("meson", "compile", "-C", str(build), "-j", str(options.jobs))
    run("meson", "install", "-C", str(build))
    environment = os.environ.copy()
    environment["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig")
    actual_version = subprocess.check_output(
        ["pkg-config", "--modversion", "libpipewire-0.3"], env=environment, text=True).strip()
    if actual_version != specification["version"]:
        raise SystemExit(f"Unexpected built PipeWire version: {actual_version}")
    required = [
        "lib/libpipewire-0.3.so", "lib/spa-0.2/support/libspa-support.so",
        "lib/spa-0.2/videoconvert/libspa-videoconvert.so",
        "lib/pipewire-0.3/libpipewire-module-protocol-native.so",
        "lib/pipewire-0.3/libpipewire-module-client-node.so",
        "lib/pipewire-0.3/libpipewire-module-adapter.so", "share/pipewire/client.conf",
    ]
    for relative in required:
        if not (prefix / relative).is_file():
            raise SystemExit(f"Required client runtime file is missing: {relative}")
    metadata = {
        "repository": specification["repository"], "commit": actual,
        "version": actual_version, "patches": specification["patches"],
        "mesonOptions": configuration, "prefix": str(prefix),
        "clientRuntimeChecked": required,
        "daemonDeployment": "Host daemon is not part of the application deployment",
    }
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"PipeWire client prefix: {prefix}")
    print(f"Set PKG_CONFIG_PATH={prefix / 'lib/pkgconfig'} for the capture target.")
    print("Keep the installed client modules, SPA plugins and client.conf with the client library.")


if __name__ == "__main__":
    main()
