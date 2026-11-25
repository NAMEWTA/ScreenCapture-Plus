#!/usr/bin/env python3
"""Retain the existing Linux application CI sources as an explicitly incomplete bundle."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import tomllib
import types

ROOT = Path(__file__).resolve().parents[1]
COLLECTOR = ROOT / "scripts/create-source-bundle.py"
BUNDLE = types.ModuleType("huhucapture_source_bundle")
BUNDLE.__file__ = str(COLLECTOR)
# Reuse the collector's checked Git/object/file operations without writing a
# Python cache into the source checkout. Collection itself still uses its CLI.
exec(compile(COLLECTOR.read_bytes(), str(COLLECTOR), "exec"), BUNDLE.__dict__)
TOOLCHAIN = "1.97.1"
REGISTRY = "registry+https://github.com/rust-lang/crates.io-index"
NATIVE_SOURCES = {
    "libportal": ".deps/portal-validation/source",
    "pipewire": ".deps/pipewire-validation/source",
    "x264": ".deps/media-validation/x264-source",
    "ffmpeg": ".deps/media-validation/ffmpeg-source",
}


def package_key(package):
    return package["name"], package["version"], package.get("source") or ""


def package_identity(key):
    return {"name": key[0], "version": key[1], "source": key[2] or None}


def metadata_coverage(packages, metadata):
    """Compare the real resolution with the whole lock, without inferring a cache."""
    locked = {package_key(item) for item in packages}
    resolved = {package_key(item) for item in metadata["packages"]}
    BUNDLE.require(len(locked) == len(packages) and
                   len(resolved) == len(metadata["packages"]), "Duplicate Cargo package identity")
    BUNDLE.require(resolved <= locked, "Actual Cargo metadata contains packages outside Cargo.lock")
    return {
        "lockedPackages": [package_identity(item) for item in sorted(locked)],
        "metadataPackages": [package_identity(item) for item in sorted(resolved)],
        "lockedAbsentFromMetadata": [package_identity(item) for item in sorted(locked - resolved)],
    }


def registry_layout(metadata):
    """Use the build's actual paths; never guess a runner/cache hash."""
    locations = set()
    for package in metadata["packages"]:
        source = package.get("source") or ""
        if not source.startswith("registry+"):
            continue
        BUNDLE.require(source == REGISTRY, "This Linux CI profile supports only its locked public registry")
        manifest = Path(package["manifest_path"])
        BUNDLE.require(manifest.is_absolute() and len(manifest.parents) >= 5 and
                       manifest.name == "Cargo.toml" and manifest.parents[2].name == "src" and
                       manifest.parents[3].name == "registry", "Unexpected actual Cargo registry path")
        locations.add((manifest.parents[4], manifest.parents[3] / "cache" / manifest.parents[1].name))
    BUNDLE.require(len(locations) == 1, "Expected the one actual public registry cache used by this build")
    return next(iter(locations))


def write_new(path, value):
    with path.open("xb") as stream:
        stream.write(BUNDLE.canonical(value))


def snapshot(source, target):
    identity = BUNDLE.descriptor(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    # A failed copy/check never publishes an unverified .crate in the retained
    # directory. Only this invocation's private temporary file is removed.
    with tempfile.TemporaryDirectory(prefix="source-material-check-", dir=ROOT / "artifacts") as temporary:
        prepared = Path(temporary) / target.name
        with source.open("rb") as incoming, prepared.open("xb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)
        prepared.chmod(0o644)
        BUNDLE.require(BUNDLE.descriptor(source) == identity and BUNDLE.descriptor(prepared) == identity,
                       "Input changed while being retained: " + str(source))
        os.link(prepared, target)  # Atomic publication; never replace an existing retained file.
    return identity


class Retention:
    def __init__(self, output, commit, qt_prefix):
        self.output, self.commit, self.qt_prefix = output, commit, qt_prefix
        self.materials = []
        self.report = {"schemaVersion": 1, "kind": "huhucapture-linux-development-source-retention",
                       "sourceCommit": commit, "platform": "linux", "profile": "existing-application-build",
                       "commands": [], "retainedMaterials": [], "status": "started"}
        self.coverage = {"schemaVersion": 1, "sourceCommit": commit, "platform": "linux",
                         "registryArchives": [], "missingRegistryArchives": [], "missingMaterials": []}
        (output / "logs").mkdir()
        (output / "inputs/registry-cache").mkdir(parents=True)

    def material(self, identifier, source):
        source = Path(source)
        BUNDLE.require(BUNDLE.IDENTIFIER.fullmatch(identifier), "Invalid fixed material ID")
        BUNDLE.relative(source.name)
        target = self.output / "materials" / identifier / source.name
        identity = snapshot(source, target)
        self.materials.append({"id": identifier, "path": target.relative_to(self.output).as_posix()})
        self.report["retainedMaterials"].append({"id": identifier, "input": str(source),
                                               "retained": self.materials[-1]["path"], **identity})
        return target

    def command(self, identifier, arguments, *, environment=None, checked=True, stdout_path=None):
        stdout_path = stdout_path or self.output / "logs" / (identifier + ".stdout.log")
        stderr_path = self.output / "logs" / (identifier + ".stderr.log")
        record = {"id": identifier, "arguments": [str(item) for item in arguments], "cwd": str(ROOT),
                  "stdout": stdout_path.relative_to(self.output).as_posix(),
                  "stderr": stderr_path.relative_to(self.output).as_posix()}
        self.report["commands"].append(record)
        with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
            try:
                result = subprocess.run(record["arguments"], cwd=ROOT, env=environment,
                                        stdout=stdout, stderr=stderr)
            except OSError as error:
                record.update(exitCode=None, launchError=str(error))
                raise
        record["exitCode"] = result.returncode
        if checked:
            BUNDLE.require(result.returncode == 0,
                           identifier + " failed; retained stderr: " + str(stderr_path))
        return record

    def clean_project(self):
        actual = BUNDLE.git(ROOT, "rev-parse", "HEAD").decode().strip()
        self.report["actualSourceCommit"] = actual
        BUNDLE.require(actual == self.commit,
                       "--source-commit must be the actual checked-out commit, including a PR merge checkout")
        BUNDLE.require(not BUNDLE.git(ROOT, "diff", "--raw", "--no-ext-diff", "--no-textconv", "HEAD", "--") and
                       not BUNDLE.git(ROOT, "ls-files", "--others", "--exclude-standard", "-z"),
                       "Source retention requires the committed clean CI checkout")
        recorded = BUNDLE.git(ROOT, "cat-file", "blob", self.commit + ":scripts/retain-development-sources.py")
        BUNDLE.require(recorded == Path(__file__).read_bytes(), "The running profile is not in this exact commit")

    def native_materials(self, lock):
        migration = lock["qtMigration"]
        for name, location in NATIVE_SOURCES.items():
            BUNDLE.require((ROOT / location).is_dir(), "Actual built native source is missing: " + location)
        for name, workspace, specification in (
            ("portal", "portal-validation", migration["libportal"]),
            ("pipewire", "pipewire-validation", migration["pipewire"]),
        ):
            value = BUNDLE.read_json(self.material(name + "-build", ROOT / ".deps" / workspace / "build-info.json"))
            for key in ("repository", "commit", "patches"):
                BUNDLE.require(value[key] == specification[key], "Actual native build differs from its lock: " + name)
            if name == "portal":
                BUNDLE.require(value["qtVersion"] == migration["qt"]["version"], "Portal used another Qt version")
            else:
                BUNDLE.require(value["mesonOptions"] == specification["mesonOptions"], "PipeWire configuration differs")
        media = BUNDLE.read_json(self.material("software-media-build", ROOT / ".deps/media-validation/build-info.json"))
        for name in ("x264", "ffmpeg"):
            actual, expected = media["components"][name], migration["softwareMedia"][name]
            for key in ("repository", "commit"):
                BUNDLE.require(actual[key] == expected[key], "Actual codec build differs from its lock: " + name)
            BUNDLE.require(actual["patches"] == expected.get("patches", []) and actual["configure"] ==
                           [str(ROOT / NATIVE_SOURCES[name] / "configure"),
                            "--prefix=" + str(ROOT / ".deps/media-validation/install"), *expected["configure"]],
                           "Actual codec patches/configuration differ: " + name)

    def rust_materials(self, lock, cargo_lock):
        actual = BUNDLE.read_json(self.material("native-rust-build", ROOT / "artifacts/native-rust/build-info.json"))
        metadata = BUNDLE.read_json(self.material("native-rust-metadata", ROOT / "artifacts/native-rust/cargo-metadata.json"))
        self.material("native-rust-log", ROOT / "artifacts/native-rust/build.log")
        BUNDLE.require(actual["cargoLockSha256"] == BUNDLE.digest(cargo_lock) ==
                       lock["qtMigration"]["nativeRust"]["cargoLockSha256"], "Actual Rust build used another Cargo.lock")
        BUNDLE.require(actual["rustVersion"] == TOOLCHAIN and actual["host"] == "x86_64-unknown-linux-gnu" and
                       actual["bootstrapResolution"] is False, "Expected the existing locked Linux release Rust build")
        for name, digest in actual["sourceFiles"].items():
            path = ROOT / BUNDLE.relative(name)
            BUNDLE.require(path.is_file() and not path.is_symlink() and BUNDLE.digest(path) == digest,
                           "Rust source changed since its actual build: " + name)
        library = Path(actual["library"])
        BUNDLE.require(library.is_absolute() and library.resolve().is_relative_to(ROOT / "artifacts"),
                       "Actual native library is outside the fixed CI artifact tree")
        self.coverage["actualNativeLibrary"] = {"path": str(library), **BUNDLE.descriptor(library)}
        BUNDLE.require(self.coverage["actualNativeLibrary"]["sha256"] == actual["librarySha256"],
                       "Actual Rust library no longer matches its build record")
        return metadata

    def registry_archives(self, packages, cache):
        for package in sorted(packages, key=package_key):
            source = package.get("source") or ""
            if not source.startswith("registry+"):
                continue
            BUNDLE.require(source == REGISTRY, "Unexpected private/additional Cargo registry")
            name = BUNDLE.relative(package["name"] + "-" + package["version"] + ".crate")
            original = cache / name
            record = {**package_identity(package_key(package)), "lockedSha256": package["checksum"]}
            if not original.exists():
                self.coverage["missingRegistryArchives"].append(record)
                continue
            BUNDLE.require(BUNDLE.HEX64.fullmatch(package["checksum"] or ""), "Invalid locked registry checksum")
            actual = BUNDLE.descriptor(original)
            if actual["sha256"] != package["checksum"]:
                self.coverage["invalidRegistryArchive"] = {**record, "actual": actual, "input": str(original)}
                raise ValueError("Existing registry archive differs from Cargo.lock: " + name)
            target = self.output / "inputs/registry-cache" / name
            retained = snapshot(original, target)
            BUNDLE.require(retained == actual, "Registry archive changed after checksum validation")
            self.coverage["registryArchives"].append({**record, "archive": retained,
                                                       "retained": target.relative_to(self.output).as_posix()})

    def gifski_checkout(self, lock, metadata, cargo_home):
        pin = lock["gifski"]
        expected_source = "git+" + pin["repository"] + "?rev=" + pin["commit"] + "#" + pin["commit"]
        choices = [item for item in metadata["packages"] if (item.get("source") or "").startswith("git+")]
        BUNDLE.require(len(choices) == 1 and choices[0]["name"] == "gifski" and
                       choices[0]["version"] == pin["version"] and choices[0]["source"] == expected_source,
                       "Expected the actual single locked gifski Git dependency")
        source = Path(choices[0]["manifest_path"]).parent.resolve(strict=True)
        BUNDLE.require(source.is_relative_to(cargo_home / "git/checkouts"), "Unexpected actual gifski checkout location")
        destination = self.output / "inputs/gifski"
        with tempfile.TemporaryDirectory(prefix="source-gifski-check-", dir=ROOT / "artifacts") as temporary:
            exported = Path(temporary) / "source"
            BUNDLE.export_git_objects(source, pin["commit"], exported)
            expected = BUNDLE.inventory(exported)
            actual_expected = list(expected)
            marker = source / ".cargo-ok"
            if os.path.lexists(marker) and not any(item["path"] == ".cargo-ok" for item in expected):
                BUNDLE.require(marker.is_file() and not marker.is_symlink(), "Invalid Cargo checkout completion marker")
                actual_expected.append({"path": ".cargo-ok", "type": "file", "size": marker.stat().st_size,
                                        "sha256": BUNDLE.digest(marker),
                                        "mode": 0o755 if marker.stat().st_mode & 0o111 else 0o644})
                self.coverage["cargoCheckoutMarker"] = actual_expected[-1]
            BUNDLE.compare_checkout(source, actual_expected, pin["commit"])
            # Only this verified local checkout supplies objects. All Git calls
            # reuse the collector's no-hooks/no-fsmonitor/no-lazy-fetch policy.
            BUNDLE.git(ROOT, "-c", "protocol.file.allow=always", "clone", "--local", "--no-hardlinks",
                       "--dissociate", "--no-checkout", str(source), str(destination))
            BUNDLE.git(destination, "-c", "core.autocrlf=false", "-c", "core.attributesFile=" + os.devnull,
                       "checkout", "--detach", pin["commit"])
            BUNDLE.compare_checkout(destination, expected, pin["commit"])
            BUNDLE.compare_checkout(source, actual_expected, pin["commit"])
        self.coverage["gifski"] = {"source": expected_source, "input": str(source),
                                   "retained": "inputs/gifski", "sourceTreeSha256": BUNDLE.tree_digest(expected)}
        return source

    def offline_vendor(self, cargo_home, cache, gifski_source):
        environment = BUNDLE.git_environment()
        environment.update(CARGO_NET_OFFLINE="true", RUSTUP_AUTO_INSTALL="0")
        for key in ("RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS",
                    "CARGO_TARGET_DIR", "RUSTUP_TOOLCHAIN"):
            environment.pop(key, None)
        executables = {}
        for program in ("cargo", "rustc"):
            record = self.command(program + "-path", ["rustup", "which", "--toolchain", TOOLCHAIN, program],
                                  environment=environment)
            executable = Path((self.output / record["stdout"]).read_text().strip()).resolve(strict=True)
            version = self.command(program + "-version", [str(executable), "--version", "--verbose"],
                                   environment=environment)
            text = (self.output / version["stdout"]).read_text()
            BUNDLE.require(text.startswith(program + " " + TOOLCHAIN + " "), "The installed toolchain differs from the lock")
            executables[program] = str(executable)
            self.coverage[program] = {"version": text.strip(), "executable": str(executable), **BUNDLE.descriptor(executable)}
        environment["RUSTC"] = executables["rustc"]
        # Only a private, disposable cache is writable by Cargo. Never copy a
        # Cargo home/config/credentials, or retain unrelated registry archives.
        with tempfile.TemporaryDirectory(prefix="source-vendor-cache-", dir=ROOT / "artifacts") as temporary:
            private = Path(temporary)
            index = cargo_home / "registry/index" / cache.name
            if index.is_dir():
                shutil.copytree(index, private / "registry/index" / cache.name, symlinks=False)
            database = cargo_home / "git/db" / gifski_source.parent.name
            if database.is_dir():
                commit = self.coverage["gifski"]["source"].rsplit("#", 1)[-1]
                raw = BUNDLE.git(database, "cat-file", "commit", commit)
                BUNDLE.require(BUNDLE.object_digest("commit", raw) == commit, "Cached gifski Git database differs from its pin")
                shutil.copytree(database, private / "git/db" / database.name, symlinks=False)
            shutil.copytree(self.output / "inputs/registry-cache", private / "registry/cache" / cache.name)
            environment.update(CARGO_HOME=str(private), CARGO_TARGET_DIR=str(private / "target"))
            vendor = self.output / "inputs/rust-vendor"
            record = self.command("cargo-vendor", [executables["cargo"], "vendor", "--locked", "--offline",
                                  "--versioned-dirs", "--manifest-path", str(ROOT / "third_party/localsend/Cargo.toml"),
                                  str(vendor)], environment=environment, checked=False,
                                  stdout_path=self.output / "vendor-config.toml")
        self.coverage["vendor"] = record
        if record["exitCode"] != 0:
            self.coverage["vendorPackages"] = None
            return None
        packages = []
        for directory in sorted(vendor.iterdir()):
            BUNDLE.require(directory.is_dir() and not directory.is_symlink(), "Invalid actual Cargo vendor output")
            package = tomllib.loads((directory / "Cargo.toml").read_text(encoding="utf-8"))["package"]
            packages.append({"name": package["name"], "version": package["version"]})
        self.coverage["vendorPackages"] = packages
        return {"directory": "inputs/rust-vendor", "registryCache": "inputs/registry-cache",
                "config": "vendor-config.toml"}

    def run(self):
        self.clean_project()
        lock = BUNDLE.read_json(ROOT / "build/dependencies.lock.json")
        BUNDLE.require(lock["qtMigration"]["rustToolchain"] == TOOLCHAIN, "This fixed CI profile needs an explicit toolchain update")
        cargo_lock = ROOT / "third_party/localsend/Cargo.lock"
        packages = tomllib.loads(cargo_lock.read_text(encoding="utf-8"))["package"]
        self.coverage.update(dependencyLockSha256=BUNDLE.digest(ROOT / "build/dependencies.lock.json"),
                             cargoLockSha256=BUNDLE.digest(cargo_lock))
        self.native_materials(lock)
        metadata = self.rust_materials(lock, cargo_lock)
        self.coverage.update(metadata_coverage(packages, metadata))
        cargo_home, cache = registry_layout(metadata)
        self.coverage.update(actualCargoHome=str(cargo_home), actualRegistryCache=str(cache))
        self.registry_archives(packages, cache)
        gifski_source = self.gifski_checkout(lock, metadata, cargo_home)
        sbom = self.qt_prefix / "sbom"
        files = sorted(path for path in sbom.rglob("*") if path.is_file()) if sbom.is_dir() else []
        if not files:
            self.coverage["missingMaterials"].append("The supplied actual Qt prefix has no SBOM files")
        self.coverage["qtPrefix"] = str(self.qt_prefix)
        for index, path in enumerate(files):
            self.material("qt-sbom-" + str(index), path)
        vendor = self.offline_vendor(cargo_home, cache, gifski_source)
        self.clean_project()
        write_new(self.output / "cargo-coverage.json", self.coverage)
        self.materials.append({"id": "cargo-coverage", "path": "cargo-coverage.json"})
        # Original archives already included by collect_rust are not duplicated
        # in the bundle. Others remain useful raw source material on either path.
        vendored = {(item["name"], item["version"]) for item in self.coverage["vendorPackages"] or []}
        for index, item in enumerate(self.coverage["registryArchives"]):
            if (item["name"], item["version"]) not in vendored:
                self.materials.append({"id": "cached-registry-" + str(index), "path": item["retained"]})
        for index, path in enumerate(sorted((self.output / "logs").iterdir())):
            if path.is_file() and path.stat().st_size:
                self.materials.append({"id": "profile-log-" + str(index), "path": path.relative_to(self.output).as_posix()})
        plan = {"schemaVersion": 1, "platform": "linux", "project": str(ROOT),
                "sources": [{"id": name, "directory": str(ROOT / location)} for name, location in NATIVE_SOURCES.items()] +
                           [{"id": "gifski", "directory": "inputs/gifski"}],
                "materials": self.materials}
        if vendor is not None:
            plan["rustVendor"] = vendor
        write_new(self.output / "collection-plan.json", plan)
        collector = self.command("source-collector", [sys.executable, str(COLLECTOR), "collect", "--plan",
                                 str(self.output / "collection-plan.json"), "--output", str(self.output / "bundle")],
                                 checked=False)
        self.report["collectorExitCode"] = collector["exitCode"]
        BUNDLE.require(collector["exitCode"] == 0, "Collector rejected the actual inputs; verified materials and logs are retained")
        manifest = BUNDLE.read_json(self.output / "bundle/source-manifest.json")
        BUNDLE.require(manifest["sourceCommit"] == self.commit and manifest["complete"] is False,
                       "Unexpected source identity/completeness from this intentionally incomplete profile")
        provided = {item["id"] for item in manifest["components"]}
        BUNDLE.require(set(NATIVE_SOURCES) | {"huhucapture", "localsend", "gifski"} <= provided,
                       "A required actual source disappeared during collection")
        self.clean_project()
        self.report.update(status="incomplete-bundle-retained", complete=False,
                           archive=manifest["archive"], gaps=manifest["gaps"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-prefix", type=Path, required=True, help="Actual QT_ROOT_DIR from this Linux CI job")
    parser.add_argument("--source-commit", required=True, help="Actual checkout SHA, including the PR merge SHA")
    parser.add_argument("--output", type=Path, required=True, help="New directory under this checkout's ignored artifacts/")
    args = parser.parse_args()
    retention = None
    try:
        BUNDLE.require(platform.system() == "Linux" and platform.machine() in ("x86_64", "amd64"),
                       "This profile is only for the existing Linux amd64 full-application CI job")
        BUNDLE.require(BUNDLE.HEX40.fullmatch(args.source_commit or ""), "An exact actual checkout SHA is required")
        output = args.output.resolve()
        BUNDLE.require(output.is_relative_to(ROOT / "artifacts") and output != ROOT / "artifacts" and
                       not os.path.lexists(args.output) and not output.exists(),
                       "Use a new output directory below the checkout's ignored artifacts/")
        qt = args.qt_prefix.resolve(strict=True)
        BUNDLE.require(qt.is_dir(), "Actual Qt prefix is missing")
        output.mkdir(parents=True)
        retention = Retention(output, args.source_commit, qt)
        retention.run()
        print(json.dumps({"output": str(output), "complete": False, "archive": retention.report["archive"]}, indent=2))
        return 0
    except (ValueError, KeyError, OSError, RuntimeError, UnicodeError, subprocess.SubprocessError,
            tomllib.TOMLDecodeError) as error:
        if retention:
            retention.report.update(status="failed", error=str(error))
        print("Development source retention failed: " + str(error), file=sys.stderr)
        return 1
    finally:
        if retention:
            if not (retention.output / "cargo-coverage.json").exists():
                write_new(retention.output / "cargo-coverage.json", retention.coverage)
            write_new(retention.output / "retention-report.json", retention.report)


if __name__ == "__main__":
    raise SystemExit(main())
