#!/usr/bin/env python3
"""Build the pinned, distributable software-codec validation dependency set."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(*arguments, cwd=None, env=None):
    subprocess.run(arguments, cwd=cwd, env=env, check=True)


def checkout(specification, source):
    run("git", "init", str(source))
    run("git", "remote", "add", "origin", specification["repository"], cwd=source)
    run("git", "fetch", "--depth=1", "--no-tags", "origin", specification["commit"], cwd=source)
    run("git", "-c", "advice.detachedHead=false", "checkout", "--detach", "FETCH_HEAD", cwd=source)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if actual != specification["commit"]:
        raise SystemExit(f"Unexpected dependency revision: {actual}")


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/media-validation")
    parser.add_argument("--jobs", type=int, default=2)
    options = parser.parse_args()
    if options.jobs < 1 or options.jobs > 64:
        raise SystemExit("--jobs must be between 1 and 64")
    specification = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]["softwareMedia"]
    workspace = options.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    if any(workspace.iterdir()):
        raise SystemExit(f"Use an empty build workspace; existing files retained: {workspace}")
    prefix = workspace / "install"
    env = os.environ.copy()
    env["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig") + (":" + env["PKG_CONFIG_PATH"] if env.get("PKG_CONFIG_PATH") else "")
    metadata = {"prefix": str(prefix), "components": {}}
    for name in ("x264", "ffmpeg"):
        package = specification[name]
        source = workspace / (name + "-source")
        build = workspace / (name + "-build")
        checkout(package, source)
        for patch in package.get("patches", []):
            path = root / patch["path"]
            if hashlib.sha256(path.read_bytes()).hexdigest() != patch["sha256"]:
                raise SystemExit(f"Controlled codec patch checksum mismatch: {path.name}")
            run("git", "apply", "--check", str(path), cwd=source)
            run("git", "apply", str(path), cwd=source)
        build.mkdir()
        arguments = [str(source / "configure"), "--prefix=" + str(prefix), *package["configure"]]
        run(*arguments, cwd=build, env=env)
        run("make", "-j" + str(options.jobs), cwd=build, env=env)
        run("make", "install", cwd=build, env=env)
        metadata["components"][name] = {"repository": package["repository"], "commit": package["commit"],
            "patches": package.get("patches", []), "configure": arguments}
    metadata["pkgConfig"] = subprocess.check_output(
        ["pkg-config", "--modversion", "x264", "libavcodec", "libavformat", "libavutil", "libswscale", "libswresample"],
        env=env, text=True).splitlines()
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"Software media prefix: {prefix}")
    print("This builds libraries, not an external ffmpeg executable or a product installer.")


if __name__ == "__main__":
    main()
