#!/usr/bin/env python3
"""Build the locked CPU Sway/Portal-wlr desktop for owned CI sessions only.

This is a test desktop service, never an application-bundled Portal daemon. Its
PipeWire daemon and policy remain the runner's system services. Only this
private compositor/backend prefix receives the fixed Wayland/wlroots stack;
it is independent of the client libraries used by huhucapture.
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
    env["PKG_CONFIG_PATH"] = os.pathsep.join(str(prefix / path) for path in ("lib/pkgconfig", "share/pkgconfig"))
    env["LD_LIBRARY_PATH"] = str(prefix / "lib")
    env["PATH"] = str(prefix / "bin") + ":/usr/bin:/bin"
    notices = prefix / "share/licenses/validation-desktop"
    notices.mkdir(parents=True)

    def build_component(name: str, spec: dict) -> None:
        source = workspace / f"{name}-source"
        build = workspace / f"{name}-build"
        component_env = env.copy()
        if name == "sway":
            # With the headless-only wlroots build Sway does not request the
            # libinput pkg-config dependency, but its shared configuration and
            # command sources still include >=1.26 types unconditionally. The
            # same fixed public header must reach feature probes and compile.
            component_env["C_INCLUDE_PATH"] = str(prefix / "include")
        checkout(spec, source, env)
        for patch in spec.get("patches", []):
            path = root / patch["path"]
            if hashlib.sha256(path.read_bytes()).hexdigest() != patch["sha256"]:
                raise RuntimeError(f"Validation desktop patch checksum mismatch: {path.name}")
            run("git", "apply", "--check", str(path), cwd=source, env=env)
            run("git", "apply", str(path), cwd=source, env=env)
        run("meson", "setup", str(build), str(source), "--wrap-mode=nodownload",
            "--prefix=" + str(prefix), "--libdir=lib", "--libexecdir=libexec", "--buildtype=release",
            *spec["mesonOptions"], env=component_env)
        run("meson", "compile", "-C", str(build), "-j", str(args.jobs), env=component_env)
        run("meson", "install", "-C", str(build), env=component_env)
        if "pkgConfig" in spec:
            actual = subprocess.check_output(["pkg-config", "--modversion", spec["pkgConfig"]],
                env=env, text=True).strip()
            if actual != spec["version"]:
                raise RuntimeError(f"Unexpected {name} version: {actual}")
        for relative in spec["licenseFiles"]:
            shutil.copyfile(source / relative, notices / (name + "-" + relative.replace("/", "-")))

    # Portal-wlr's software SHM path uses ext-image-copy-capture. Sway 1.9's
    # wlr-screencopy plus pixman supplies no linux-dmabuf global and cannot
    # drive that path. Build compatible upstream code instead of modifying
    # the protocol implementation or treating failed capture as a pass.
    for name, key in (("wayland", "wayland"), ("wayland-protocols", "waylandProtocols"),
                      ("libdrm", "libdrm"), ("pixman", "pixman"), ("libinput", "libinput"),
                      ("wlroots", "wlroots"), ("sway", "sway")):
        build_component(name, specification[key])
    backend = specification["backend"]
    build_component("portal-wlr", backend)
    binary = prefix / "libexec/xdg-desktop-portal-wlr"
    binaries = {}
    for name, relative in (("sway", "bin/sway"), ("swaymsg", "bin/swaymsg"),
                           ("xdg-desktop-portal-wlr", "libexec/xdg-desktop-portal-wlr")):
        path = prefix / relative
        if not path.is_file() or not os.access(path, os.X_OK):
            raise RuntimeError(f"The built validation desktop binary is missing: {name}")
        linkage = subprocess.check_output(["ldd", str(path)], env=env, text=True)
        if "not found" in linkage:
            raise RuntimeError(f"Unavailable validation desktop runtime dependency: {name}")
        for line in linkage.splitlines():
            if "=> /" not in line:
                continue
            library = Path(line.split("=> ", 1)[1].split(" (", 1)[0]).resolve()
            if not any(library.is_relative_to(base) for base in (prefix, Path("/usr/lib"), Path("/lib"))):
                raise RuntimeError(f"Validation desktop imported a non-host/application library: {library}")
        binaries[name] = {"relativePath": relative, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                          "linkage": linkage.splitlines()}
    runtime_files = []
    for path in sorted((prefix / "lib").glob("*.so*")):
        if not path.resolve(strict=True).is_relative_to(prefix):
            raise RuntimeError(f"A validation library escapes its private prefix: {path.name}")
        runtime_files.append({"relativePath": path.relative_to(prefix).as_posix(),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "symlink": os.readlink(path) if path.is_symlink() else None})
    versions = {}
    for package in ("libpipewire-0.3", "wayland-client", "wayland-protocols", "wlroots-0.19", "pixman-1", "inih", "gbm", "libdrm", "libsystemd", "libinput"):
        versions[package] = subprocess.check_output(["pkg-config", "--modversion", package],
            env=env, text=True).strip()
    metadata = {
        "purpose": "Owned CI desktop; not distributed with the application",
        "schema": 2, "sources": specification, "dependencies": versions,
        "backend": backend, "waylandProtocols": specification["waylandProtocols"],
        "binaryRelativePath": "install/libexec/xdg-desktop-portal-wlr",
        "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "binaries": binaries, "runtimeFiles": runtime_files, "prefix": str(prefix),
    }
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"Validation-only Portal-wlr backend: {binary}")


if __name__ == "__main__":
    main()
