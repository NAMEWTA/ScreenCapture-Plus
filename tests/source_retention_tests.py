#!/usr/bin/env python3
"""Offline contracts for the fixed CI profile; no Cargo fetch/vendor is simulated as a real build."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/retain-development-sources.py"
profile = types.ModuleType("source_retention")
profile.__file__ = str(SCRIPT)
exec(compile(SCRIPT.read_bytes(), str(SCRIPT), "exec"), profile.__dict__)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)


def archive(path, name):
    with tarfile.open(path, "w:gz") as stream:
        for filename, data in (("Cargo.toml", '[package]\nname = "' + name + '"\nversion = "1.0.0"\n'),
                               ("LICENSE", "Contract fixture license\n")):
            encoded = data.encode()
            entry = tarfile.TarInfo(name + "-1.0.0/" + filename)
            entry.size = len(encoded)
            stream.addfile(entry, io.BytesIO(encoded))
    return {"name": name, "version": "1.0.0", "source": profile.REGISTRY,
            "checksum": hashlib.sha256(path.read_bytes()).hexdigest()}


class SourceRetentionTests(unittest.TestCase):
    def test_retained_replay_inputs_exclude_credentials_and_unrelated_cargo_state(self):
        with tempfile.TemporaryDirectory(prefix='retained-public-cargo-') as temporary:
            root = Path(temporary).resolve()
            private = root / 'private'
            for name, value in (('registry/cache/public/package.crate', b'original source archive'),
                                ('registry/index/public/config.json', b'public registry metadata'),
                                ('git/db/locked/config', b'[core]\n bare = true\n'),
                                ('credentials.toml', b'never retain'), ('config.toml', b'never inherit'),
                                ('target/objects', b'not corresponding source')):
                write(private / name, value)
            kept = root / 'kept'
            result = profile.retain_replay_inputs(private, kept)
            self.assertEqual(set(result), {'registry/cache', 'registry/index', 'git/db'})
            self.assertEqual(sorted(item['path'] for item in profile.BUNDLE.inventory(kept)),
                             ['git/db/locked/config', 'registry/cache/public/package.crate',
                              'registry/index/public/config.json'])
            shutil.rmtree(private)
            self.assertEqual((kept / 'registry/cache/public/package.crate').read_bytes(), b'original source archive')
            with self.assertRaisesRegex(ValueError, 'already exist'):
                profile.retain_replay_inputs(kept, kept)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="source-retention-contract-")
        # Match production ROOT: Windows TEMP may use an 8.3 directory alias,
        # while the CLI resolves its output before checking containment.
        self.base = Path(self.temporary.name).resolve(strict=True)
        self.root = self.base / "project"
        self.root.mkdir()
        self.root_patch = mock.patch.object(profile, "ROOT", self.root)
        self.root_patch.start()
        write(self.root / ".gitignore", "artifacts/\n")
        write(self.root / "scripts/retain-development-sources.py", SCRIPT.read_bytes())
        write(self.root / "third_party/localsend/Cargo.toml", "[workspace]\nmembers = []\n")
        self.lock = self.root / "third_party/localsend/Cargo.lock"
        write(self.lock, "version = 4\n")
        write(self.root / "src/source.rs", "pub fn fixture() {}\n")
        for arguments in (("init", "--quiet"), ("config", "user.name", "Source retention contract"),
                          ("config", "user.email", "retention@example.invalid"),
                          ("config", "core.autocrlf", "false"), ("add", "."),
                          ("commit", "--quiet", "-m", "Actual fixed profile contract source")):
            profile.BUNDLE.git(self.root, *arguments)
        self.commit = profile.BUNDLE.git(self.root, "rev-parse", "HEAD").decode().strip()
        (self.root / "artifacts").mkdir()
        self.original_cache = self.base / "original-cache"
        self.original_cache.mkdir()
        self.inputs = self.base / "archive-inputs"
        self.inputs.mkdir()
        self.packages = [archive(self.inputs / (name + "-1.0.0.crate"), name) for name in ("alpha", "beta")]
        shutil.copyfile(self.inputs / "alpha-1.0.0.crate", self.original_cache / "alpha-1.0.0.crate")

    def tearDown(self):
        self.root_patch.stop()
        self.temporary.cleanup()

    def retention(self, name="result", *, fetch=True):
        output = self.root / "artifacts" / name
        output.mkdir()
        return profile.Retention(output, self.commit, self.root, fetch_locked_sources=fetch)

    def private_cache(self, retention):
        private = self.root / "artifacts" / (retention.output.name + "-private")
        private.mkdir()
        cache = private / "registry/cache/contract-index"
        shutil.copytree(retention.output / "inputs/registry-cache", cache)
        return private, cache

    def controlled_fetch(self, retention, private, cache, script):
        # This is an orchestration boundary test. The only executed program is
        # Python, and its genuine arguments/exit/logs remain identified as such.
        # It never pretends to prove Cargo or network behavior.
        actual_command = retention.command
        requested = []
        environment = profile.cargo_environment(private)

        def run(identifier, arguments, **keywords):
            requested.append((identifier, arguments, dict(keywords["environment"])))
            return actual_command(identifier, [sys.executable, "-c", script], **keywords)

        with mock.patch.object(retention, "command", side_effect=run):
            retention.fetch_locked(self.packages, cache, "/contract/not-executed/cargo", environment,
                                   self.lock, private)
        self.assertEqual(environment["CARGO_NET_OFFLINE"], "true")
        return requested

    def test_rescan_keeps_verified_archives_without_overwrite_or_duplicate(self):
        retention = self.retention()
        retention.registry_archives(self.packages, self.original_cache)
        before = retention.registry_coverage()
        first = retention.output / "inputs/registry-cache/alpha-1.0.0.crate"
        first_stat = first.stat()
        private, cache = self.private_cache(retention)
        shutil.copyfile(self.inputs / "beta-1.0.0.crate", cache / "beta-1.0.0.crate")
        write(cache / "unlocked-1.0.0.crate", b"not a locked input")
        retention.registry_archives(self.packages, cache)
        retention.registry_archives(self.packages, cache)
        self.assertEqual(len(before["registryArchives"]), 1)
        self.assertEqual(len(before["missingRegistryArchives"]), 1)
        self.assertEqual(len(retention.coverage["registryArchives"]), 2)
        self.assertEqual(retention.coverage["missingRegistryArchives"], [])
        self.assertEqual((first.stat().st_ino, first.stat().st_mtime_ns), (first_stat.st_ino, first_stat.st_mtime_ns))
        self.assertFalse((retention.output / "inputs/registry-cache/unlocked-1.0.0.crate").exists())
        self.assertFalse((self.original_cache / "beta-1.0.0.crate").exists())

    def test_new_corrupt_archive_is_rejected_and_never_published(self):
        retention = self.retention()
        retention.registry_archives(self.packages, self.original_cache)
        private, cache = self.private_cache(retention)
        write(cache / "beta-1.0.0.crate", b"wrong archive")
        with self.assertRaisesRegex(ValueError, "differs from Cargo.lock"):
            retention.registry_archives(self.packages, cache)
        self.assertEqual(retention.coverage["invalidRegistryArchive"]["name"], "beta")
        self.assertEqual(len(retention.coverage["registryArchives"]), 1)
        self.assertFalse((retention.output / "inputs/registry-cache/beta-1.0.0.crate").exists())

    def appimage_archive_fixture(self):
        commit = "a" * 40
        path = self.root / "artifacts/packaging-tools/appimageRuntime/source.tar.gz"
        path.parent.mkdir(parents=True)
        with tarfile.open(path, "w:gz") as stream:
            for name, data in (("src/runtime/runtime.c", b"int runtime_fixture;\n"),
                               ("LICENSE", b"Runtime source contract fixture license\n")):
                entry = tarfile.TarInfo("type2-runtime-" + commit + "/" + name)
                entry.size = len(data)
                stream.addfile(entry, io.BytesIO(data))
        pin = {"repository": "https://example.invalid/runtime-fixture", "commit": commit, "tag": "fixture",
               "sourceArchive": {"size": path.stat().st_size, "sha256": profile.BUNDLE.digest(path)}}
        write(self.root / "build/package/dependencies.lock.json", profile.BUNDLE.canonical({"tools": {"appimageRuntime": pin}}))
        return path, pin

    def test_absent_runtime_archive_remains_a_material_gap(self):
        retention = self.retention()
        self.assertIsNone(retention.appimage_runtime())
        self.assertEqual(retention.materials, [])
        self.assertNotIn("appimageRuntime", retention.coverage)
        self.assertTrue(any("appimageRuntime/source.tar.gz" in item for item in retention.coverage["missingMaterials"]))

    def native_receipt_fixture(self):
        workspace = self.root / ".deps/receipt-fixture"
        receipt = workspace / "source-build-receipt.json"
        record = workspace / "build-execution.json"
        # These bytes exercise only retention/ownership. They are never fed to
        # the source collector or described as an actual product build receipt.
        write(receipt, '{"scope":"retention contract fixture"}\n')
        write(record, '{"scope":"original retained record fixture"}\n')
        plan = {"id": "ffmpeg", "path": str(receipt), "staticFiles": {},
                "buildRecords": [{"path": str(record), "sha256": profile.BUNDLE.digest(record)}]}
        return workspace, receipt, record, plan

    def test_native_receipt_handoff_uses_verified_snapshots_without_rewriting_old_metadata(self):
        workspace, receipt, record, plan = self.native_receipt_fixture()
        originals = receipt.read_bytes(), record.read_bytes()
        retention = self.retention()
        retention.native_receipt("ffmpeg", plan, workspace)
        self.assertEqual(len(retention.build_receipts), 1)
        retained = retention.build_receipts[0]
        self.assertEqual(retained["id"], "ffmpeg")
        self.assertEqual(retained["staticFiles"], {})
        self.assertEqual(retention.materials, [])
        write(receipt, "changed original receipt")
        write(record, "changed original record")
        self.assertEqual((retention.output / retained["path"]).read_bytes(), originals[0])
        self.assertEqual((retention.output / retained["buildRecords"][0]["path"]).read_bytes(), originals[1])
        missing = self.retention("old-build")
        missing.native_receipt("ffmpeg", None, workspace)
        self.assertEqual(missing.build_receipts, [])
        self.assertIn("original builder did not record", missing.coverage["missingMaterials"][0])

    def test_native_receipt_rejects_changed_record_and_evidence_outside_owned_workspace(self):
        workspace, receipt, record, plan = self.native_receipt_fixture()
        plan["buildRecords"][0]["sha256"] = "0" * 64
        changed = self.retention("changed-record")
        with self.assertRaisesRegex(ValueError, "changed after its receipt"):
            changed.native_receipt("ffmpeg", plan, workspace)
        self.assertEqual(changed.build_receipts, [])
        outside = self.base / "other-build-record.json"
        write(outside, "unrelated caller data")
        plan["buildRecords"] = [{"path": str(outside), "sha256": profile.BUNDLE.digest(outside)}]
        escaped = self.retention("outside-record")
        with self.assertRaisesRegex(ValueError, "escaped its actual build workspace"):
            escaped.native_receipt("ffmpeg", plan, workspace)
        self.assertEqual(escaped.build_receipts, [])
        self.assertFalse(any(item["input"] == str(outside) for item in escaped.report["retainedMaterials"]))

    def test_prepared_cache_metadata_preserves_both_paths_and_rejects_a_different_graph(self):
        # These JSON fixtures check retention and graph identity only. They are
        # not fed to a build or presented as real Cargo execution evidence.
        package_id = profile.REGISTRY + "#alpha@1.0.0"
        prepared = {"packages": [{"id": package_id, "name": "alpha", "version": "1.0.0",
                     "source": profile.REGISTRY, "manifest_path": str(self.original_cache / "alpha/Cargo.toml")}],
                    "resolve": {"nodes": [{"id": package_id, "features": [], "deps": []}]},
                    "workspace_members": [], "workspace_default_members": []}
        metadata_path = self.root / ".deps/native-build-sources/cargo-metadata.json"
        write(metadata_path, profile.BUNDLE.canonical(prepared))
        metadata_identity = profile.BUNDLE.descriptor(metadata_path)
        plan_path = metadata_path.with_name("source-plan.json")
        plan = {"schemaVersion": 1, "platform": "linux", "project": str(self.root),
                "materials": [{"id": "native-prepared-metadata", "path": str(metadata_path), **metadata_identity}]}
        write(plan_path, profile.BUNDLE.canonical(plan))
        proof_path = self.root / "artifacts/native-rust/source-execution/proof.json"
        proof = {"kind": "huhucapture-native-source-build", "platform": "linux", "sourceCommit": self.commit,
                 "library": {"sha256": "2" * 64},
                 "sourcePlan": {"path": str(plan_path), **profile.BUNDLE.descriptor(plan_path)}}
        write(proof_path, profile.BUNDLE.canonical(proof))
        receipt = {"librarySha256": "2" * 64,
                   "sourceBuildProof": {"path": str(proof_path), **profile.BUNDLE.descriptor(proof_path)}}
        actual = json.loads(json.dumps(prepared))
        actual["packages"][0]["manifest_path"] = str(self.root / ".deps/native-build-sources/vendor/alpha/Cargo.toml")
        retention = self.retention()
        original_bytes = metadata_path.read_bytes()
        self.assertEqual(retention.source_cache_metadata(receipt, actual), prepared)
        self.assertEqual(metadata_path.read_bytes(), original_bytes)
        self.assertEqual(Path(actual["packages"][0]["manifest_path"]),
                         self.root / ".deps/native-build-sources/vendor/alpha/Cargo.toml")
        actual["resolve"]["nodes"][0]["features"] = ["different-feature"]
        with self.assertRaisesRegex(ValueError, "differs from the actual vendored build graph"):
            self.retention("changed-graph").source_cache_metadata(receipt, actual)
        write(metadata_path, profile.BUNDLE.canonical(actual))
        with self.assertRaisesRegex(ValueError, "changed after the actual producer"):
            self.retention("changed-preparation").source_cache_metadata(receipt, actual)

    def test_locked_runtime_archive_uses_a_retained_snapshot_in_the_tar_plan(self):
        original, pin = self.appimage_archive_fixture()
        expected = original.read_bytes()
        retention = self.retention()
        source = retention.appimage_runtime()
        retained = retention.output / source["archive"]
        self.assertEqual(source, {"id": "appimage-runtime", "kind": "tar",
                                  "archive": "materials/appimage-runtime-source/source.tar.gz",
                                  "prefix": "type2-runtime-" + pin["commit"]})
        self.assertEqual(retained.read_bytes(), expected)
        self.assertEqual(retention.coverage["appimageRuntime"]["archive"]["sha256"], pin["sourceArchive"]["sha256"])
        self.assertEqual(retention.materials, [{"id": "appimage-runtime-source", "path": source["archive"]}])
        write(original, b"later mutation of the original input")
        self.assertEqual(retained.read_bytes(), expected)

    def test_runtime_archive_size_or_hash_mismatch_is_not_an_ordinary_gap(self):
        original, _ = self.appimage_archive_fixture()
        data = original.read_bytes()
        for label, changed in (("size", data + b"extra"), ("hash", bytes([data[0] ^ 1]) + data[1:])):
            with self.subTest(label=label):
                write(original, changed)
                retention = self.retention("runtime-" + label)
                with self.assertRaisesRegex(ValueError, "differs from its packaging lock"):
                    retention.appimage_runtime()
                self.assertEqual(retention.materials, [])
                self.assertNotIn("appimageRuntime", retention.coverage)
                self.assertFalse((retention.output / "materials/appimage-runtime-source/source.tar.gz").exists())

    def test_fetch_results_keep_partial_bytes_and_actual_logs_without_changing_inputs(self):
        for exit_code in (0, 101):
            with self.subTest(exit_code=exit_code):
                retention = self.retention("fetch-" + str(exit_code))
                retention.registry_archives(self.packages, self.original_cache)
                private, cache = self.private_cache(retention)
                original_lock = self.lock.read_bytes()
                original_source = (self.root / "src/source.rs").read_bytes()
                script = ("import shutil, sys\n"
                          + "shutil.copyfile(" + repr(str(self.inputs / "beta-1.0.0.crate")) + ", "
                          + repr(str(cache / "beta-1.0.0.crate")) + ")\n"
                          + "print('controlled process stdout')\nprint('controlled process stderr', file=sys.stderr)\n"
                          + "raise SystemExit(" + str(exit_code) + ")\n")
                requested = self.controlled_fetch(retention, private, cache, script)
                self.assertEqual(requested[0][1], ["/contract/not-executed/cargo", "fetch", "--locked",
                                                 "--manifest-path", str(self.root / "third_party/localsend/Cargo.toml")])
                self.assertEqual(requested[0][2]["CARGO_NET_OFFLINE"], "false")
                self.assertEqual(requested[0][2]["RUSTUP_AUTO_INSTALL"], "0")
                self.assertEqual(requested[0][2]["CARGO_HTTP_PROXY"], "")
                result = retention.coverage["fetch"]
                self.assertEqual(result["arguments"][0], sys.executable)
                self.assertEqual(result["exitCode"], exit_code)
                self.assertEqual(result["environment"]["CARGO_HTTP_PROXY"], "")
                self.assertEqual((retention.output / result["stdout"]).read_text().strip(), "controlled process stdout")
                self.assertEqual((retention.output / result["stderr"]).read_text().strip(), "controlled process stderr")
                self.assertEqual(len(retention.coverage["registryArchiveCoverageBeforeFetch"]["registryArchives"]), 1)
                self.assertEqual(len(retention.coverage["registryArchiveCoverageAfterFetch"]["registryArchives"]), 2)
                self.assertEqual(retention.coverage["missingRegistryArchives"], [])
                self.assertTrue(retention.coverage["fetchInputs"]["sourceUnchanged"])
                self.assertEqual(self.lock.read_bytes(), original_lock)
                self.assertEqual((self.root / "src/source.rs").read_bytes(), original_source)
                self.assertFalse((self.original_cache / "beta-1.0.0.crate").exists())

    def test_lock_mutation_is_a_hard_failure_even_when_fetch_failed(self):
        retention = self.retention()
        retention.registry_archives(self.packages, self.original_cache)
        private, cache = self.private_cache(retention)
        script = "from pathlib import Path\nPath(" + repr(str(self.lock)) + ").write_text('changed lock')\nraise SystemExit(101)\n"
        with self.assertRaisesRegex(ValueError, "Cargo.lock changed"):
            self.controlled_fetch(retention, private, cache, script)
        self.assertEqual(retention.coverage["fetch"]["exitCode"], 101)
        self.assertNotIn("sourceUnchanged", retention.coverage["fetchInputs"])
        self.assertNotIn("registryArchiveCoverageAfterFetch", retention.coverage)

    def test_other_source_mutation_is_a_hard_failure_after_fetch_success(self):
        retention = self.retention()
        retention.registry_archives(self.packages, self.original_cache)
        private, cache = self.private_cache(retention)
        script = "from pathlib import Path\nPath(" + repr(str(self.root / "src/source.rs")) + ").write_text('changed source')\n"
        with self.assertRaisesRegex(ValueError, "unrecorded source change"):
            self.controlled_fetch(retention, private, cache, script)
        self.assertEqual(retention.coverage["fetch"]["exitCode"], 0)
        self.assertNotIn("sourceUnchanged", retention.coverage["fetchInputs"])

    def test_private_environment_removes_cargo_credentials_and_config_overrides(self):
        inherited = {"CARGO_REGISTRY_TOKEN": "contract-secret", "CARGO_REGISTRIES_OTHER_TOKEN": "contract-secret",
                     "CARGO_HOME": "original-home", "CARGO_NET_OFFLINE": "false", "RUSTUP_AUTO_INSTALL": "1",
                     "CARGO_HTTP_PROXY": "http://contract-secret@proxy.invalid", "HTTPS_PROXY": "http://proxy.invalid",
                     "http_proxy": "http://proxy.invalid", "ALL_PROXY": "http://proxy.invalid",
                     "CARGO_BUILD_RUSTC_WRAPPER": "unexpected-wrapper", "RUSTC_WRAPPER": "unexpected-wrapper"}
        private = self.root / "artifacts/private"
        with mock.patch.dict(os.environ, inherited):
            environment = profile.cargo_environment(private)
            self.assertEqual(os.environ["CARGO_REGISTRY_TOKEN"], "contract-secret")
        self.assertNotIn("CARGO_REGISTRY_TOKEN", environment)
        self.assertNotIn("CARGO_REGISTRIES_OTHER_TOKEN", environment)
        self.assertNotIn("CARGO_BUILD_RUSTC_WRAPPER", environment)
        self.assertNotIn("RUSTC_WRAPPER", environment)
        self.assertEqual(environment["CARGO_HOME"], str(private))
        self.assertEqual(environment["CARGO_NET_OFFLINE"], "true")
        self.assertEqual(environment["RUSTUP_AUTO_INSTALL"], "0")
        self.assertEqual(environment["CARGO_HTTP_PROXY"], "")
        for name in ("HTTPS_PROXY", "http_proxy", "ALL_PROXY"):
            self.assertNotIn(name, environment)
        write(self.root / ".cargo/config.toml", "[net]\noffline = false\n")
        with self.assertRaisesRegex(ValueError, "cannot inherit Cargo configuration"):
            profile.cargo_environment(private)

    def test_real_launch_failure_is_recorded_without_an_invented_exit_code(self):
        retention = self.retention()
        result = retention.command("unavailable-command", [str(self.base / "does-not-exist")], checked=False)
        self.assertIsNone(result["exitCode"])
        self.assertIn("launchError", result)
        self.assertEqual((retention.output / result["stdout"]).read_bytes(), b"")
        self.assertIn("Process could not be started", (retention.output / result["stderr"]).read_text())

    def test_private_cargo_cache_survives_the_collector_handoff_and_is_removed_on_failure(self):
        retention = self.retention()
        observed = []

        def collector_boundary(private):
            # A resource-lifetime contract only: no Cargo or collector success
            # is simulated. The actual temporary directory is the object tested.
            self.assertTrue(private.is_dir())
            self.assertTrue(private.is_relative_to(self.root / "artifacts"))
            write(private / "registry/cache/fixture", b"owned lifecycle fixture")
            observed.append(private)
            self.assertEqual((private / "registry/cache/fixture").read_bytes(), b"owned lifecycle fixture")
            raise ValueError("actual handoff failure fixture")

        with mock.patch.object(retention, "collect_existing", side_effect=collector_boundary) as handoff:
            with self.assertRaisesRegex(ValueError, "actual handoff failure"):
                retention.run()
        handoff.assert_called_once()
        self.assertEqual(len(observed), 1)
        self.assertFalse(observed[0].exists())
        self.assertTrue(retention.output.is_dir())

    @unittest.skipIf(os.name == "nt", "The real Linux ELF identity binding is validated with the host C compiler")
    def test_installed_identity_is_bound_to_real_executable_and_current_inputs(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.fail("The installed identity source binding requires the actual host C compiler")
        lock = {"qtMigration": {"qt": {"version": "6.12.1"}}}
        write(self.root / "build/dependencies.lock.json", profile.BUNDLE.canonical(lock))
        write(self.root / "build/version.json", profile.BUNDLE.canonical({"schemaVersion": 1, "version": "0.0.5"}))
        source = self.root / "src/identity.c"
        write(source, 'volatile const char identity[] = "HUHU_BUILD_IDENTITY_V1\\0" BUILD_IDENTITY "\\0HUHU_BUILD_IDENTITY_END";\n'
                      "int main(void) { return identity[0] == 'H' ? 0 : 1; }\n")
        profile.BUNDLE.git(self.root, "add", ".")
        profile.BUNDLE.git(self.root, "commit", "--quiet", "-m", "Actual identity contract source")
        self.commit = profile.BUNDLE.git(self.root, "rev-parse", "HEAD").decode().strip()
        identity = {"schemaVersion": 1, "product": "huhucapture", "repository": "NAMEWTA/huhucapture",
                    "version": "0.0.5", "tag": "v0.0.5", "sourceCommit": self.commit, "sourceDirty": False,
                    "platform": "linux", "architecture": "amd64", "configuration": "Release", "qtVersion": "6.12.1",
                    "dependencyLockSha256": profile.BUNDLE.digest(self.root / "build/dependencies.lock.json"),
                    "cargoLockSha256": profile.BUNDLE.digest(self.lock)}
        executable = self.root / "artifacts/install/linux/bin/huhucapture"
        executable.parent.mkdir(parents=True)
        subprocess.run([compiler, str(source), "-DBUILD_IDENTITY=" + json.dumps(json.dumps(identity)), "-o", str(executable)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sidecar = executable.with_name("build-identity.json")
        write(sidecar, profile.BUNDLE.canonical(identity))
        retention = self.retention()
        actual = retention.installed_identity(lock, self.lock)
        self.assertEqual(actual.read_bytes(), sidecar.read_bytes())
        self.assertEqual(retention.coverage["installedBuildIdentity"]["sha256"], profile.BUNDLE.digest(executable))
        write(sidecar, profile.BUNDLE.canonical({**identity, "sourceCommit": "0" * 40}))
        with self.assertRaisesRegex(ValueError, "different actual source/build inputs"):
            self.retention("mismatched-sidecar").installed_identity(lock, self.lock)

    def test_missing_installed_identity_remains_a_material_gap(self):
        retention = self.retention()
        self.assertIsNone(retention.installed_identity({}, self.lock))
        self.assertIn("The current installed Linux executable/build-identity pair is missing",
                      retention.coverage["missingMaterials"])

    def test_cli_preserves_failure_after_a_retained_incomplete_result(self):
        output = self.root / "artifacts/cli-result"

        def retained_failure(retention):
            # Only the CLI handoff is isolated here, not an actual collector
            # result. No source bundle, Cargo success or build receipt is made.
            retention.report.update(status="incomplete-bundle-retained-fetch-failed", complete=False,
                                    archive={"contractFixture": True})
            retention.coverage["fetch"] = {"exitCode": 101}
            return 1

        arguments = [str(SCRIPT), "--qt-prefix", str(self.root), "--source-commit", self.commit,
                     "--output", str(output), "--fetch-locked-sources"]
        stderr = io.StringIO()
        with mock.patch.object(profile.Retention, "run", autospec=True, side_effect=retained_failure) as run, \
                mock.patch.object(profile.platform, "system", return_value="Linux"), \
                mock.patch.object(profile.platform, "machine", return_value="x86_64"), \
                mock.patch.object(sys, "argv", arguments), \
                mock.patch.object(sys, "stdout", io.StringIO()), mock.patch.object(sys, "stderr", stderr):
            result = profile.main()
        self.assertEqual(result, 1, stderr.getvalue())
        self.assertEqual(run.call_count, 1, stderr.getvalue())
        self.assertIn("Locked Cargo fetch failed; verified incomplete bundle and actual logs are retained.",
                      stderr.getvalue())
        self.assertTrue((output / "retention-report.json").is_file(), stderr.getvalue())
        report = json.loads((output / "retention-report.json").read_text())
        self.assertEqual(report["status"], "incomplete-bundle-retained-fetch-failed")
        self.assertTrue(report["fetchLockedSources"])
        self.assertFalse(report["complete"])
        self.assertEqual(json.loads((output / "cargo-coverage.json").read_text())["fetch"]["exitCode"], 101)


if __name__ == "__main__":
    unittest.main()
