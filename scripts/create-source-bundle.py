#!/usr/bin/env python3
"""Collect real corresponding-source inputs offline; never manufacture completeness."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import runpy
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import types

HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
IDENTIFIER = re.compile(r"[a-z0-9][a-z0-9._+-]{0,95}\Z")
MAX_FILE = 2 * 1024**3
MAX_TREE = 32 * 1024**3
MAX_FILES = 500_000
CARGO_REGISTRY_VENDOR_TOOLCHAIN = "1.97.1"
CARGO_REGISTRY_VENDOR_COMMIT = "c980f4866141969fab6254a680546a277789d6f0"
CARGO_REGISTRY_VENDOR_SOURCE = ("https://github.com/rust-lang/cargo/blob/" +
                                CARGO_REGISTRY_VENDOR_COMMIT + "/src/cargo/ops/vendor.rs")
COMMON = {"huhucapture", "localsend", "gifski", "rust-vendor", "rust-toolchain-source", "ffmpeg",
          "qtbase", "qtsvg", "qttranslations"}
APPIMAGE_STATIC_SOURCES = {"appimage-musl", "appimage-gcc", "appimage-fuse", "appimage-squashfuse", "appimage-zstd",
                          "appimage-zlib", "appimage-mimalloc"}
LINUX = {"qtwayland", "libportal", "pipewire", "x264", "openssl", "icu", "appimage-runtime"} | APPIMAGE_STATIC_SOURCES


def require(condition, message):
    if not condition:
        raise ValueError(message)


def appimage_static_inputs_present(receipts):
    return any(any(source["id"] == "appimage-runtime" for source in receipt["sources"]) and
               APPIMAGE_STATIC_SOURCES <= {item["component"] for item in receipt["staticInputs"]}
               for receipt in receipts.values())


def canonical(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def source_output_names(version, platform):
    require(platform in ("windows", "linux"), "Unsupported source bundle platform")
    architecture = "x64" if platform == "windows" else "amd64"
    stem = "huhucapture-v" + version + "-" + platform + "-" + architecture
    return stem + "-sources.tar.gz", stem + "-source-manifest.json"


def digest(path, algorithm="sha256"):
    value = hashlib.new(algorithm)
    with Path(path).open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def object_digest(kind, data):
    return hashlib.sha1(kind.encode() + b" " + str(len(data)).encode() + b"\0" + data).hexdigest()


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path, limit=64 * 1024**2):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= limit,
            f"Missing, linked or oversized JSON input: {path}")
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)


def relative(value):
    require(isinstance(value, str) and value and not value.startswith("/") and
            not any(c in value for c in ("\\", ":", "\0")) and
            all(ord(c) >= 32 for c in value) and len(value.encode()) <= 4096 and
            all(part not in ("", ".", "..") for part in value.split("/")),
            f"Unsafe relative path: {value!r}")
    require(".git" not in value.split("/"), f"Git administrative data is not a source file: {value}")
    return value


def checked_link(path, target):
    require(target and not target.startswith("/") and "\\" not in target and ":" not in target and
            not any(ord(c) < 32 for c in target), f"Unsafe source link: {path}")
    parts = list(PurePosixPath(path).parent.parts)
    for part in target.split("/"):
        if part == "..":
            require(parts, f"Source link escapes its component: {path}")
            parts.pop()
        elif part not in ("", "."):
            parts.append(part)
    require(".git" not in parts, f"Source link points at Git administration: {path}")
    return target


def ensure_parents(root, path):
    for parent in path.relative_to(root).parents:
        require(not (root / parent).is_symlink(), f"Source entry traverses a symlink: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)


def header_path_api():
    return runpy.run_path(str(Path(__file__).resolve().parents[1] / "build/package/linux_header_paths.py"))


def path_entries(profile):
    return header_path_api()["entries"](profile) if profile is not None else {}


def check_unique_path(name, names, reviewed):
    previous = names.setdefault(name.casefold(), set())
    require(name not in previous and (not previous or name in reviewed and previous <= reviewed.keys()),
            f"Duplicate or case-colliding source/archive path: {name}")
    previous.add(name)


def inventory(root, *, path_profile=None):
    root = Path(root)
    require(root.is_dir() and not root.is_symlink(), f"Source directory is missing or linked: {root}")
    result, total, names = [], 0, {}
    reviewed = path_entries(path_profile)
    for folder, directories, files in os.walk(root, followlinks=False):
        directories.sort()
        for name in list(directories):
            if (Path(folder) / name).is_symlink():
                directories.remove(name)
                files.append(name)
        for name in sorted(files):
            path = Path(folder) / name
            key = relative(path.relative_to(root).as_posix())
            check_unique_path(key, names, reviewed)
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                target = checked_link(key, os.readlink(path))
                data = target.encode("utf-8")
                item = {"path": key, "type": "symlink", "target": target,
                        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "mode": 0o777}
            else:
                require(stat.S_ISREG(info.st_mode) and info.st_size <= MAX_FILE,
                        f"Special or oversized source file: {key}")
                item = {"path": key, "type": "file", "size": info.st_size,
                        "sha256": digest(path), "mode": 0o755 if info.st_mode & 0o111 else 0o644}
            total += item["size"]
            if key in reviewed:
                require(all(item.get(field) == value for field, value in reviewed[key].items()),
                        "A case-distinct Linux header differs from its exact original file: " + key)
            require(total <= MAX_TREE and len(result) < MAX_FILES, "Source input exceeds collection bounds")
            result.append(item)
    for item in result:
        if item["type"] == "symlink":
            try:
                resolved = (root / item["path"]).resolve(strict=True)
            except (OSError, RuntimeError) as error:
                raise ValueError("Dangling or cyclic source link: " + item["path"]) from error
            require(resolved.is_relative_to(root.resolve()),
                    "Resolved source link escapes its component: " + item["path"])
    return sorted(result, key=lambda item: item["path"])


def tree_digest(files):
    return hashlib.sha256(canonical(files)).hexdigest()


def recognizable_source(identifier, pin, files):
    if any(Path(entry['path']).suffix.lower() in ('.c', '.cc', '.cpp', '.h', '.hpp', '.rs', '.ts', '.s')
           for entry in files if entry['type'] == 'file'):
        return True
    # Canonical musl-cross-make consists of build scripts and patches. Retain
    # the complete exact Git source; do not invent a C file or silently omit
    # the build system used to produce the compiler/CRT/static libraries.
    if identifier == 'appimage-toolchain-build' and pin and pin.get('repository') == \
            'https://github.com/richfelker/musl-cross-make.git' and \
            pin.get('commit') == '5e8042e63de29f8473fc3b0b831a587ba284e1a8':
        names = {entry['path'] for entry in files if entry['type'] == 'file' and entry['size']}
        return {'Makefile', 'litecross/Makefile', 'LICENSE', 'cowpatch.sh'} <= names
    return False


def git_environment():
    environment = os.environ.copy()
    for key in tuple(environment):
        if key.startswith("GIT_"):
            del environment[key]
    environment.update(GIT_OPTIONAL_LOCKS="0", GIT_NO_REPLACE_OBJECTS="1", GIT_NO_LAZY_FETCH="1",
                       GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_SYSTEM=os.devnull, GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_TERMINAL_PROMPT="0", GIT_PROTOCOL_FROM_USER="0")
    return environment


def git_command(directory, *arguments):
    return ["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false",
            "-c", "core.hooksPath=" + os.devnull, "-c", "log.showSignature=false", "-c", "protocol.allow=never",
            "-C", str(directory), *arguments]


def git(directory, *arguments, input=None):
    # No checkout, index refresh, fetch, filters or hooks are run in an input.
    result = subprocess.run(git_command(directory, *arguments),
                            input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=git_environment())
    require(result.returncode == 0, "Git source check failed: " + result.stderr.decode("utf-8", "replace").strip())
    return result.stdout


def git_files(directory, commit):
    result = []
    for record in git(directory, "ls-tree", "-r", "-z", "--full-tree", commit).split(b"\0"):
        if not record:
            continue
        header, raw_path = record.split(b"\t", 1)
        mode, kind, sha = header.decode("ascii").split()
        path = relative(raw_path.decode("utf-8"))
        require(mode in ("100644", "100755", "120000", "160000"), f"Unsupported Git mode: {mode}")
        result.append((mode, kind, sha, path))
    require(result and len(result) <= MAX_FILES, "Empty or oversized Git source")
    return result


def export_git_objects(directory, commit, destination):
    require(HEX40.fullmatch(commit or ""), "An exact Git commit is required")
    directory = Path(directory).resolve()
    top_level = Path(git(directory, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    require(top_level.samefile(directory),
            f"Source must be its own Git checkout: {directory}")
    require(git(directory, "rev-parse", "HEAD").decode().strip() == commit, "Source HEAD differs from its pin")
    require(git(directory, "cat-file", "-t", commit).strip() == b"commit", "Source pin is not a commit")
    destination.mkdir(parents=True, exist_ok=True)
    submodules = []
    entries = git_files(directory, commit)
    process = subprocess.Popen(git_command(directory, "cat-file", "--batch"),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=git_environment())
    try:
        for mode, kind, sha, name in entries:
            path = destination / name
            ensure_parents(destination, path)
            if mode == "160000":
                submodules.append({"path": name, "commit": sha,
                                   "submodules": export_git_objects(directory / name, sha, path)})
                continue
            process.stdin.write((sha + "\n").encode())
            process.stdin.flush()
            header = process.stdout.readline().decode().strip().split()
            require(len(header) == 3 and header[:2] == [sha, "blob"] and int(header[2]) <= MAX_FILE,
                    "Git source blob is missing or oversized")
            size = int(header[2])
            value = hashlib.sha1(b"blob " + str(size).encode() + b"\0")
            if mode == "120000":
                require(size <= 4096, "Oversized Git source link")
                data = process.stdout.read(size)
                value.update(data)
                os.symlink(checked_link(name, data.decode("utf-8")), path)
            else:
                with path.open("xb") as output:
                    remaining = size
                    while remaining:
                        chunk = process.stdout.read(min(remaining, 1024 * 1024))
                        require(chunk, "Truncated Git blob stream")
                        value.update(chunk)
                        output.write(chunk)
                        remaining -= len(chunk)
                path.chmod(0o755 if mode == "100755" else 0o644)
            require(process.stdout.read(1) == b"\n" and value.hexdigest() == sha,
                    "Git source blob failed its object hash")
        process.stdin.close()
        require(process.wait() == 0, "Git source export failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdout.close()
        process.stderr.close()
        if not process.stdin.closed:
            process.stdin.close()
    return submodules


def compare_checkout(directory, expected, commit):
    directory = Path(directory)
    allowed = {item["path"] for item in expected}
    for mode, _, sha, name in git_files(directory, commit):
        if mode == "160000":
            nested = [{**item, "path": item["path"][len(name) + 1:]} for item in expected
                      if item["path"].startswith(name + "/")]
            compare_checkout(directory / name, nested, sha)
        elif name not in allowed:
            require(not os.path.lexists(directory / name), "Patch-deleted source remains in the actual checkout: " + name)
    for raw in git(directory, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0"):
        if raw:
            name = raw.decode("utf-8")
            require(name in allowed, f"Unrecorded source input in checkout: {name}")
    for item in expected:
        path = directory / item["path"]
        require(all(not (directory / parent).is_symlink() for parent in Path(item["path"]).parents),
                "Checkout source traverses a symlinked directory: " + item["path"])
        require(path.is_symlink() if item["type"] == "symlink" else path.is_file() and not path.is_symlink(),
                f"Checkout source is missing or has a different type: {item['path']}")
        if item["type"] == "symlink":
            require(os.readlink(path) == item["target"], f"Checkout link changed: {item['path']}")
        else:
            require(path.stat().st_size == item["size"] and digest(path) == item["sha256"],
                    f"Checkout contains an unrecorded source change: {item['path']}")
            if os.name != "nt":
                require(bool(path.stat().st_mode & 0o111) == (item["mode"] == 0o755),
                        f"Checkout executable mode changed: {item['path']}")
    require(git(directory, "rev-parse", "HEAD").decode().strip() == commit, "Source HEAD changed during collection")


def apply_patches(destination, patches):
    if not patches:
        return
    # Only the disposable export gets an index/repository. The caller's worktree,
    # index, branch, and upstream repository remain untouched.
    git(destination, "init", "--quiet")
    try:
        for patch in patches:
            git(destination, "apply", "--check", "--whitespace=nowarn", str(patch))
            git(destination, "apply", "--whitespace=nowarn", str(patch))
    finally:
        shutil.rmtree(destination / ".git")


def export_git(directory, commit, destination, patches=()):
    submodules = export_git_objects(directory, commit, destination)
    pristine = tree_digest(inventory(destination))
    apply_patches(destination, patches)
    files = inventory(destination)
    compare_checkout(directory, files, commit)
    return {"kind": "git", "commit": commit, "pristineTreeSha256": pristine,
            "sourceTreeSha256": tree_digest(files), "submodules": submodules}, files


def extract_tar(archive, destination, prefix="", *, path_profile=None):
    if prefix:
        relative(prefix)
    names, total, count = {}, 0, 0
    reviewed = path_entries(path_profile)
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:*") as stream:
        for member in stream:
            name = member.name.removeprefix("./").rstrip("/")
            if not name:
                continue
            relative(name)
            if prefix:
                if name == prefix and member.isdir():
                    continue
                require(name.startswith(prefix + "/"), "Source archive has an entry outside the selected root")
                name = name[len(prefix) + 1:]
            relative(name)
            check_unique_path(name, names, reviewed)
            count += 1
            require(count <= MAX_FILES and member.size <= MAX_FILE, "Source archive exceeds bounds")
            if name in reviewed:
                require(member.isfile(), "A case-distinct Linux header must remain an original regular file")
            path = destination / name
            ensure_parents(destination, path)
            if member.isdir():
                path.mkdir(exist_ok=True)
            elif member.issym():
                os.symlink(checked_link(name, member.linkname), path)
            else:
                require(member.isfile(), f"Source archive has a hardlink or special entry: {name}")
                total += member.size
                require(total <= MAX_TREE, "Source archive exceeds the expanded byte limit")
                source = stream.extractfile(member)
                require(source is not None, f"Missing archive data: {name}")
                with source, path.open("xb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
                require(path.stat().st_size == member.size, f"Truncated source archive entry: {name}")
                path.chmod(0o755 if member.mode & 0o111 else 0o644)
    return inventory(destination, path_profile=path_profile)


def copy_verified_tree(source, destination, expected, *, path_profile=None):
    destination.mkdir(parents=True)
    for item in expected:
        path = destination / item["path"]
        ensure_parents(destination, path)
        if item["type"] == "symlink":
            os.symlink(item["target"], path)
        else:
            shutil.copyfile(source / item["path"], path)
            path.chmod(item["mode"])
    require(inventory(destination, path_profile=path_profile) == expected and
            inventory(source, path_profile=path_profile) == expected,
            "Source files changed while they were retained")


def descriptor(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= MAX_FILE,
            f"Missing, empty, linked or oversized retained material: {path}")
    return {"name": path.name, "size": path.stat().st_size, "sha256": digest(path)}


def locked_sources(project):
    lock = read_json(project / "build/dependencies.lock.json")
    migration = lock["qtMigration"]
    package_lock = read_json(project / "build/package/dependencies.lock.json")
    qt = migration["qt"]
    result = {"qtbase": {"repository": qt["qtbaseRepository"], "commit": qt["qtbaseCommit"]},
              "gifski": lock["gifski"], "libportal": migration["libportal"], "pipewire": migration["pipewire"],
              "ffmpeg": migration["softwareMedia"]["ffmpeg"], "x264": migration["softwareMedia"]["x264"],
              "appimage-runtime": package_lock["tools"]["appimageRuntime"]}
    for name in ("qtsvg", "qtwayland", "qttranslations"):
        if name in qt.get("sourceModules", {}):
            result[name] = qt["sourceModules"][name]
    if "qttools" in qt.get("sourceTools", {}):
        result["qttools"] = qt["sourceTools"]["qttools"]
    if "icu" in qt.get("sourceDependencies", {}):
        result["icu"] = qt["sourceDependencies"]["icu"]
    for identifier, pin in package_lock.get("runtimeSourceBuild", {}).get("sources", {}).items():
        require(IDENTIFIER.fullmatch(identifier) and identifier not in result,
                "A locked runtime source cannot override another component pin")
        result[identifier] = pin
    return lock, result


def license_files(root, files=None):
    return [entry["path"] for entry in (files if files is not None else inventory(root)) if entry["type"] == "file" and
            (Path(entry["path"]).name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE")) or
             "LICENSES" in (part.upper() for part in Path(entry["path"]).parts)) and entry["size"]]


def cargo_registry_vendor_file(path):
    # Match vendor_this in the locked Cargo 1.97.1 registry unpack operation:
    # https://github.com/rust-lang/cargo/blob/c980f4866141969fab6254a680546a277789d6f0/src/cargo/ops/vendor.rs
    # This is only the comparison view. Original .crate bytes remain retained,
    # and the existing archive/path safety checks still run before this filter.
    relative_path = PurePosixPath(path)
    return (".git" not in relative_path.parts and
            relative_path.name not in (".gitattributes", ".gitignore") and path != ".cargo-ok")


def validate_vendor(vendor, cargo_lock, registry_cache, git_sources, gaps, external_licenses=None):
    packages = tomllib.loads(cargo_lock.read_text(encoding="utf-8"))["package"]
    wanted = {(p["name"], p["version"]): p for p in packages if p.get("source")}
    require(len(wanted) == sum(bool(p.get("source")) for p in packages),
            "Ambiguous same-name/version Cargo sources need explicit support")
    found, records = set(), []
    for directory in sorted(vendor.iterdir()):
        require(directory.is_dir() and not directory.is_symlink(), "Vendor root may contain only real crate directories")
        manifest = tomllib.loads((directory / "Cargo.toml").read_text(encoding="utf-8"))
        package = manifest["package"]
        key = package["name"], package["version"]
        require(key in wanted and key not in found, f"Unexpected or duplicate vendored package: {key}")
        found.add(key)
        locked = wanted[key]
        checksum = read_json(directory / ".cargo-checksum.json")
        files = inventory(directory)
        by_path = {p["path"]: p for p in files if p["path"] != ".cargo-checksum.json"}
        require(all(p["type"] == "file" for p in files), "Cargo directory sources must contain regular files")
        require(set(checksum.get("files", {})) == set(by_path), f"Vendor checksum file list differs: {key}")
        for name, expected in checksum["files"].items():
            relative(name)
            require(HEX64.fullmatch(expected or "") and by_path[name]["sha256"] == expected,
                    f"Vendored file checksum mismatch: {key}/{name}")
        if locked["source"].startswith("registry+"):
            require(checksum.get("package") == locked.get("checksum") and HEX64.fullmatch(locked.get("checksum", "")),
                    f"Registry package checksum differs from Cargo.lock: {key}")
            archive = registry_cache / (key[0] + "-" + key[1] + ".crate") if registry_cache else None
            if archive is None or not archive.is_file():
                gaps.append(f"rust-vendor:{key[0]}@{key[1]}: original checksum-pinned .crate archive is missing")
            else:
                require(digest(archive) == locked["checksum"], f"Registry archive differs from Cargo.lock: {key}")
                with tempfile.TemporaryDirectory(prefix="huhucapture-crate-") as temp:
                    original = extract_tar(archive, Path(temp), key[0] + "-" + key[1])
                    actual = {p["path"]: {k: v for k, v in p.items() if k != "mode"} for p in by_path.values()}
                    expected = {p["path"]: {k: v for k, v in p.items() if k != "mode"} for p in original
                                if cargo_registry_vendor_file(p["path"])}
                    if actual != expected:
                        differences = (
                            ("missing", sorted(expected.keys() - actual.keys())),
                            ("unexpected", sorted(actual.keys() - expected.keys())),
                            ("changed", sorted(name for name in actual.keys() & expected.keys()
                                               if actual[name] != expected[name])),
                        )
                        detail = "; ".join(f"{label}={len(paths)} {[name[:160] for name in paths[:5]]!r}"
                                           for label, paths in differences)
                        raise ValueError(f"Vendored bytes differ from the locked registry archive: {key}; {detail}")
        elif locked["source"].startswith("git+"):
            require(checksum.get("package") is None, "Git vendor packages must not claim a registry checksum")
            commit = locked["source"].rsplit("#", 1)[-1]
            source = git_sources.get(commit)
            if source is None:
                gaps.append(f"rust-vendor:{key[0]}@{key[1]}: exact upstream Git source is missing")
            else:
                original_manifest = tomllib.loads((source / "Cargo.toml").read_text(encoding="utf-8"))
                require(original_manifest["package"]["name"] == key[0] and
                        original_manifest["package"]["version"] == key[1], "Git vendor package identity differs")
                for name, item in by_path.items():
                    if name == "Cargo.toml":
                        # Cargo normalizes Git manifests for directory replacement.
                        # Both complete upstream and actual normalized inputs survive;
                        # their different hashes are recorded, never called unmodified.
                        continue
                    upstream_name = "Cargo.toml" if name == "Cargo.toml.orig" else name
                    original = source / relative(upstream_name)
                    require(original.is_file() and not original.is_symlink() and digest(original) == item["sha256"],
                            f"Git vendor source differs from the pinned checkout: {key}/{name}")
                if manifest != original_manifest:
                    gaps.append(f"rust-vendor:{key[0]}@{key[1]}: normalized Git Cargo.toml requires a pinned-Cargo replay receipt")
        else:
            raise ValueError("Unsupported Cargo source: " + locked["source"])
        licenses = license_files(directory)
        retained_license = (external_licenses or {}).get(key[0] + "@" + key[1])
        if retained_license:
            require(retained_license.get("registryChecksum") == locked.get("checksum"),
                    "External crate license reference is for a different registry package")
        if not licenses and not retained_license:
            gaps.append(f"rust-vendor:{key[0]}@{key[1]}: no retained license/copyright text")
        records.append({"name": key[0], "version": key[1], "source": locked["source"],
                        "registryChecksum": locked.get("checksum"), "sourceTreeSha256": tree_digest(files),
                        "manifestSha256": by_path["Cargo.toml"]["sha256"], "licenses": licenses,
                        "externalLicense": retained_license})
    for key in sorted(set(wanted) - found):
        gaps.append(f"rust-vendor:{key[0]}@{key[1]}: locked package source is missing")
    return records


def validate_cargo_config(path, packages):
    value = tomllib.loads(path.read_text(encoding="utf-8"))
    require(set(value) <= {"source", "net"}, "Vendor configuration contains unrelated Cargo overrides")
    source = value.get("source", {})
    directories = [name for name, item in source.items() if "directory" in item]
    require(len(directories) == 1, "Vendor configuration must use one retained directory source")
    target = directories[0]
    # The collector writes a relocatable replacement after checking the original.
    require(set(source[target]) == {"directory"}, "Directory source contains unexpected overrides")
    registries = {p["source"] for p in packages if p["source"].startswith("registry+")}
    require(registries <= {"registry+https://github.com/rust-lang/crates.io-index"},
            "Additional Cargo registries require explicit source replacement support")
    if registries:
        require(source.get("crates-io", {}).get("replace-with") == target, "Crates.io is not replaced by the vendor")
    for location in {p["source"] for p in packages if p["source"].startswith("git+")}:
        url, revision = location.removeprefix("git+").split("?rev=", 1)
        revision, commit = revision.split("#", 1)
        require(revision == commit and any(item.get("git", "").removesuffix(".git") == url.removesuffix(".git") and
                item.get("rev") == revision and item.get("replace-with") == target for item in source.values()),
                "The exact locked Git source is not replaced by the vendor")
    value["source"][target]["directory"] = "../rust-vendor"
    value["net"] = {"offline": True}
    lines = []
    for name, fields in sorted(value["source"].items()):
        lines.append("[source." + json.dumps(name) + "]")
        lines.extend(key + " = " + json.dumps(text) for key, text in sorted(fields.items()))
        lines.append("")
    lines += ["[net]", "offline = true", ""]
    return "\n".join(lines).encode()


def make_archive(root, archive, epoch, *, path_profile=None):
    files = inventory(root, path_profile=path_profile)
    with archive.open("xb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0, compresslevel=6) as zipped:
        with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for item in files:
                info = tarfile.TarInfo("huhucapture-sources/" + item["path"])
                info.mode, info.mtime, info.uid, info.gid = item["mode"], epoch, 0, 0
                info.uname = info.gname = ""
                if item["type"] == "symlink":
                    info.type, info.linkname = tarfile.SYMTYPE, item["target"]
                    tar.addfile(info)
                else:
                    info.size = item["size"]
                    with (root / item["path"]).open("rb") as source:
                        tar.addfile(info, source)
    require(inventory(root, path_profile=path_profile) == files, "Collected source tree changed during archiving")
    # Reopen and hash every archived entry, not merely the gzip header.
    with tempfile.TemporaryDirectory(prefix="huhucapture-source-verify-") as temp:
        actual = extract_tar(archive, Path(temp), "huhucapture-sources", path_profile=path_profile)
        require(actual == files, "Written source archive differs from the verified collected tree")
    return descriptor(archive)


def json_pointer(value, pointer):
    require(isinstance(pointer, str) and (pointer == "" or pointer.startswith("/")), "Invalid evidence JSON pointer")
    for part in pointer.split("/")[1:]:
        part = part.replace("~1", "/").replace("~0", "~")
        value = value[int(part)] if isinstance(value, list) else value[part]
    return value


def verify_configure_command(command, profile, component, platform):
    if component == "icu":
        require(platform == "linux" and len(command) == 4 + len(profile) and
                Path(command[0]).name == "runConfigureICU" and command[1] == "Linux" and
                command[2].startswith("--prefix=") and command[2] != "--prefix=" and
                command[3] == "--libdir=" + command[2].removeprefix("--prefix=") + "/lib" and command[4:] == profile,
                "ICU command differs from the exact locked Linux shared-library configure profile")
        return
    if component == "pipewire":
        require(len(command) >= 6 and Path(command[0]).name in ("meson", "meson.py", "meson.exe") and
                command[1] == "setup" and command[4].startswith("--prefix=") and command[4] != "--prefix=" and
                command[5] == "--libdir=lib" and command[6:] == profile,
                "PipeWire command differs from the exact locked meson setup profile")
        return
    require(len(command) >= 2 and command[0].replace("\\", "/").rsplit("/", 1)[-1] == "configure" and
            command[1].startswith("--prefix=") and command[1] != "--prefix=",
            "Codec receipt must retain the actual source configure argument array")
    if component == "ffmpeg" and platform == "windows":
        expected = [*profile, "--extra-cflags=-MD", "--extra-cxxflags=-MD", "--extra-ldflags=-debug -opt:ref -opt:icf"]
        require(len(command) == len(expected) + 3 and command[2:-1] == expected and
                command[-1].startswith("--x86asmexe=") and command[-1] != "--x86asmexe=",
                "Windows FFmpeg command differs from the exact locked release CRT/profile")
    else:
        require(command[2:] == profile, "Codec command differs from the exact ordered locked configure profile")


def packaging_metadata(path=None):
    # Reuse the production PE/ELF/embedded identity and installed-tree checks.
    # This imports our tool's own helper, never code from an input upstream tree.
    path = path or Path(__file__).resolve().parents[1] / "build/package/metadata.py"
    module = types.ModuleType("huhucapture_source_package_metadata")
    module.__file__ = str(path)
    # Execute the retained helper bytes without generating timestamp-dependent
    # __pycache__ files inside the archive's otherwise immutable evidence tree.
    exec(compile(path.read_bytes(), str(path), "exec"), module.__dict__)
    return module


def replay_vendor(project, specification, expected, destination, toolchain):
    cargo, rustc = Path(specification["cargo"]).resolve(), Path(specification["rustc"]).resolve()
    cache = Path(specification["cache"]).resolve()
    environment = git_environment()
    # Run the explicit installed toolchain, not a rustup shim that might fetch it.
    for key in tuple(environment):
        if key.startswith("CARGO_") or key in ("RUSTC", "RUSTDOC", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER",
                "RUSTFLAGS", "RUSTUP_TOOLCHAIN", "SSH_ASKPASS", "SSH_ASKPASS_REQUIRE",
                "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY",
                "no_proxy", "NO_PROXY"):
            del environment[key]
    environment.update(CARGO_NET_OFFLINE="true", RUSTUP_AUTO_INSTALL="0", RUSTC=str(rustc),
                       CARGO_NET_GIT_FETCH_WITH_CLI="false", CARGO_HTTP_PROXY="")
    versions = {}
    rust_commit = None
    for label, program in (("cargo", cargo), ("rustc", rustc)):
        result = subprocess.run([str(program), "--version", "--verbose"], env=environment, check=True,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        require(result.stdout.startswith(label + " " + toolchain + " "), "Vendor replay uses the wrong Rust toolchain")
        versions[label] = {"version": result.stdout.strip(), "executable": descriptor(program)}
        if label == "rustc":
            matches = re.findall(r"(?m)^commit-hash: ([0-9a-f]{40})$", result.stdout)
            require(len(matches) == 1, "Rust toolchain does not report one exact source commit")
            rust_commit = matches[0]
    private_cache = destination / "cargo-home"
    for name in ("registry/cache", "registry/index", "git/db"):
        if (cache / name).is_dir():
            shutil.copytree(cache / name, private_cache / name, symlinks=False)
    environment.update(CARGO_HOME=str(private_cache), CARGO_TARGET_DIR=str(destination / "target"))
    for parent in (project, *project.parents):
        require(not any(os.path.lexists(parent / ".cargo" / name) for name in ("config", "config.toml")),
                "Offline replay cannot inherit unrelated Cargo configuration")
    output = destination / "vendor"
    command = [str(cargo), "vendor", "--locked", "--offline", "--versioned-dirs",
               "--manifest-path", str(project / "third_party/localsend/Cargo.toml"), str(output)]
    result = subprocess.run(command, cwd=project, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, "Offline vendor replay failed: " + result.stderr[-5000:])
    require(inventory(output) == expected, "Provided vendor differs from actual pinned-Cargo offline replay")
    def portable(text):
        return text.replace(str(project), "$SOURCE_ROOT").replace(str(destination), "$REPLAY_ROOT")
    return {"toolchain": toolchain, "rustSourceCommit": rust_commit, "tools": versions,
            "command": [portable(argument) for argument in command],
            "vendorTreeSha256": tree_digest(expected), "offline": True, "exitCode": result.returncode,
            "environment": {key: portable(environment[key]) for key in ("CARGO_HOME", "CARGO_TARGET_DIR",
                            "CARGO_NET_OFFLINE", "RUSTUP_AUTO_INSTALL", "CARGO_NET_GIT_FETCH_WITH_CLI", "CARGO_HTTP_PROXY")},
            "stdout": portable(result.stdout), "stderr": portable(result.stderr)}


class Collector:
    def __init__(self, plan, plan_directory, stage):
        self.plan, self.base, self.stage = plan, plan_directory, stage
        self.gaps, self.components, self.materials, self.receipts, self.source_transformations = [], {}, {}, {}, {}
        self.sources = stage / "sources"
        self.sources.mkdir()

    def path(self, value):
        require(isinstance(value, str) and value, "A real local input path is required")
        path = Path(value)
        return (self.base / path).resolve() if not path.is_absolute() else path.resolve()

    def gap(self, message):
        self.gaps.append(message)

    def retain(self, identifier, value):
        require(IDENTIFIER.fullmatch(identifier) and identifier not in self.materials, "Invalid or duplicate material identifier")
        path = self.path(value)
        actual = descriptor(path)
        relative_path = "materials/" + identifier + "/" + relative(actual["name"])
        target = self.stage / relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        require(not target.exists(), "Duplicate material destination")
        shutil.copyfile(path, target)
        target.chmod(0o644)
        require(descriptor(path) == actual and descriptor(target) == actual, "Material changed during collection")
        self.materials[identifier] = {**actual, "path": relative_path}
        return target

    def component(self, identifier, provenance, files, family=None, source_path=None):
        require(IDENTIFIER.fullmatch(identifier) and identifier not in self.components, "Duplicate or invalid component ID")
        root = source_path or self.sources / identifier
        licenses = license_files(root, files)
        if not licenses:
            self.gap(identifier + ": no retained license/copyright text")
        record = {"id": identifier, "name": family or ("qt" if identifier in
                  {"qtbase", "qtsvg", "qtwayland", "qttranslations"} else identifier),
                  "path": root.relative_to(self.stage).as_posix(), "source": provenance,
                  "sourceTreeSha256": tree_digest(files), "files": files, "licenses": licenses}
        self.components[identifier] = record
        return record

    def collect_project(self):
        project = self.path(self.plan["project"])
        self.project = project
        commit = git(project, "rev-parse", "HEAD").decode().strip()
        destination = self.sources / "huhucapture"
        provenance, files = export_git(project, commit, destination)
        self.component("huhucapture", provenance, files)
        self.exported_project = destination
        lock, self.pins = locked_sources(destination)
        self.lock = lock
        self.cargo_lock = destination / "third_party/localsend/Cargo.lock"
        require(digest(self.cargo_lock) == lock["qtMigration"]["nativeRust"]["cargoLockSha256"],
                "The aggregate Cargo.lock differs from the project dependency lock")
        version = read_json(destination / "build/version.json")["version"]
        require(re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version), "Invalid project version")
        self.identity = {"schemaVersion": 1, "product": "huhucapture", "repository": "NAMEWTA/huhucapture",
                         "version": version, "sourceCommit": commit}
        self.epoch = int(git(project, "show", "-s", "--format=%ct", commit).strip())
        require(self.plan.get("platform") in ("windows", "linux"), "Select the actual build platform")
        self.platform = self.plan["platform"]
        if self.plan.get("buildIdentity"):
            path = self.retain("build-identity", self.plan["buildIdentity"])
            self.build_identity = read_json(path)
            for name in ("product", "repository", "version", "sourceCommit"):
                require(self.build_identity.get(name) == self.identity[name], "Build identity differs from the checked-out project: " + name)
            require(self.build_identity.get("sourceDirty") is False and
                    self.build_identity.get("platform") == self.platform and
                    self.build_identity.get("qtVersion") == lock["qtMigration"]["qt"].get("version") and
                    self.build_identity.get("configuration") in ("Release", "RelWithDebInfo") and
                    self.build_identity.get("dependencyLockSha256") == digest(destination / "build/dependencies.lock.json") and
                    self.build_identity.get("cargoLockSha256") == digest(self.cargo_lock),
                    "Build identity is dirty, stale or uses different locked inputs")
        else:
            self.gap("huhucapture: actual matching build-identity.json is missing")
        local = destination / "third_party/localsend"
        upstream = read_json(local / "upstream.json")
        require(upstream["commit"] == lock["qtMigration"]["localsend"]["commit"], "LocalSend provenance differs from its pin")
        self.component("localsend", {"kind": "in-project", "projectCommit": commit,
                       "upstreamCommit": upstream["commit"], "adaptation": "third_party/localsend/patches/native-core.patch"},
                       inventory(local), source_path=local)

    def collect_source(self, item):
        identifier = item["id"]
        require(IDENTIFIER.fullmatch(identifier) and identifier not in self.components,
                "Duplicate or invalid source component")
        require("patchApplication" not in item, "A collection plan cannot override the locked patch application recipe")
        require("sourcePathProfile" not in item and "path_profile" not in item,
                "A collection plan cannot choose a source path exception")
        if item.get("kind") == "debian-source":
            return self.collect_debian_source(item)
        destination = self.sources / identifier
        pin = self.pins.get(identifier)
        header_profile = header_path_api()["source_profile"](identifier, pin or {}, self.platform)
        source_input = item.get("archive") if item.get("kind") == "tar" else item.get("directory")
        if not source_input or not self.path(source_input).exists():
            self.gap(identifier + ": real source checkout/archive is missing")
            return
        if not pin and not item.get("sha256" if item.get("kind") == "tar" else "commit"):
            self.gap(identifier + ": exact source pin is missing")
            return
        patches, patch_records = [], []
        specifications = pin.get("patches", []) if pin else item.get("patches", [])
        if pin:
            require("patches" not in item, "Locked patches cannot be overridden by a collection plan")
        for index, patch in enumerate(specifications):
            path = self.exported_project / relative(patch["path"]) if pin else self.path(patch["path"])
            require(digest(path) == patch["sha256"], "Source patch differs from its recorded SHA256")
            retained = self.retain(f"{identifier}-patch-{index}", str(path))
            require(digest(retained) == patch["sha256"], "Source patch changed while being retained")
            patches.append(retained)
            patch_records.append({"path": self.materials[f"{identifier}-patch-{index}"]["path"], "sha256": patch["sha256"]})
        if item.get("kind", "git") == "git":
            require(header_profile is None, "The Linux header path exception requires the exact original tar archive")
            require(not pin or "patchApplication" not in pin,
                    "The original toolchain patch recipe requires its exact source archive")
            commit = pin["commit"] if pin else item.get("commit")
            require(not pin or item.get("commit", commit) == commit, "Plan overrides a locked Git revision")
            provenance, files = export_git(self.path(item["directory"]), commit, destination, patches)
            provenance["repository"] = pin["repository"] if pin else item.get("repository")
            require(provenance["repository"], "Source Git repository provenance is missing")
            exact_pin = commit
        elif item.get("kind") == "tar":
            archive = self.path(item["archive"])
            expected = pin.get("sourceArchive", {}).get("sha256") if pin else item.get("sha256")
            require(expected and HEX64.fullmatch(expected) and digest(archive) == expected,
                    "Source archive has no matching exact source pin")
            retained = self.retain(identifier + "-original-archive", str(archive))
            require(digest(retained) == expected, "Source archive changed while being retained")
            files = extract_tar(retained, destination, item.get("prefix", ""), path_profile=header_profile)
            pristine = tree_digest(files)
            replay_material = None
            if pin and "patchApplication" in pin:
                helper = runpy.run_path(str(Path(__file__).resolve().parents[1] / "build/package/toolchain_patches.py"))
                actual = helper["apply"](identifier, destination, patches, pin, self.exported_project, globals())
                with tempfile.TemporaryDirectory(prefix="original-patch-replay-") as temporary:
                    record = Path(temporary) / "patch-replay.json"
                    record.write_bytes(canonical(actual))
                    self.retain(identifier + "-patch-replay", str(record))
                replay_material = self.materials[identifier + "-patch-replay"]
            else:
                apply_patches(destination, patches)
            files = inventory(destination, path_profile=header_profile)
            provenance = {"kind": "tar", "archiveSha256": expected,
                          "pristineTreeSha256": pristine, "sourceTreeSha256": tree_digest(files)}
            if replay_material:
                provenance["patchReplay"] = replay_material
            if header_profile:
                require(tree_digest(files) == pin["sourcePathProfile"]["sourceTreeSha256"],
                        "The complete original Linux header archive tree differs from its pin")
                provenance["sourcePathProfile"] = pin["sourcePathProfile"]
                self.retain("original-linux-header-path-policy",
                            str(Path(__file__).resolve().parents[1] / "build/package/linux_header_paths.py"))
            exact_pin = expected
        else:
            raise ValueError("Unsupported source type; supply real Git sources or a pinned source tar archive")
        provenance["patches"] = patch_records
        if not pin:
            evidence = item.get("pinEvidence")
            if not evidence:
                self.gap(identifier + ": independent exact source-pin provenance is missing")
            else:
                path = self.retain(identifier + "-pin-evidence", evidence["path"])
                require(digest(path) == evidence["sha256"], "Source pin evidence changed")
                actual = json_pointer(read_json(path), evidence["pointer"])
                require(actual == exact_pin or isinstance(actual, str) and actual.endswith("@" + exact_pin),
                        "Exact source pin is absent from the supplied provenance record")
                provenance["pinEvidence"] = self.materials[identifier + "-pin-evidence"]
        else:
            runtime_sources = read_json(self.sources / "huhucapture/build/package/dependencies.lock.json").get(
                "runtimeSourceBuild", {}).get("sources", {})
            provenance["lockFile"] = "sources/huhucapture/" + ("build/package/dependencies.lock.json"
                if identifier == "appimage-runtime" or identifier in runtime_sources else "build/dependencies.lock.json")
        if item.get("sourceSbom"):
            path = self.retain(identifier + "-source-sbom", item["sourceSbom"])
            verify_source_spdx(path, destination)
            provenance["sourceSbom"] = self.materials[identifier + "-source-sbom"]
        if not recognizable_source(identifier, pin, files):
            self.gap(identifier + ": archive/checkout has no recognizable native/module source files")
        self.component(identifier, provenance, files, None if identifier in COMMON | LINUX else item.get("family"))

    def collect_debian_source(self, item):
        identifier = item["id"]
        require(self.platform == "linux" and identifier not in self.pins and set(item) <=
                {"id", "kind", "sourcePackage", "sourceVersion", "dsc", "sha256", "sourceIndex",
                 "sourceIndexSha256", "family"},
                "Debian source material cannot override a locked component or supply unverified patches")
        helper_path = Path(__file__).resolve().parents[1] / "build/package/debian_sources.py"
        helper = types.ModuleType("huhucapture_debian_source_materials")
        helper.__file__ = str(helper_path)
        exec(compile(helper_path.read_bytes(), str(helper_path), "exec"), helper.__dict__)
        specification = {**item, "dsc": str(self.path(item["dsc"])),
                         "sourceIndex": str(self.path(item["sourceIndex"]))}
        destination = self.sources / identifier
        provenance, files, originals = helper.extract_source(specification, destination, inventory)
        retained = []
        for index, (path, expected) in enumerate(originals):
            material_id = identifier + "-debian-input-" + str(index)
            actual = self.retain(material_id, str(path))
            require(digest(actual) == expected["sha256"] and actual.stat().st_size == expected["size"],
                    "Original Debian source material changed during retention")
            retained.append(self.materials[material_id])
        provenance["retainedOriginals"] = retained
        # A Sources stanza and matching .dsc establish byte consistency. They
        # are not the original signed Release -> index chain. The producer's
        # APT execution is retained separately, without inventing an offline
        # archive signature or treating this extraction as a binary build.
        self.gap(identifier + ": original authenticated repository index/signature chain is missing")
        if not any(Path(entry["path"]).suffix.lower() in (".c", ".cc", ".cpp", ".h", ".hpp", ".rs", ".s")
                   for entry in files if entry["type"] == "file"):
            self.gap(identifier + ": Debian source package has no recognizable native source files")
        self.component(identifier, provenance, files, item.get("family"))

    def collect_rust(self):
        specification = self.plan.get("rustVendor")
        if not specification:
            self.gap("rust-vendor: full locked Cargo directory source and registry archives are missing")
            return
        require(self.lock["qtMigration"]["rustToolchain"] == CARGO_REGISTRY_VENDOR_TOOLCHAIN,
                "Registry vendor comparison is pinned to Cargo " + CARGO_REGISTRY_VENDOR_TOOLCHAIN +
                "; the locked toolchain needs its own verified comparison policy")
        vendor = self.path(specification["directory"])
        expected = inventory(vendor)
        git_sources = {value["source"]["commit"]: self.stage / value["path"] for value in self.components.values()
                       if value["source"]["kind"] == "git"}
        cache = self.path(specification["registryCache"]) if specification.get("registryCache") else None
        external_licenses = {}
        for package, reference in specification.get("licenseTexts", {}).items():
            component = self.components.get(reference["component"])
            if component is None:
                self.gap("rust-vendor:" + package + ": referenced license source component is missing")
                continue
            name = relative(reference["path"])
            require(name in component["licenses"], "External crate license is not a retained source license/copyright file")
            source = self.stage / component["path"] / name
            external_licenses[package] = {**reference, "sha256": digest(source)}
        records = validate_vendor(vendor, self.cargo_lock, cache, git_sources, self.gaps, external_licenses)
        destination = self.sources / "rust-vendor"
        copy_verified_tree(vendor, destination, expected)
        if specification.get("noticeReviews"):
            self.collect_rust_notices(destination, records, specification["noticeReviews"])
        provenance = {"kind": "cargo-vendor", "cargoLockSha256": digest(self.cargo_lock),
                      "packages": records, "toolchain": self.lock["qtMigration"]["rustToolchain"],
                      "registryComparisonPolicy": {"cargoVersion": CARGO_REGISTRY_VENDOR_TOOLCHAIN,
                                                   "cargoCommit": CARGO_REGISTRY_VENDOR_COMMIT,
                                                   "source": CARGO_REGISTRY_VENDOR_SOURCE}}
        archives = []
        if cache:
            for index, package in enumerate(records):
                if package["registryChecksum"]:
                    archive = cache / (package["name"] + "-" + package["version"] + ".crate")
                    if archive.is_file():
                        identifier = "registry-" + str(index)
                        self.retain(identifier, str(archive))
                        require(self.materials[identifier]["sha256"] == package["registryChecksum"],
                                "Registry source archive changed while being retained")
                        archives.append({"package": package["name"], "version": package["version"],
                                         "archive": self.materials[identifier]})
        provenance["registryArchives"] = archives
        replay = specification.get("replay")
        if replay:
            replay = {key: str(self.path(value)) for key, value in replay.items()}
            with tempfile.TemporaryDirectory(prefix="huhucapture-vendor-replay-") as temp:
                # The source export has no Cargo replacement file added yet.
                result = replay_vendor(self.exported_project, replay, expected, Path(temp), provenance["toolchain"])
            standard = self.components.get("rust-toolchain-source")
            if standard:
                if standard["source"]["kind"] == "git":
                    actual_commit = standard["source"]["commit"]
                else:
                    commit_file = self.stage / standard["path"] / "git-commit-hash"
                    actual_commit = commit_file.read_text().strip() if commit_file.is_file() else None
                require(actual_commit == result["rustSourceCommit"],
                        "Retained Rust standard-library source differs from the actual pinned compiler commit")
            path = self.stage / "vendor-replay.json"
            path.write_bytes(canonical(result))
            provenance["replay"] = {"path": "vendor-replay.json", "sha256": digest(path)}
            self.gaps = [gap for gap in self.gaps if not gap.endswith("requires a pinned-Cargo replay receipt")]
        else:
            self.gap("rust-vendor: actual offline replay with the pinned Cargo toolchain is missing")
        if specification.get("config"):
            path = self.retain("cargo-vendor-config-original", specification["config"])
            relocated = validate_cargo_config(path, records)
            # Keep generated replay configuration separate from the exact project
            # export. Rebuilding copies this one file into the documented location.
            (self.stage / "cargo-vendor-config.toml").write_bytes(relocated)
        else:
            self.gap("rust-vendor: original Cargo source-replacement configuration is missing")
        self.component("rust-vendor", provenance, expected)

    def collect_rust_notices(self, vendor, records, review_path):
        # The mature runtime-materials helper validates the reviewed original
        # notices against exact Cargo checksum inventories. Load our own retained
        # helper, never Python supplied by an input project or dependency.
        require(review_path == "third_party/rust-notices/upstream.json",
                "Rust notice reviews must use the committed project provenance file")
        helper_path = self.retain("rust-notice-helper", str(Path(__file__).resolve().parents[1] /
                                                         "build/package/rust_materials.py"))
        helper = types.ModuleType("huhucapture_source_rust_notices")
        helper.__file__ = str(helper_path)
        exec(compile(helper_path.read_bytes(), str(helper_path), "exec"), helper.__dict__)

        def unchanged(path):
            value = descriptor(path)
            return {key: value[key] for key in ("size", "sha256")}

        def copy_original(source, target):
            before = unchanged(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open("rb") as incoming, target.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing)
            target.chmod(0o644)
            require(unchanged(source) == before and unchanged(target) == before,
                    "Reviewed Rust notice changed while being retained")
            return before

        api = types.SimpleNamespace(ROOT=self.exported_project,
            meta=types.SimpleNamespace(require=require, safe_relative=relative,
                                       json_read=lambda path, limit=MAX_FILE: read_json(path, limit)),
            unchanged_digest=unchanged, copy_original=copy_original)
        path, identity, reviews = helper.original_notice_reviews(api)
        retained = self.retain("rust-notice-reviews", str(path))
        require(unchanged(retained) == identity, "Rust notice provenance changed while retaining")
        locked = {(p["name"], p["version"]): p for p in tomllib.loads(self.cargo_lock.read_text())["package"]
                  if p.get("source")}
        by_key = {(record["name"], record["version"]): record for record in records}
        for crate in sorted(vendor.iterdir()):
            package = tomllib.loads((crate / "Cargo.toml").read_text(encoding="utf-8"))["package"]
            key = package["name"], package["version"]
            review = reviews.get(key)
            if review is None:
                continue
            notices, provenance = helper.reviewed_crate_notices(api, crate, locked[key], package.get("license"),
                crate / ".cargo-checksum.json", review, identity, self.stage)
            require(notices, "An exact Rust notice review retained no notice bytes")
            by_key[key].update(supplementalNotices=notices, noticeProvenance=provenance)
            missing = f"rust-vendor:{key[0]}@{key[1]}: no retained license/copyright text"
            self.gaps = [gap for gap in self.gaps if gap != missing]

    def collect_receipts(self):
        for item in self.plan.get("buildReceipts", []):
            identifier = item["id"]
            require(identifier not in self.receipts, "Duplicate build receipt ID")
            path = self.retain("receipt-" + identifier, item["path"])
            value = read_json(path)
            require(value.get("schemaVersion") == 1 and value.get("kind") == "huhucapture-source-build-receipt" and
                    value.get("platform") == self.platform, "Unsupported or mismatching source build receipt")
            require(isinstance(value.get("commands"), list) and value["commands"] and
                    all(isinstance(command, list) and command and all(isinstance(arg, str) and arg for arg in command)
                        for command in value["commands"]), "Build receipt must retain actual argument arrays")
            require(isinstance(value.get("toolchain"), dict) and value["toolchain"] and
                    isinstance(value.get("environment"), dict), "Build receipt lacks actual toolchain/environment inputs")
            inputs = value.get("sources", [])
            require(inputs and len({source["id"] for source in inputs}) == len(inputs), "Receipt source identities are missing or duplicate")
            for source in inputs:
                component = self.components.get(source["id"])
                if component is None:
                    self.gap("receipt " + identifier + ": source component missing: " + source["id"])
                else:
                    require(source.get("sourceTreeSha256") == component["sourceTreeSha256"],
                            "Build receipt used different source bytes: " + source["id"])
            source_ids = {source["id"] for source in inputs}
            profiles = {}
            migration = self.lock["qtMigration"]
            if "ffmpeg" in source_ids:
                profiles["ffmpeg"] = (migration["videoInspectionWindows"] if self.platform == "windows"
                                      else migration["softwareMedia"]["ffmpeg"])["configure"]
            if "x264" in source_ids:
                profiles["x264"] = migration["softwareMedia"]["x264"]["configure"]
            if "pipewire" in source_ids:
                profiles["pipewire"] = migration["pipewire"]["mesonOptions"]
            if "icu" in source_ids and self.platform == "linux" and "icu" in migration.get("qt", {}).get("sourceDependencies", {}):
                profiles["icu"] = migration["qt"]["sourceDependencies"]["icu"]["configure"]
            for component, profile in profiles.items():
                index = value.get("profileCommands", {}).get(component)
                if index is None:
                    self.gap("receipt " + identifier + ": exact configure command binding is missing for " + component)
                    continue
                require(type(index) is int and 0 <= index < len(value["commands"]), "Invalid configure command index")
                selected = value["commands"][index]
                verify_configure_command(selected, profile, component, self.platform)
                for command in value["commands"]:
                    # A second command for this same configure program cannot
                    # silently undo the pinned flags after a matching command.
                    if command[0] == selected[0] and (component != "pipewire" or len(command) > 1 and command[1] == "setup"):
                        verify_configure_command(command, profile, component, self.platform)
            build_records = item.get("buildRecords", [])
            if not build_records:
                self.gap("receipt " + identifier + ": original configure/link/build evidence files are missing")
            retained_records = []
            for index, record in enumerate(build_records):
                record_path = self.retain("receipt-" + identifier + "-record-" + str(index), record["path"])
                require(digest(record_path) == record["sha256"], "Build evidence file changed after it was recorded")
                retained_records.append(self.materials["receipt-" + identifier + "-record-" + str(index)])
            require(isinstance(value.get("staticInputs"), list), "Build receipt must enumerate actual static link inputs")
            static_names = set()
            for static_input in value["staticInputs"]:
                name = static_input["id"]
                require(IDENTIFIER.fullmatch(name) and name not in static_names, "Duplicate or invalid static input identity")
                static_names.add(name)
                component = self.components.get(static_input["component"])
                require(HEX64.fullmatch(static_input.get("sha256", "")) and static_input.get("size", 0) > 0,
                        "Static link input lacks its real bytes/hash identity")
                if component is None:
                    self.gap("receipt " + identifier + ": static input source missing: " + static_input["component"])
                else:
                    require(static_input.get("sourceTreeSha256") == component["sourceTreeSha256"],
                            "Static link receipt source identity differs")
                actual_path = item.get("staticFiles", {}).get(name)
                if not actual_path or not self.path(actual_path).is_file():
                    self.gap("receipt " + identifier + ": actual linked static input is missing: " + name)
                else:
                    actual_file = descriptor(self.path(actual_path))
                    require(all(actual_file[key] == static_input[key] for key in ("sha256", "size")),
                            "Actual linked static input differs from its receipt: " + name)
            if "huhucapture" in source_ids and not any(part["component"] == "rust-vendor" for part in value["staticInputs"]):
                self.gap("receipt " + identifier + ": actual aggregated Rust static input/source binding is missing")
            if "huhucapture" in source_ids and not {"localsend", "gifski", "rust-vendor", "rust-toolchain-source"} <= source_ids:
                self.gap("receipt " + identifier + ": aggregated Rust local/Git/vendor source closure is missing")
            outputs = value.get("outputs", [])
            require(outputs and len({relative(output["path"]) for output in outputs}) == len(outputs),
                    "Build receipt outputs are missing or duplicate")
            for output in outputs:
                require(HEX64.fullmatch(output.get("sha256", "")) and isinstance(output.get("size"), int) and output["size"] > 0,
                        "Build receipt output lacks an exact file identity")
            self.receipts[identifier] = {**value, "retainedBuildRecords": retained_records}
        used = {source["id"] for receipt in self.receipts.values() for source in receipt["sources"]}
        for identifier in self.components:
            if identifier not in used and identifier != "localsend":
                self.gap(identifier + ": matching source/build-parameter receipt is missing")

    def collect_source_transformations(self):
        for item in self.plan.get("sourceTransformations", []):
            require(set(item) == {"id", "stage", "executionMaterial"} and
                    IDENTIFIER.fullmatch(item["id"]) and item["id"] not in self.source_transformations,
                    "Invalid or duplicate original source transformation specification")
            identifier, stage = item["id"], self.path(item["stage"])
            helper_path = Path(__file__).resolve().parent / "package-qt.py"
            helper = types.ModuleType("huhucapture_original_package_transformations")
            helper.__file__ = str(helper_path)
            scripts = str(helper_path.parent)
            if scripts not in sys.path:
                sys.path.insert(0, scripts)
            exec(compile(helper_path.read_bytes(), str(helper_path), "exec"), helper.__dict__)
            prepared = helper.verify_prepared(stage)
            require(prepared["build"]["platform"] == self.platform and all(
                    prepared["build"][key] == self.identity[key] for key in ("sourceCommit", "product", "repository", "version")),
                    "Original package transformations belong to another source/build identity")
            journal = read_json(stage / "transformations.json")
            execution = read_json(stage / "executions/build-execution.json")
            bound = helper.lineage_api().validate(stage, journal, execution, helper.source_inventory_api())
            project = {entry["path"]: entry for entry in self.components["huhucapture"]["files"]}
            for producer in bound["producerSources"]:
                source = project.get(producer["path"])
                require(source is not None and source["type"] == "file" and all(
                        source[key] == producer[key] for key in ("sha256", "size")),
                        "Actual transformation producer differs from the retained project source")
            material = self.materials.get(item["executionMaterial"])
            require(material is not None, "The original complete packaging execution archive is missing")
            with tempfile.TemporaryDirectory(prefix="original-package-execution-", dir=self.stage) as temporary:
                files = extract_tar(self.stage / material["path"], Path(temporary), "huhucapture-sources")
                require(files == journal["executionInventory"],
                        "Retained packaging execution archive differs from the original per-file evidence")
            originals = ["prepared.json", "transformations.json", "file-transformations.json", "installed-input-inventory.json"]
            originals += [entry["path"] for entry in journal["deploymentStages"]]
            retained = []
            for index, name in enumerate(originals):
                key = "transformation-" + identifier + "-record-" + str(index)
                self.retain(key, str(stage / relative(name)))
                retained.append(self.materials[key])
            self.source_transformations[identifier] = {**bound, "platform": self.platform,
                "sourceCommit": self.identity["sourceCommit"], "retainedRecords": retained,
                "executionArchive": material,
                "scope": "Observed final-file transformations; each mapping must retain its separately verified original source/build receipt"}

    def original_mapping(self, origin, entry):
        if set(origin) != {"transformation", "output", "input"}:
            return origin, entry
        name = entry["path"]
        require(origin["output"] == name and isinstance(origin["input"], dict) and
                set(origin["input"]) in ({"receipt", "output"}, {"component", "path"}),
                "Derived deployment mapping must retain exactly one original source/build origin")
        transformation = self.source_transformations.get(origin["transformation"])
        bound = transformation.get("origins", {}).get(name) if transformation else None
        if bound is None:
            self.gap("deployment: no unbroken original packaging transformation ancestry for " + name)
            return None, None
        require(all(bound["output"][key] == entry[key] for key in ("path", "size", "sha256")),
                "Derived deployed file differs from the actual final transformation output")
        return origin["input"], {"type": "file", **bound["input"]}

    def collect_deployment(self):
        specification = self.plan.get("deployment")
        if not specification:
            self.gap("deployment: final real installed tree, exact inventory and per-file source map are missing")
            return None
        directory = self.path(specification["directory"])
        actual = inventory(directory)
        require(actual, "Final deployment is empty; no installed product was supplied")
        default = "huhucapture.exe" if self.platform == "windows" else "bin/huhucapture"
        helper = self.materials.get("package-metadata-helper")
        meta = packaging_metadata(self.stage / helper["path"] if helper else None)
        products = specification.get("products")
        explicit_products = products is not None
        if explicit_products:
            require("executable" not in specification and isinstance(products, list) and 0 < len(products) <= 2,
                    "Deployment products must explicitly name one or two actual package roots")
            products = [dict(product) for product in products]
            roots, kinds = [], set()
            for product in products:
                require(set(product) == {"root", "executable", "packageKind"}, "Unknown deployment product fields")
                root = PurePosixPath(relative(product["root"]))
                require(product["executable"] == default, "Each product must use its canonical runtime executable path")
                allowed = {"portable-zip", "installer"} if self.platform == "windows" else {"appimage", "deb"}
                require(product["packageKind"] in allowed and product["packageKind"] not in kinds,
                        "Duplicate or platform-mismatched deployment package kind")
                require(not any(root == other or root.is_relative_to(other) or other.is_relative_to(root) for other in roots),
                        "Deployment product roots overlap")
                require(not any((directory / parent).is_symlink() for parent in (root, *root.parents)),
                        "Deployment product root traverses a symlink")
                roots.append(root)
                kinds.add(product["packageKind"])
        else:
            executable_name = relative(specification.get("executable", default))
            executable = directory / executable_name
            require(executable.name == Path(default).name, "Final deployment product executable has a noncanonical name")
            root = executable.parent if self.platform == "windows" else executable.parent.parent
            products = [{"root": root.relative_to(directory).as_posix(), "executable": default, "packageKind": None}]
        products.sort(key=lambda product: product["root"])
        executable_names = []
        observed_identity = getattr(self, "build_identity", None)
        for product in products:
            product_root = directory / product["root"]
            executable = product_root / product["executable"]
            require(executable.is_file() and not executable.is_symlink(), "Final deployment has no actual huhucapture executable")
            require(executable.resolve().is_relative_to(directory), "Deployment product executable escapes the payload root")
            executable_names.append(executable.relative_to(directory).as_posix())
            embedded = meta.binary_identity(executable)
            meta.identity(embedded)
            meta.verify_binary(executable, embedded)
            for key in ("product", "repository", "version", "sourceCommit"):
                require(embedded[key] == self.identity[key], "Installed executable belongs to a different source/build identity")
            require(embedded["platform"] == self.platform, "Installed executable uses a different platform")
            if observed_identity is not None:
                require(embedded == observed_identity, "Installed executable differs from its actual retained build identity")
            observed_identity = embedded
            sidecar = executable.with_name("build-identity.json")
            require(read_json(sidecar) == embedded, "Installed sidecar differs from the real executable identity")
            marker_path = executable.with_name("installation.json")
            if marker_path.is_file():
                marker = read_json(marker_path)
                if explicit_products:
                    require(marker.get("packageKind") == product["packageKind"],
                            "Product installation marker names a different package kind")
                else:
                    product["packageKind"] = marker.get("packageKind")
            try:
                meta.audit_tree(product_root, product["packageKind"])
            except (ValueError, KeyError) as error:
                self.gap("deployment: production installed-tree audit is incomplete for " + product["root"] + ": " + str(error))
        path = self.retain("deployment-inventory", specification["inventory"])
        require(read_json(path) == actual, "Actual final deployed files differ from their supplied inventory")
        path = self.retain("deployment-source-map", specification["sourceMap"])
        mapping = read_json(path)
        require(isinstance(mapping, dict), "Deployment source map must be an object keyed by exact relative path")
        require(set(mapping) <= {entry["path"] for entry in actual}, "Source map names a file absent from the final deployment")
        for executable_name in executable_names:
            application_mapping = mapping.get(executable_name, {})
            require(isinstance(application_mapping, dict) and
                    (not application_mapping or set(application_mapping) in
                        ({"receipt", "output"}, {"transformation", "output", "input"})),
                    "Product executable requires its original application receipt, directly or through recorded transformations")
            application_entry = next(entry for entry in actual if entry["path"] == executable_name)
            application_mapping, _ = self.original_mapping(application_mapping, application_entry)
            application_mapping = application_mapping or {}
            application_receipt = self.receipts.get(application_mapping.get("receipt"))
            if not application_receipt or not any(source["id"] == "huhucapture" for source in application_receipt["sources"]):
                self.gap("deployment: actual product executable is not bound to its application source build receipt: " + executable_name)
        for entry in actual:
            name = entry["path"]
            if name not in mapping:
                self.gap("deployment: no exact source mapping for " + name)
                continue
            origin = mapping[name]
            require(isinstance(origin, dict), "Deployment source mapping must be one typed object")
            if entry["type"] == "symlink":
                require(origin == {"linkTarget": entry["target"]}, "Deployment symlink mapping differs")
                require((directory / name).resolve().is_relative_to(directory) and (directory / name).resolve().is_file(),
                        "Deployment link is dangling or outside the real installed tree")
                continue
            require(set(origin) in ({"component", "path"}, {"receipt", "output"}, {"transformation", "output", "input"}),
                    "Deployment source mapping must use exactly one origin form")
            origin, bound_entry = self.original_mapping(origin, entry)
            if origin is None:
                continue
            if "component" in origin:
                with (directory / name).open("rb") as stream:
                    native = stream.read(4).startswith((b"MZ", b"\x7fELF"))
                if native:
                    self.gap("deployment: native file requires its actual original source build receipt: " + name)
                    continue
                component = self.components.get(origin["component"])
                if component is None:
                    self.gap("deployment: mapped source component missing for " + name)
                    continue
                matches = [item for item in component["files"] if item["path"] == relative(origin["path"])]
                require(len(matches) == 1 and matches[0]["type"] == "file" and
                        all(matches[0][key] == bound_entry[key] for key in ("sha256", "size")),
                        "Deployed copied file differs from its exact source: " + name)
            elif "receipt" in origin:
                receipt = self.receipts.get(origin["receipt"])
                if receipt is None:
                    self.gap("deployment: matching build receipt missing for " + name)
                    continue
                matches = [item for item in receipt["outputs"] if item["path"] == relative(origin["output"])]
                require(len(matches) == 1 and all(matches[0][key] == bound_entry[key] for key in ("sha256", "size")),
                        "Deployed binary/data file differs from its source build receipt: " + name)
            else:
                raise ValueError("Every deployed file needs real source bytes or a build output receipt: " + name)
        require(inventory(directory) == actual, "Final deployed tree changed during source mapping validation")
        return {"treeSha256": tree_digest(actual), "files": actual, "products": products}

    def archive_path_profile(self):
        helper = header_path_api()
        identifier = helper["COMPONENT"]
        component = self.components.get(identifier)
        if component is None:
            return None
        pin = self.pins[identifier]
        helper["source_profile"](identifier, pin, self.platform)
        require(component["path"] == "sources/" + identifier and
                component["source"].get("archiveSha256") == helper["ARCHIVE_SHA256"] and
                component["source"].get("sourcePathProfile") == helper["PROFILE_RECORD"] and
                helper["source_inventory"](identifier, pin, self.sources / identifier, globals()) == component["files"],
                "Source archiving requires the unchanged complete original Linux header component")
        return helper["BUNDLE"]

    def finish(self):
        require(self.plan.get("schemaVersion") == 1 and "complete" not in self.plan and
                "approved" not in self.plan, "Completeness is computed; a plan cannot assert approval/completeness")
        self.retain("source-collector", str(Path(__file__).resolve()))
        self.retain("package-metadata-helper", str(Path(__file__).resolve().parents[1] / "build/package/metadata.py"))
        self.collect_project()
        for item in self.plan.get("sources", []):
            self.collect_source(item)
        self.collect_rust()
        for item in self.plan.get("materials", []):
            self.retain(item["id"], item["path"])
        self.collect_receipts()
        self.collect_source_transformations()
        deployment = self.collect_deployment()
        for identifier in sorted((COMMON | (LINUX if self.platform == "linux" else set())) - set(self.components)):
            self.gap(identifier + ": required source component is missing")
        if self.platform == "linux" and "appimage-runtime" in self.components:
            if not appimage_static_inputs_present(self.receipts):
                self.gap("appimage-runtime: matching built runtime/static-input closure receipt is missing")
        # A final comparison detects concurrent tracked source changes after the
        # initial export, without refreshing or changing the caller's Git index.
        project = self.components["huhucapture"]
        compare_checkout(self.project, project["files"], self.identity["sourceCommit"])
        report = {**self.identity, "generator": "scripts/create-source-bundle.py", "platform": self.platform,
                  "architecture": "x64" if self.platform == "windows" else "amd64",
                  "complete": not self.gaps, "gaps": sorted(set(self.gaps)),
                  "components": list(self.components.values()), "materials": self.materials,
                  "buildReceipts": self.receipts, "sourceTransformations": self.source_transformations, "deployment": deployment,
                  "completionScope": "All recorded source/build/deployment inputs verified; independent distribution review remains required"}
        (self.stage / "source-inventory.json").write_bytes(canonical(report))
        return report


def verify_source_spdx(path, root):
    # Qt's retained source SBOM uses SPDX tag/value with FileName/SHA1 pairs.
    # It supplies byte evidence, not an invented Git revision.
    text = path.read_text(encoding="utf-8")
    require(text.startswith("SPDXVersion:"), "Unsupported source SBOM; supply Qt's tag/value source SPDX")
    names = set()
    for block in re.split(r"(?m)^FileName: ", text)[1:]:
        raw, _, remainder = block.partition("\n")
        name = relative(raw.removeprefix("./"))
        require(name not in names, "Duplicate source SPDX file")
        names.add(name)
        matches = re.findall(r"(?m)^FileChecksum: SHA1: ([0-9a-f]{40})$", remainder)
        require(len(matches) == 1 and (root / name).is_file() and not (root / name).is_symlink() and
                digest(root / name, "sha1") == matches[0], "Source SPDX byte mismatch: " + name)
    require(names, "Source SPDX contains no file checksums")
    return len(names)


def collect(plan_path, output):
    plan_path, output = Path(plan_path).resolve(), Path(output).resolve()
    plan = read_json(plan_path)
    require(not output.exists(), "Use a new output directory; existing results are never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="huhucapture-sources-", dir=output.parent) as temporary:
        stage = Path(temporary) / "bundle"
        stage.mkdir()
        collector = Collector(plan, plan_path.parent, stage)
        report = collector.finish()
        output.mkdir()
        archive_name, manifest_name = source_output_names(report["version"], report["platform"])
        archive = output / archive_name
        report["archive"] = make_archive(stage, archive, collector.epoch, path_profile=collector.archive_path_profile())
        report["sourceInventory"] = {"path": "huhucapture-sources/source-inventory.json",
                                     "sha256": digest(stage / "source-inventory.json")}
        (output / manifest_name).write_bytes(canonical(report))
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("collect", help="Collect supplied real inputs; missing material produces an incomplete report")
    create.add_argument("--plan", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--require-complete", action="store_true", help="Fail after retaining the report when actual inputs are incomplete")
    listing = commands.add_parser("inventory", help="Hash every actual file/link in a source or final deployment tree")
    listing.add_argument("--directory", type=Path, required=True)
    listing.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "inventory":
            with args.output.open("xb") as stream:
                stream.write(canonical(inventory(args.directory)))
            return 0
        result = collect(args.plan, args.output)
        print(json.dumps({key: result[key] for key in ("complete", "archive", "gaps")}, indent=2, ensure_ascii=False))
        return 2 if args.require_complete and not result["complete"] else 0
    except (ValueError, KeyError, OSError, subprocess.SubprocessError, tarfile.TarError, tomllib.TOMLDecodeError) as error:
        print("Source collection failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
