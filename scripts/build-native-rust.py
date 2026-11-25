#!/usr/bin/env python3
"""Build one locked Rust static library and record its actual C/C++ link inputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
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


def source_fingerprints(root):
    paths = [root / "scripts/build-native-rust.py"]
    for directory in ("src/platform/rust", "third_party/localsend", "third_party/gifski"):
        paths.extend(path for path in (root / directory).rglob("*")
                     if path.is_file() and not {"target", ".git", "__pycache__"}.intersection(path.parts))
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(paths)}


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
    source_inputs = source_fingerprints(root)
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
    link_options = []
    library_files = []
    cargo_packages = json.loads(metadata)["packages"]
    for item in matches[0].split():
        # The fixed Windows standard library reports its dynamic CRT as an
        # actual linker directive, not a filename. Preserve this directive;
        # never suppress a CRT conflict or invent /MDd compatibility.
        if host.endswith("windows-msvc") and item.lower() == "/defaultlib:msvcrt":
            link_options.append(item)
            continue
        value = item[2:] if item.startswith("-l") else item
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", value):
            raise SystemExit("Unrecognized native link input: " + item)
        if host.endswith("windows-msvc") and re.fullmatch(r"windows\.\d+\.\d+\.\d+\.lib", value):
            # Official windows-sys bindings use their matching windows-targets
            # import library. Rust reports its name but not Cargo's -L search
            # directory. Resolve only the actual locked host package, preserve
            # its bytes, and pass that verified absolute path to CMake.
            matches = []
            for package in cargo_packages:
                if package["name"] != "windows_x86_64_msvc":
                    continue
                candidate = Path(package["manifest_path"]).parent / "lib" / value
                if candidate.is_file():
                    matches.append((package, candidate))
            if len(matches) != 1:
                raise SystemExit("Missing or ambiguous official Windows import library: " + value)
            package, source = matches[0]
            locked_package = [entry for entry in resolved if entry["name"] == package["name"]
                              and entry["version"] == package["version"]
                              and entry.get("source") == package.get("source")]
            if len(locked_package) != 1 or not locked_package[0].get("checksum"):
                raise SystemExit("Windows import library is not from the resolved registry lock")
            destination = evidence / "native-libraries" / value
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
                raise SystemExit("Windows import library changed during retention")
            library_files.append({"path": destination.as_posix(), "sha256": digest,
                                  "package": package["name"], "version": package["version"],
                                  "registryChecksum": locked_package[0]["checksum"],
                                  "source": package["source"]})
            value = destination.as_posix()
        libraries.append(value)
    windows = host.endswith("windows-msvc")
    if windows and any(re.fullmatch(r"libcmt(?:d)?(?:\.lib)?", item, re.IGNORECASE) for item in libraries):
        raise SystemExit("The aggregate reported a static Windows CRT dependency")
    if windows and not link_options and not any(item.lower() in ("msvcrt", "msvcrt.lib") for item in libraries):
        raise SystemExit("The fixed Rust standard library did not report its expected dynamic release CRT")
    library = target / "release" / ("huhucapture_native.lib" if windows else "libhuhucapture_native.a")
    if not library.is_file():
        raise SystemExit("Cargo did not produce the aggregate static library")
    if source_fingerprints(root) != source_inputs:
        raise SystemExit("Native source files changed during the build")
    result = {
        "schemaVersion": 1, "rustVersion": toolchain, "host": host, "pythonVersion": sys.version,
        "crate": native["package"], "library": library.as_posix(),
        "librarySha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "cargoLockSha256": hashlib.sha256(lock.read_bytes()).hexdigest(),
        "sourceFiles": source_inputs,
        "nativeLibraries": libraries, "nativeLinkOptions": link_options,
        "nativeLibraryFiles": library_files, "codecPackages": pins,
        "windowsCrt": "MD" if windows else None,
        "bootstrapResolution": options.bootstrap_lock,
    }
    (evidence / "build-info.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Aggregate native library:", library)
    print("Actual native link libraries:", " ".join(libraries))
    print("Actual native link options:", " ".join(link_options))
    print("Cargo.lock SHA256:", result["cargoLockSha256"])


if __name__ == "__main__":
    main()
