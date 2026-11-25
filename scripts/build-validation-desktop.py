#!/usr/bin/env python3
"""Build the locked CPU-capable Portal-wlr backend for owned CI desktops only.

This is a test desktop service, never an application-bundled Portal daemon. Its
PipeWire/Wayland dependencies are the runner's system libraries, independently
of the fixed client library used by huhucapture.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def run(*arguments: str, cwd: Path | None = None, env: dict[str, str]) -> None:
    subprocess.run(arguments, cwd=cwd, env=env, check=True)


def checkout(specification: dict, destination: Path, env: dict[str, str]) -> None:
    run("git", "init", str(destination), env=env)
    run("git", "remote", "add", "origin", specification["repository"], cwd=destination, env=env)
    run("git", "fetch", "--depth=1", "--no-tags", "origin", specification["commit"],
        cwd=destination, env=env)
    run("git", "-c", "advice.detachedHead=false", "checkout", "--detach", "FETCH_HEAD",
        cwd=destination, env=env)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=destination, env=env, text=True).strip()
    if actual != specification["commit"]:
        raise RuntimeError(f"Unexpected {destination.name} revision: {actual}")


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/validation-desktop")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    specification = json.loads((root / "build/dependencies.lock.json").read_text(
        encoding="utf-8"))["qtMigration"]["headlessPortalValidation"]
    workspace = args.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    if any(workspace.iterdir()):
        raise SystemExit(f"Use an empty validation workspace; existing files retained: {workspace}")

    # The application prefix may already be in the runner environment. A host
    # service must not accidentally link to its libraries or load its modules.
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("PIPEWIRE_", "SPA_", "WIREPLUMBER_", "WP_", "WLR_", "PKG_CONFIG_")) or key in (
            "LD_LIBRARY_PATH", "LD_PRELOAD", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH",
            "CPLUS_INCLUDE_PATH", "CMAKE_PREFIX_PATH", "CC", "CXX", "CFLAGS", "CPPFLAGS", "LDFLAGS"):
            env.pop(key, None)
    env["PATH"] = "/usr/bin:/bin"
    prefix = workspace / "install"
    protocols = specification["waylandProtocols"]
    protocol_source = workspace / "wayland-protocols-source"
    protocol_build = workspace / "wayland-protocols-build"
    checkout(protocols, protocol_source, env)
    run("meson", "setup", str(protocol_build), str(protocol_source), "--wrap-mode=nodownload",
        "--prefix=" + str(prefix), "-Dtests=false", env=env)
    run("meson", "install", "-C", str(protocol_build), env=env)
    env["PKG_CONFIG_PATH"] = str(prefix / "share/pkgconfig")
    actual_protocols = subprocess.check_output(["pkg-config", "--modversion", "wayland-protocols"],
        env=env, text=True).strip()
    if actual_protocols != protocols["version"]:
        raise RuntimeError(f"Unexpected protocol definitions: {actual_protocols}")

    backend = specification["backend"]
    source = workspace / "portal-wlr-source"
    build = workspace / "portal-wlr-build"
    checkout(backend, source, env)
    run("meson", "setup", str(build), str(source), "--wrap-mode=nodownload",
        "--prefix=" + str(prefix), "--libexecdir=libexec", *backend["mesonOptions"], env=env)
    run("meson", "compile", "-C", str(build), "-j", str(args.jobs), env=env)
    run("meson", "install", "-C", str(build), env=env)
    binary = prefix / "libexec/xdg-desktop-portal-wlr"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError("The built validation backend is missing")
    linkage = subprocess.check_output(["ldd", str(binary)], env=env, text=True)
    if "not found" in linkage or ".deps/" in linkage:
        raise RuntimeError("Validation backend did not link exclusively to available host libraries")
    versions = {}
    for package in ("libpipewire-0.3", "wayland-client", "wayland-protocols", "inih", "gbm", "libdrm", "libsystemd"):
        versions[package] = subprocess.check_output(["pkg-config", "--modversion", package],
            env=env, text=True).strip()
    notices = prefix / "share/licenses/validation-desktop"
    notices.mkdir(parents=True)
    shutil.copyfile(source / "LICENSE", notices / "portal-wlr-LICENSE")
    shutil.copyfile(protocol_source / "COPYING", notices / "wayland-protocols-COPYING")
    metadata = {
        "purpose": "Owned CI desktop backend; not distributed with the application",
        "backend": backend, "waylandProtocols": protocols, "hostDependencies": versions,
        "binaryRelativePath": "install/libexec/xdg-desktop-portal-wlr",
        "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "linkage": linkage.splitlines(), "prefix": str(prefix),
    }
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"Validation-only Portal-wlr backend: {binary}")


if __name__ == "__main__":
    main()
