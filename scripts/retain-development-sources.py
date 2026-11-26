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
RUST_MATERIALS = types.ModuleType("huhucapture_retained_rust_graph")
_rust_graph_path = ROOT / "build/package/rust_materials.py"
exec(compile(_rust_graph_path.read_bytes(), str(_rust_graph_path), "exec"), RUST_MATERIALS.__dict__)
GRAPH_API = types.SimpleNamespace(meta=types.SimpleNamespace(require=BUNDLE.require))
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


def cargo_environment(private):
    """The fixed public-source profile does not inherit Cargo credentials/config."""
    environment = BUNDLE.git_environment()
    for key in tuple(environment):
        if key.startswith("CARGO_") or key in (
            "RUSTC", "RUSTDOC", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "RUSTFLAGS",
            "CARGO_ENCODED_RUSTFLAGS", "RUSTUP_TOOLCHAIN", "SSH_ASKPASS", "SSH_ASKPASS_REQUIRE",
            "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY",
            "no_proxy", "NO_PROXY",
        ):
            del environment[key]
    environment.update(CARGO_HOME=str(private), CARGO_TARGET_DIR=str(private / "target"),
                       CARGO_NET_OFFLINE="true", CARGO_NET_GIT_FETCH_WITH_CLI="false", RUSTUP_AUTO_INSTALL="0",
                       CARGO_HTTP_PROXY="")
    # In the fixed Cargo, an explicit empty HTTP proxy bypasses libgit2's
    # Config::open_default fallback and tells libcurl to use no proxy. Git CLI
    # configuration environment variables alone cannot isolate that fallback.
    # Cargo searches .cargo in the working directory and every ancestor, even
    # with a private CARGO_HOME. This fixed checkout has no such configuration.
    for directory in (ROOT, *ROOT.parents):
        for name in ("config", "config.toml"):
            BUNDLE.require(not os.path.lexists(directory / ".cargo" / name),
                           "The fixed source profile cannot inherit Cargo configuration: " + str(directory / ".cargo" / name))
    return environment


def retain_replay_inputs(source, destination):
    """Keep only this task's public source cache for later real offline replay."""
    BUNDLE.require(not destination.exists(), 'Replay source inputs already exist')
    destination.mkdir(parents=True)
    retained = {}
    for name in ('registry/cache', 'registry/index', 'git/db'):
        original = source / name
        if not original.is_dir():
            continue
        expected = BUNDLE.inventory(original)
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(original, target, symlinks=False)
        BUNDLE.require(BUNDLE.inventory(original) == expected and BUNDLE.inventory(target) == expected,
                       'Public replay source cache changed during retention: ' + name)
        retained[name] = {'treeSha256': BUNDLE.tree_digest(expected), 'files': len(expected)}
    BUNDLE.require(set(retained) == {'registry/cache', 'registry/index', 'git/db'},
                   'Actual offline replay inputs are incomplete')
    return retained


class Retention:
    def __init__(self, output, commit, qt_prefix, *, fetch_locked_sources=False, retain_replay_cache=False):
        self.output, self.commit, self.qt_prefix = output, commit, qt_prefix
        self.fetch_locked_sources = fetch_locked_sources
        self.retain_replay_cache = retain_replay_cache
        self.materials = []
        self.build_receipts = []
        self.report = {"schemaVersion": 1, "kind": "huhucapture-linux-development-source-retention",
                       "sourceCommit": commit, "platform": "linux", "profile": "existing-application-build",
                       "commands": [], "retainedMaterials": [], "status": "started",
                       "fetchLockedSources": fetch_locked_sources}
        self.coverage = {"schemaVersion": 1, "sourceCommit": commit, "platform": "linux",
                         "registryArchives": [], "missingRegistryArchives": [], "missingMaterials": []}
        (output / "logs").mkdir()
        (output / "inputs/registry-cache").mkdir(parents=True)

    def material(self, identifier, source, *, plan_material=True):
        source = Path(source)
        BUNDLE.require(BUNDLE.IDENTIFIER.fullmatch(identifier), "Invalid fixed material ID")
        BUNDLE.relative(source.name)
        target = self.output / "materials" / identifier / source.name
        identity = snapshot(source, target)
        relative_path = target.relative_to(self.output).as_posix()
        if plan_material:
            self.materials.append({"id": identifier, "path": relative_path})
        self.report["retainedMaterials"].append({"id": identifier, "input": str(source),
                                               "retained": relative_path, **identity})
        return target

    def command(self, identifier, arguments, *, environment=None, checked=True, stdout_path=None):
        stdout_path = stdout_path or self.output / "logs" / (identifier + ".stdout.log")
        stderr_path = self.output / "logs" / (identifier + ".stderr.log")
        record = {"id": identifier, "arguments": [str(item) for item in arguments], "cwd": str(ROOT),
                  "stdout": stdout_path.relative_to(self.output).as_posix(),
                  "stderr": stderr_path.relative_to(self.output).as_posix()}
        if environment is not None:
            record["environment"] = {key: environment[key] for key in (
                "CARGO_HOME", "CARGO_NET_OFFLINE", "CARGO_NET_GIT_FETCH_WITH_CLI", "RUSTUP_AUTO_INSTALL", "RUSTC",
                "CARGO_HTTP_PROXY",
            ) if key in environment}
        self.report["commands"].append(record)
        with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
            try:
                result = subprocess.run(record["arguments"], cwd=ROOT, env=environment,
                                        stdout=stdout, stderr=stderr)
            except OSError as error:
                record.update(exitCode=None, launchError=str(error))
                stderr.write(("Process could not be started: " + str(error) + "\n").encode("utf-8", "replace"))
                if checked:
                    raise
                return record
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
            self.native_receipt("libportal" if name == "portal" else name, value.get("sourceBuildReceipt"),
                                ROOT / ".deps" / workspace)
        media = BUNDLE.read_json(self.material("software-media-build", ROOT / ".deps/media-validation/build-info.json"))
        for name in ("x264", "ffmpeg"):
            actual, expected = media["components"][name], migration["softwareMedia"][name]
            for key in ("repository", "commit"):
                BUNDLE.require(actual[key] == expected[key], "Actual codec build differs from its lock: " + name)
            BUNDLE.require(actual["patches"] == expected.get("patches", []) and actual["configure"] ==
                           [str(ROOT / NATIVE_SOURCES[name] / "configure"),
                            "--prefix=" + str(ROOT / ".deps/media-validation/install"), *expected["configure"]],
                           "Actual codec patches/configuration differ: " + name)
            self.native_receipt(name, actual.get("sourceBuildReceipt"), ROOT / ".deps/media-validation")

    def native_receipt(self, component, specification, workspace):
        if specification is None:
            self.coverage["missingMaterials"].append(component + ": this original builder did not record a source/build receipt")
            return
        BUNDLE.require(isinstance(specification, dict) and specification.get("id") == component and
                       specification.get("staticFiles") == {} and specification.get("buildRecords"),
                       "Unsupported actual native producer receipt")
        workspace = workspace.resolve(strict=True)

        def retained_path(identifier, value, expected=None):
            source = Path(value)
            BUNDLE.require(source.is_absolute() and source.resolve(strict=True).is_relative_to(workspace),
                           "Native producer evidence escaped its actual build workspace")
            path = self.material(identifier, source, plan_material=False)
            BUNDLE.require(expected is None or BUNDLE.digest(path) == expected,
                           "Actual native producer evidence changed after its receipt")
            return path.relative_to(self.output).as_posix()

        receipt = retained_path(component + "-source-receipt", specification["path"])
        records = [{"path": retained_path(component + "-source-record-" + str(index), item["path"], item["sha256"]),
                    "sha256": item["sha256"]} for index, item in enumerate(specification["buildRecords"])]
        self.build_receipts.append({"id": component, "path": receipt, "buildRecords": records, "staticFiles": {}})

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
        return self.source_cache_metadata(actual, metadata)

    def source_cache_metadata(self, actual, metadata):
        """Use unchanged original cache paths only after the full build graphs agree."""
        if not actual.get("sourceBuildProof"):
            return metadata

        def retained_reference(identifier, reference, boundary):
            original = Path(reference["path"])
            BUNDLE.require(original.is_absolute() and original.resolve(strict=True).is_relative_to(boundary),
                           "Prepared Rust provenance escaped its owned workspace")
            retained = self.material(identifier, original)
            descriptor = BUNDLE.descriptor(retained)
            BUNDLE.require(all(descriptor[key] == reference[key] for key in ("size", "sha256")),
                           "Prepared Rust provenance changed after the actual producer recorded it")
            return original, BUNDLE.read_json(retained)

        _, proof = retained_reference("native-source-build-proof", actual["sourceBuildProof"],
                                      ROOT / "artifacts/native-rust")
        BUNDLE.require(proof.get("kind") == "huhucapture-native-source-build" and proof.get("platform") == "linux" and
                       proof.get("sourceCommit") == self.commit and
                       proof.get("library", {}).get("sha256") == actual["librarySha256"],
                       "Prepared cache metadata belongs to another actual native build")
        plan_path, plan = retained_reference("native-source-plan", proof["sourcePlan"], ROOT)
        BUNDLE.require(plan.get("schemaVersion") == 1 and plan.get("platform") == "linux" and
                       Path(plan["project"]).resolve() == ROOT,
                       "Prepared source plan belongs to another checkout")
        originals = [item for item in plan.get("materials", []) if item["id"] == "native-prepared-metadata"]
        BUNDLE.require(len(originals) == 1, "The original locked source-preparation graph is missing")
        reference = dict(originals[0])
        reference["path"] = str((plan_path.parent / reference["path"]).resolve())
        original, prepared = retained_reference("native-source-cache-metadata", reference, plan_path.parent)
        BUNDLE.require(RUST_MATERIALS.graph_identity(GRAPH_API, prepared) ==
                       RUST_MATERIALS.graph_identity(GRAPH_API, metadata),
                       "Original source-cache graph differs from the actual vendored build graph")
        self.coverage["sourcePreparedMetadata"] = {
            "path": str(original), "sha256": reference["sha256"],
            "meaning": "Original pre-vendor cache paths; complete graph checked against the retained actual build metadata"}
        return prepared

    def registry_archives(self, packages, cache):
        # Rescanning after fetch replaces coverage, not previously retained
        # bytes. Verified archives remain available even after a partial fetch.
        self.coverage["registryArchives"] = []
        self.coverage["missingRegistryArchives"] = []
        for package in sorted(packages, key=package_key):
            source = package.get("source") or ""
            if not source.startswith("registry+"):
                continue
            BUNDLE.require(source == REGISTRY, "Unexpected private/additional Cargo registry")
            name = BUNDLE.relative(package["name"] + "-" + package["version"] + ".crate")
            original = cache / name
            record = {**package_identity(package_key(package)), "lockedSha256": package["checksum"]}
            BUNDLE.require(BUNDLE.HEX64.fullmatch(package["checksum"] or ""), "Invalid locked registry checksum")
            if not original.exists():
                self.coverage["missingRegistryArchives"].append(record)
                continue
            actual = BUNDLE.descriptor(original)
            if actual["sha256"] != package["checksum"]:
                self.coverage["invalidRegistryArchive"] = {**record, "actual": actual, "input": str(original)}
                raise ValueError("Existing registry archive differs from Cargo.lock: " + name)
            target = self.output / "inputs/registry-cache" / name
            if os.path.lexists(target):
                BUNDLE.require(target.is_file() and not target.is_symlink(), "Invalid retained registry archive")
                retained = BUNDLE.descriptor(target)
            else:
                retained = snapshot(original, target)
            BUNDLE.require(retained == actual, "Registry archive changed after checksum validation")
            self.coverage["registryArchives"].append({**record, "archive": retained,
                                                       "retained": target.relative_to(self.output).as_posix()})

    def appimage_runtime(self):
        archive = ROOT / "artifacts/packaging-tools/appimageRuntime/source.tar.gz"
        if not os.path.lexists(archive):
            self.coverage["missingMaterials"].append("The existing packaging step has not retained appimageRuntime/source.tar.gz")
            return None
        BUNDLE.require(archive.is_file() and not archive.is_symlink(), "Actual AppImage runtime source archive is not a regular file")
        pin = BUNDLE.read_json(ROOT / "build/package/dependencies.lock.json")["tools"]["appimageRuntime"]
        expected = pin["sourceArchive"]
        actual = BUNDLE.descriptor(archive)
        BUNDLE.require(actual["size"] == expected["size"] and actual["sha256"] == expected["sha256"],
                       "Actual AppImage runtime source archive differs from its packaging lock")
        BUNDLE.require(BUNDLE.HEX40.fullmatch(pin["commit"] or ""), "AppImage runtime has no exact locked commit")
        retained = self.material("appimage-runtime-source", archive)
        BUNDLE.require(BUNDLE.descriptor(retained) == actual, "AppImage runtime source archive changed during retention")
        location = retained.relative_to(self.output).as_posix()
        self.coverage["appimageRuntime"] = {"repository": pin["repository"], "commit": pin["commit"],
                                            "tag": pin["tag"], "archive": actual, "retained": location}
        return {"id": "appimage-runtime", "kind": "tar", "archive": location,
                "prefix": "type2-runtime-" + pin["commit"]}

    def registry_coverage(self):
        return {key: list(self.coverage[key]) for key in ("registryArchives", "missingRegistryArchives")}

    def installed_identity(self, lock, cargo_lock):
        executable = ROOT / "artifacts/install/linux/bin/huhucapture"
        sidecar = executable.with_name("build-identity.json")
        if not executable.is_file() or not sidecar.is_file():
            self.coverage["missingMaterials"].append("The current installed Linux executable/build-identity pair is missing")
            return None
        before = BUNDLE.descriptor(executable)
        retained = self.material("installed-build-identity", sidecar)
        meta = BUNDLE.packaging_metadata()
        identity = meta.binary_identity(executable)
        meta.identity(identity)
        meta.verify_binary(executable, identity)
        BUNDLE.require(BUNDLE.read_json(retained) == identity and identity["sourceCommit"] == self.commit and
                       identity["platform"] == "linux" and identity["architecture"] == "amd64" and
                       identity["dependencyLockSha256"] == BUNDLE.digest(ROOT / "build/dependencies.lock.json") and
                       identity["cargoLockSha256"] == BUNDLE.digest(cargo_lock) and
                       identity["qtVersion"] == lock["qtMigration"]["qt"]["version"] and
                       identity["version"] == BUNDLE.read_json(ROOT / "build/version.json")["version"],
                       "Installed executable/sidecar belongs to different actual source/build inputs")
        BUNDLE.require(BUNDLE.descriptor(executable) == before, "Installed executable changed during identity collection")
        self.coverage["installedBuildIdentity"] = {"executable": str(executable), **before,
                                                 "sidecar": retained.relative_to(self.output).as_posix()}
        return retained

    def fetch_locked(self, packages, cache, cargo, environment, cargo_lock, private):
        self.coverage["registryArchiveCoverageBeforeFetch"] = self.registry_coverage()
        original_lock = cargo_lock.read_bytes()
        exported = private / "project-source-check"
        BUNDLE.export_git_objects(ROOT, self.commit, exported)
        source_files = BUNDLE.inventory(exported)
        BUNDLE.compare_checkout(ROOT, source_files, self.commit)
        inputs = {"sourceCommit": self.commit, "sourceTreeSha256": BUNDLE.tree_digest(source_files),
                  "cargoLockSha256Before": BUNDLE.digest(cargo_lock)}
        self.coverage["fetchInputs"] = inputs
        fetch_environment = dict(environment)
        fetch_environment["CARGO_NET_OFFLINE"] = "false"
        # No --target: the fixed Cargo fetch traverses every platform in the
        # locked workspace resolution. It does not replace actual build metadata.
        record = self.command("cargo-fetch", [cargo, "fetch", "--locked", "--manifest-path",
                              str(ROOT / "third_party/localsend/Cargo.toml")],
                              environment=fetch_environment, checked=False)
        self.coverage["fetch"] = record
        inputs["cargoLockSha256After"] = BUNDLE.digest(cargo_lock)
        BUNDLE.require(cargo_lock.read_bytes() == original_lock, "Cargo.lock changed during locked source fetch")
        BUNDLE.compare_checkout(ROOT, source_files, self.commit)
        self.clean_project()
        inputs["sourceUnchanged"] = True
        self.registry_archives(packages, cache)
        self.coverage["registryArchiveCoverageAfterFetch"] = self.registry_coverage()

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

    def prepare_vendor(self, packages, cargo_lock, cargo_home, cache, gifski_source, private):
        # Only a private, disposable cache is writable by Cargo. Never copy a
        # Cargo home/config/credentials, or retain unrelated registry archives.
        environment = cargo_environment(private)
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
        index = cargo_home / "registry/index" / cache.name
        if index.is_dir():
            shutil.copytree(index, private / "registry/index" / cache.name, symlinks=False)
        database = private / "git/db" / gifski_source.parent.name
        database.parent.mkdir(parents=True)
        # Seed only verified local objects, without copying a cached Git
        # configuration, hooks or credential helper into the network phase.
        BUNDLE.git(ROOT, "-c", "protocol.file.allow=always", "clone", "--bare", "--local", "--no-hardlinks",
                   "--dissociate", str(self.output / "inputs/gifski"), str(database))
        commit = self.coverage["gifski"]["source"].rsplit("#", 1)[-1]
        actual_commit = BUNDLE.git(database, "rev-parse", "--verify", commit + "^{commit}").decode().strip()
        raw = BUNDLE.git(database, "cat-file", "commit", commit)
        BUNDLE.require(actual_commit == commit and BUNDLE.object_digest("commit", raw) == commit,
                       "Private gifski database is missing the exact locked commit")
        # Cargo's locked-revision/local-object branch returns this database
        # without entering its Git network/credential-configuration path.
        self.coverage["gifski"]["privateDatabaseCommit"] = actual_commit
        private_cache = private / "registry/cache" / cache.name
        shutil.copytree(self.output / "inputs/registry-cache", private_cache)
        if self.fetch_locked_sources:
            self.fetch_locked(packages, private_cache, executables["cargo"], environment, cargo_lock, private)
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
        replay_cache = private
        if self.retain_replay_cache:
            replay_cache = self.output / 'inputs/replay-cache'
            self.coverage['retainedReplayInputs'] = retain_replay_inputs(private, replay_cache)
        return {"directory": "inputs/rust-vendor", "registryCache": "inputs/registry-cache",
                "config": "vendor-config.toml", "noticeReviews": "third_party/rust-notices/upstream.json",
                "replay": {**executables, "cache": str(replay_cache)}}

    def run(self):
        # The collector must finish its real offline replay before this owned
        # cache is removed. It never writes to the build runner's Cargo cache.
        with tempfile.TemporaryDirectory(prefix="source-vendor-cache-", dir=ROOT / "artifacts") as temporary:
            return self.collect_existing(Path(temporary))

    def collect_existing(self, private):
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
        runtime = self.appimage_runtime()
        vendor = self.prepare_vendor(packages, cargo_lock, cargo_home, cache, gifski_source, private)
        identity = self.installed_identity(lock, cargo_lock)
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
                "materials": self.materials, "buildReceipts": self.build_receipts}
        if identity is not None:
            plan["buildIdentity"] = identity.relative_to(self.output).as_posix()
        if runtime is not None:
            plan["sources"].append(runtime)
        if vendor is not None:
            plan["rustVendor"] = vendor
        write_new(self.output / "collection-plan.json", plan)
        collector = self.command("source-collector", [sys.executable, str(COLLECTOR), "collect", "--plan",
                                 str(self.output / "collection-plan.json"), "--output", str(self.output / "bundle")],
                                 environment=cargo_environment(private), checked=False)
        self.report["collectorExitCode"] = collector["exitCode"]
        BUNDLE.require(collector["exitCode"] == 0, "Collector rejected the actual inputs; verified materials and logs are retained")
        version = BUNDLE.read_json(ROOT / "build/version.json")["version"]
        _, manifest_name = BUNDLE.source_output_names(version, "linux")
        manifest = BUNDLE.read_json(self.output / "bundle" / manifest_name)
        BUNDLE.require(manifest["sourceCommit"] == self.commit and manifest["complete"] is False,
                       "Unexpected source identity/completeness from this intentionally incomplete profile")
        provided = {item["id"] for item in manifest["components"]}
        BUNDLE.require(set(NATIVE_SOURCES) | {"huhucapture", "localsend", "gifski"} <= provided,
                       "A required actual source disappeared during collection")
        BUNDLE.require(runtime is None or "appimage-runtime" in provided,
                       "The verified AppImage runtime source disappeared during collection")
        self.clean_project()
        fetch_failed = self.fetch_locked_sources and self.coverage["fetch"]["exitCode"] != 0
        self.report.update(status="incomplete-bundle-retained-fetch-failed" if fetch_failed else "incomplete-bundle-retained", complete=False,
                           archive=manifest["archive"], gaps=manifest["gaps"])
        return 1 if fetch_failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-prefix", type=Path, required=True, help="Actual QT_ROOT_DIR from this Linux CI job")
    parser.add_argument("--source-commit", required=True, help="Actual checkout SHA, including the PR merge SHA")
    parser.add_argument("--output", type=Path, required=True, help="New directory under this checkout's ignored artifacts/")
    parser.add_argument("--fetch-locked-sources", action="store_true",
                        help="Fetch the fixed lock into a private cache before offline collection; retain materials but fail on fetch failure")
    parser.add_argument("--retain-replay-cache", action="store_true",
                        help="Retain only this task's public registry/Git source inputs for a later real offline source-plan replay")
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
        retention = Retention(output, args.source_commit, qt, fetch_locked_sources=args.fetch_locked_sources,
                              retain_replay_cache=args.retain_replay_cache)
        result = retention.run()
        print(json.dumps({"output": str(output), "status": retention.report["status"],
                          "complete": False, "archive": retention.report["archive"]}, indent=2))
        if result:
            print("Locked Cargo fetch failed; verified incomplete bundle and actual logs are retained.", file=sys.stderr)
        return result
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
