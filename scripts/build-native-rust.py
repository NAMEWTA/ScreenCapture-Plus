#!/usr/bin/env python3
"""Build one locked Rust static library and record its actual C/C++ link inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tomllib


def run(arguments, cwd, capture=False):
    result = subprocess.run(arguments, cwd=cwd, text=True, encoding="utf-8",
                            errors="replace", stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.stderr:
        print(result.stderr, end="", flush=True)
    if not capture and result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    return result.stdout, result.stderr


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootstrap-lock", action="store_true",
                        help="Review-only initial Cargo resolution; normal builds always use --locked")
    parser.add_argument("--evidence", type=Path, default=root / "artifacts/native-rust")
    parser.add_argument("--target-dir", type=Path,
                        default=Path(os.environ.get("CARGO_TARGET_DIR", root / "artifacts/cargo/localsend")))
    options = parser.parse_args()
    specification = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]
    native = specification["nativeRust"]
    toolchain = specification["rustToolchain"]
    manifest = root / native["workspaceManifest"]
    workspace = manifest.parent
    if "+crt-static" in os.environ.get("RUSTFLAGS", "") + os.environ.get("CARGO_ENCODED_RUSTFLAGS", ""):
        raise SystemExit("The Qt aggregate library requires dynamic CRT; remove +crt-static")
    version, _ = run(["rustc", "+" + toolchain, "--version", "--verbose"], workspace, capture=True)
    if not version.startswith("rustc " + toolchain + " "):
        raise SystemExit("Unexpected Rust toolchain")
    host = next((line.removeprefix("host: ") for line in version.splitlines() if line.startswith("host: ")), "")
    if host not in ("x86_64-pc-windows-msvc", "x86_64-unknown-linux-gnu"):
        raise SystemExit("The native ABI validation currently targets Windows x64 and Linux amd64")
    cargo = ["cargo", "+" + toolchain, "--color", "never"]
    source_lock = root / "third_party/gifski/upstream-cargo.lock"
    source_bytes = source_lock.read_bytes()
    source_manifest = json.loads((source_lock.parent / "upstream.json").read_text(encoding="utf-8"))
    expected = next(entry["blob"] for entry in source_manifest["files"] if entry["path"] == source_lock.name)
    actual = hashlib.sha1(b"blob " + str(len(source_bytes)).encode() + b"\0" + source_bytes).hexdigest()
    if actual != expected:
        raise SystemExit("The reviewed upstream GIF dependency lock has changed")
    original = tomllib.loads(source_bytes.decode("utf-8"))["package"]
    pins = {}
    for name in native["preserveCodecPackages"]:
        choices = [entry["version"] for entry in original if entry["name"] == name]
        if len(choices) != 1:
            raise SystemExit("Ambiguous upstream codec version: " + name)
        pins[name] = choices[0]

    evidence = options.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    if options.bootstrap_lock:
        # Let Cargo preserve the existing core lock and add the actual new
        # dependency graph. Never manufacture registry checksums or Git objects.
        run(cargo + ["metadata", "--format-version", "1"], workspace, capture=True)
        for name, version in pins.items():
            run(cargo + ["update", "-p", name, "--precise", version], workspace)
    lock = workspace / "Cargo.lock"
    resolved = tomllib.loads(lock.read_text(encoding="utf-8"))["package"]
    for name, expected in pins.items():
        versions = {entry["version"] for entry in resolved if entry["name"] == name}
        if versions != {expected}:
            raise SystemExit(f"Codec dependency drift for {name}: {sorted(versions)}; expected {expected}")
    metadata, _ = run(cargo + ["metadata", "--format-version", "1", "--locked"], workspace, capture=True)
    (evidence / "cargo-metadata.json").write_text(metadata, encoding="utf-8")
    target = options.target_dir.resolve()
    # Keep debug information in the production CRT configuration. A Cargo dev
    # build does not turn the Windows Rust standard library into /MDd.
    command = cargo + ["rustc", "-p", native["package"], "--release", "--locked",
                       "--target-dir", str(target), "--", "-C", "debuginfo=2", "--print", "native-static-libs"]
    output, errors = run(command, workspace, capture=True)
    log = output + errors
    (evidence / "build.log").write_text(log, encoding="utf-8")
    matches = re.findall(r"native-static-libs:\s*([^\r\n]+)", log)
    if len(matches) != 1:
        raise SystemExit("Rust did not report one exact native-static-libs list")
    libraries = []
    for item in matches[0].split():
        value = item[2:] if item.startswith("-l") else item
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", value):
            raise SystemExit("Unrecognized native link input: " + item)
        libraries.append(value)
    windows = host.endswith("windows-msvc")
    if windows and any(re.fullmatch(r"libcmt(?:d)?(?:\.lib)?", item, re.IGNORECASE) for item in libraries):
        raise SystemExit("The aggregate reported a static Windows CRT dependency")
    library = target / "release" / ("huhucapture_native.lib" if windows else "libhuhucapture_native.a")
    if not library.is_file():
        raise SystemExit("Cargo did not produce the aggregate static library")
    result = {
        "schemaVersion": 1, "rustVersion": toolchain, "host": host, "pythonVersion": sys.version,
        "crate": native["package"], "library": library.as_posix(),
        "librarySha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "cargoLockSha256": hashlib.sha256(lock.read_bytes()).hexdigest(),
        "nativeLibraries": libraries, "codecPackages": pins,
        "windowsCrt": "MD" if windows else None,
        "bootstrapResolution": options.bootstrap_lock,
    }
    (evidence / "build-info.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Aggregate native library:", library)
    print("Actual native link libraries:", " ".join(libraries))
    print("Cargo.lock SHA256:", result["cargoLockSha256"])


if __name__ == "__main__":
    main()
