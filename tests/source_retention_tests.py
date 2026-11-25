#!/usr/bin/env python3
"""Offline contracts for the fixed CI profile; no Cargo fetch/vendor is simulated as a real build."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
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
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="source-retention-contract-")
        self.base = Path(self.temporary.name)
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
        with mock.patch.object(profile.Retention, "run", retained_failure), \
                mock.patch.object(profile.platform, "system", return_value="Linux"), \
                mock.patch.object(profile.platform, "machine", return_value="x86_64"), \
                mock.patch.object(sys, "argv", arguments), \
                mock.patch.object(sys, "stdout", io.StringIO()), mock.patch.object(sys, "stderr", io.StringIO()):
            self.assertEqual(profile.main(), 1)
        report = json.loads((output / "retention-report.json").read_text())
        self.assertEqual(report["status"], "incomplete-bundle-retained-fetch-failed")
        self.assertTrue(report["fetchLockedSources"])
        self.assertFalse(report["complete"])
        self.assertEqual(json.loads((output / "cargo-coverage.json").read_text())["fetch"]["exitCode"], 101)


if __name__ == "__main__":
    unittest.main()
