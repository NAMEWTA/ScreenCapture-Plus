#!/usr/bin/env python3
"""Offline collection tests using actual temporary Git repositories and archives."""
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("source_bundle", ROOT / "scripts/create-source-bundle.py")
bundle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundle)


def command(directory, *args):
    return subprocess.check_output(args, cwd=directory, stderr=subprocess.PIPE).decode().strip()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)


def write_json(path, value):
    write(path, bundle.canonical(value))


def repository(path, files):
    path.mkdir()
    command(path, "git", "init", "--quiet")
    command(path, "git", "config", "user.name", "Source bundle fixture")
    command(path, "git", "config", "user.email", "source-fixture@example.invalid")
    command(path, "git", "config", "core.autocrlf", "false")
    for name, data in files.items():
        write(path / name, data)
    command(path, "git", "add", ".")
    command(path, "git", "commit", "--quiet", "-m", "Actual source collection fixture")
    return command(path, "git", "rev-parse", "HEAD")


def source_archive(path, files, prefix="crate-1.0.0"):
    with tarfile.open(path, "w:gz") as archive:
        for name, data in files.items():
            info = tarfile.TarInfo(prefix + "/" + name)
            encoded = data.encode() if isinstance(data, str) else data
            info.size, info.mode = len(encoded), 0o644
            archive.addfile(info, io.BytesIO(encoded))


class SourceBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="source-bundle-tests-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_actual_patched_git_export_keeps_input_index_and_refs(self):
        source = self.root / "upstream"
        commit = repository(source, {"LICENSE": "Fixture license\n", "source.c": "int value = 1;\n",
                                     ".gitignore": "build/\n", ".gitattributes": "LICENSE export-ignore\n"})
        patch = self.root / "fix.patch"
        write(source / "source.c", "int value = 2;\n")
        write(patch, bundle.git(source, "diff", "--binary"))
        write(source / "build/ignored.o", b"not a source input")
        before = {name: (source / ".git" / name).read_bytes() for name in ("HEAD", "index", "refs/heads/master")
                  if (source / ".git" / name).exists()}
        with self.assertRaisesRegex(ValueError, "Source must be its own Git checkout"):
            bundle.export_git(source / "build", commit, self.root / "subdirectory-export")
        self.assertFalse((self.root / "subdirectory-export").exists())
        output = self.root / "export"
        provenance, files = bundle.export_git(source, commit, output, [patch])
        self.assertEqual((output / "source.c").read_text(), "int value = 2;\n")
        self.assertTrue((output / "LICENSE").is_file())  # export-ignore cannot drop source/license material
        self.assertFalse((output / "build").exists())
        self.assertNotEqual(provenance["pristineTreeSha256"], provenance["sourceTreeSha256"])
        self.assertEqual(before, {name: (source / ".git" / name).read_bytes() for name in before})
        write(source / "source.c", "int value = 3;\n")
        with self.assertRaisesRegex(ValueError, "unrecorded source change"):
            bundle.compare_checkout(source, files, commit)

    def test_deleted_patch_file_and_untracked_source_cannot_escape_comparison(self):
        source = self.root / "upstream"
        commit = repository(source, {"source.c": "int value;\n", "obsolete.c": "int removed;\n"})
        old = (source / "obsolete.c").read_bytes()
        (source / "obsolete.c").unlink()
        patch = self.root / "delete.patch"
        write(patch, bundle.git(source, "diff", "--binary"))
        write(source / "obsolete.c", old)
        with self.assertRaisesRegex(ValueError, "Patch-deleted source"):
            bundle.export_git(source, commit, self.root / "export", [patch])
        (source / "obsolete.c").unlink()
        write(source / "extra.c", "int unrecorded;\n")
        with self.assertRaisesRegex(ValueError, "Unrecorded source input"):
            bundle.export_git(source, commit, self.root / "export2", [patch])

    def test_uninitialized_submodule_is_not_a_complete_tree(self):
        child = self.root / "child"
        child_commit = repository(child, {"source.c": "int child;\n"})
        source = self.root / "parent"
        repository(source, {"LICENSE": "license\n"})
        command(source, "git", "update-index", "--add", "--cacheinfo", "160000," + child_commit + ",child")
        command(source, "git", "commit", "--quiet", "-m", "Submodule fixture")
        with self.assertRaisesRegex(ValueError, "Git source check failed"):
            bundle.export_git(source, command(source, "git", "rev-parse", "HEAD"), self.root / "export")

    @unittest.skipIf(os.name == "nt", "This Unix fsmonitor hook regression requires executable shell scripts")
    def test_git_source_collection_never_executes_repository_fsmonitor(self):
        source = self.root / "upstream"
        commit = repository(source, {"source.c": "int value;\n"})
        marker, hook = self.root / "hook-was-run", self.root / "fsmonitor-hook"
        write(hook, "#!/bin/sh\nprintf ran > " + shlex.quote(str(marker)) + "\nprintf '0\\0'\n")
        hook.chmod(0o755)
        command(source, "git", "config", "core.fsmonitor", str(hook))
        _, files = bundle.export_git(source, commit, self.root / "export")
        bundle.compare_checkout(source, files, commit)
        self.assertFalse(marker.exists(), "Read-only source collection ran an input repository hook")

    @unittest.skipIf(os.name == "nt", "This Unix signature-program regression requires executable shell scripts")
    def test_commit_timestamp_does_not_execute_configured_signature_program(self):
        source = self.root / "upstream"
        repository(source, {"source.c": "int value;\n"})
        tree = command(source, "git", "rev-parse", "HEAD^{tree}")
        raw = ("tree " + tree + "\nauthor Fixture <fixture@example.invalid> 1700000000 +0000\n"
               "committer Fixture <fixture@example.invalid> 1700000000 +0000\n"
               "gpgsig -----BEGIN PGP SIGNATURE-----\n \n Zml4dHVyZQ==\n -----END PGP SIGNATURE-----\n\n"
               "Unsigned test fixture with an intentionally invalid signature block.\n").encode()
        commit = subprocess.check_output(["git", "hash-object", "-t", "commit", "-w", "--stdin"], cwd=source, input=raw).decode().strip()
        marker, program = self.root / "signature-was-run", self.root / "test-gpg-program"
        write(program, "#!/bin/sh\nprintf ran > " + shlex.quote(str(marker)) + "\nexit 1\n")
        program.chmod(0o755)
        command(source, "git", "config", "gpg.program", str(program))
        command(source, "git", "config", "gpg.format", "openpgp")
        command(source, "git", "config", "log.showSignature", "true")
        # Establish that the actual Git fixture invokes this harmless owned
        # program without the collector's guard; no signature is validated.
        subprocess.run(["git", "show", "-s", "--format=%ct", commit], cwd=source,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertTrue(marker.exists(), "Signature fixture did not exercise actual Git behavior")
        marker.unlink()
        self.assertEqual(bundle.git(source, "show", "-s", "--format=%ct", commit).strip(), b"1700000000")
        self.assertFalse(marker.exists(), "Read-only source collection executed the configured signature program")

    def test_consumers_read_the_retained_snapshot_after_original_changes(self):
        stage = self.root / "stage"
        stage.mkdir()
        collector = bundle.Collector({}, self.root, stage)
        original = self.root / "receipt.json"
        write_json(original, {"input": "verified bytes"})
        snapshot = collector.retain("receipt", str(original))
        write_json(original, {"input": "changed after retention"})
        self.assertNotEqual(snapshot, original)
        self.assertEqual(bundle.read_json(snapshot), {"input": "verified bytes"})
        self.assertEqual(bundle.digest(snapshot), collector.materials["receipt"]["sha256"])

    def test_configure_profiles_reject_explicit_conflicting_or_reordered_flags(self):
        windows = ["--enable-shared", "--disable-network", "--enable-swscale"]
        command = ["/source/configure", "--prefix=/prefix", *windows, "--extra-cflags=-MD",
                   "--extra-cxxflags=-MD", "--extra-ldflags=-debug -opt:ref -opt:icf", "--x86asmexe=/tools/nasm"]
        bundle.verify_configure_command(command, windows, "ffmpeg", "windows")
        for changed in (command + ["--disable-swscale"], command[:-1] + ["--enable-nonfree", command[-1]],
                        [part.replace("-MD", "-MDd") for part in command],
                        command[:2] + list(reversed(windows)) + command[2 + len(windows):]):
            with self.assertRaises(ValueError):
                bundle.verify_configure_command(changed, windows, "ffmpeg", "windows")
        linux = ["--enable-gpl", "--enable-version3", "--enable-libx264"]
        command = ["/source/configure", "--prefix=/prefix", *linux]
        bundle.verify_configure_command(command, linux, "ffmpeg", "linux")
        with self.assertRaisesRegex(ValueError, "exact ordered"):
            bundle.verify_configure_command(command + ["--enable-nonfree"], linux, "ffmpeg", "linux")
        meson = ["--buildtype=release", "-Dauto_features=disabled", "-Dtests=disabled"]
        command = ["meson", "setup", "/build", "/source", "--prefix=/prefix", "--libdir=lib", *meson]
        bundle.verify_configure_command(command, meson, "pipewire", "linux")
        with self.assertRaisesRegex(ValueError, "exact locked"):
            bundle.verify_configure_command(command + ["-Dtests=enabled"], meson, "pipewire", "linux")

    def test_exact_registry_archive_detects_rewritten_vendor_checksums(self):
        vendor, cache = self.root / "vendor", self.root / "cache"
        vendor.mkdir()
        cache.mkdir()
        crate = vendor / "crate-1.0.0"
        files = {"Cargo.toml": '[package]\nname="crate"\nversion="1.0.0"\nlicense="MIT"\n',
                 "src/lib.rs": "pub fn original() {}\n", "LICENSE": "Original test license\n"}
        archive = cache / "crate-1.0.0.crate"
        source_archive(archive, files)
        for name, value in files.items():
            write(crate / name, value)
        checksum = {"package": bundle.digest(archive), "files": {name: hashlib.sha256(value.encode()).hexdigest()
                                                                 for name, value in files.items()}}
        write_json(crate / ".cargo-checksum.json", checksum)
        lock = self.root / "Cargo.lock"
        write(lock, 'version = 4\n[[package]]\nname="crate"\nversion="1.0.0"\n'
                    'source="registry+https://github.com/rust-lang/crates.io-index"\nchecksum="' + checksum["package"] + '"\n')
        gaps = []
        records = bundle.validate_vendor(vendor, lock, cache, {}, gaps)
        self.assertEqual(len(records), 1)
        self.assertEqual(gaps, [])
        write(crate / "src/lib.rs", "pub fn altered() {}\n")
        checksum["files"]["src/lib.rs"] = bundle.digest(crate / "src/lib.rs")
        write_json(crate / ".cargo-checksum.json", checksum)
        with self.assertRaisesRegex(ValueError, "differ from the locked registry archive"):
            bundle.validate_vendor(vendor, lock, cache, {}, [])

    def test_missing_registry_bytes_and_locked_crates_are_explicit_gaps(self):
        vendor = self.root / "vendor"
        vendor.mkdir()
        lock = self.root / "Cargo.lock"
        write(lock, 'version=4\n[[package]]\nname="absent"\nversion="2.0.0"\n'
                    'source="registry+https://github.com/rust-lang/crates.io-index"\nchecksum="' + "1" * 64 + '"\n')
        gaps = []
        self.assertEqual(bundle.validate_vendor(vendor, lock, None, {}, gaps), [])
        self.assertEqual(gaps, ["rust-vendor:absent@2.0.0: locked package source is missing"])

    def test_reviewed_standard_license_binds_original_manifest_and_separate_text_origin(self):
        # These are explicit contract source fixtures. The retained Apache text
        # is the real pinned standard; no fixture is called an upstream build.
        authored = ('[package]\nname="crate"\nversion="1.0.0"\nlicense="MIT OR Apache-2.0"\n'
                    'authors=["Fixture author"]\nrepository="https://example.invalid/original"\n')
        commit = repository(self.root / "original", {"Cargo.toml": authored, "src/lib.rs": "pub fn original() {}\n"})
        vendor, cache, project = self.root / "vendor", self.root / "cache", self.root / "project"
        vendor.mkdir(); cache.mkdir(); project.mkdir()
        crate = vendor / "crate-1.0.0"
        files = {"Cargo.toml": authored.encode(), "Cargo.toml.orig": authored.encode(),
                 "src/lib.rs": b"pub fn original() {}\n",
                 ".cargo_vcs_info.json": bundle.canonical({"git": {"sha1": commit}, "path_in_vcs": ""})}
        archive = cache / "crate-1.0.0.crate"
        source_archive(archive, files)
        for name, value in files.items():
            write(crate / name, value)
        write_json(crate / ".cargo-checksum.json", {"package": bundle.digest(archive),
                   "files": {name: hashlib.sha256(value).hexdigest() for name, value in files.items()}})
        cargo_lock = project / "Cargo.lock"
        write(cargo_lock, 'version=4\n[[package]]\nname="crate"\nversion="1.0.0"\n'
              'source="registry+https://github.com/rust-lang/crates.io-index"\nchecksum="' + bundle.digest(archive) + '"\n')
        standard_name = "third_party/rust-notices/standard-licenses/Apache-2.0.txt"
        standard = project / standard_name
        write(standard, (ROOT / standard_name).read_bytes())
        original_manifest = crate / "Cargo.toml.orig"
        review = {"name": "crate", "version": "1.0.0", "source": "registry+https://github.com/rust-lang/crates.io-index",
                  "registryChecksum": bundle.digest(archive), "cargoChecksumFileSha256": bundle.digest(crate / ".cargo-checksum.json"),
                  "declaredLicense": "MIT OR Apache-2.0", "declaredRepository": "https://example.invalid/original",
                  "authors": ["Fixture author"], "upstreamRepository": "https://example.invalid/original",
                  "upstreamCommit": commit, "pathInVcs": "", "bindingType": "cargo-vcs-and-authored-files-match",
                  "matchedPublishedFileCount": 2, "cargoGeneratedFiles": [".cargo_vcs_info.json"],
                  "publicationCommitRecorded": True, "selectedLicense": "Apache-2.0",
                  "sourceManifest": {"path": "Cargo.toml.orig", "size": original_manifest.stat().st_size,
                     "sha256": bundle.digest(original_manifest), "gitBlobSha1": bundle.object_digest("blob", authored.encode())},
                  "notices": [{"inputKind": "standard-license", "inputPath": standard_name,
                     "upstreamPath": "text/Apache-2.0.txt", "spdxLicense": "Apache-2.0",
                     "standardRepository": "https://github.com/spdx/license-list-data",
                     "standardCommit": "cc4fe1ac462bd3ad3c81a07234baf7d9541ee871",
                     "size": standard.stat().st_size, "sha256": bundle.digest(standard),
                     "gitBlobSha1": "137069b823873b8bcf42979bcf8e9371052d26a2"}]}
        review_name = "third_party/rust-notices/upstream.json"
        initial = bundle.inventory(vendor)

        def collect_review(name, current):
            write_json(project / review_name, {"schemaVersion": 1, "correspondingSourceComplete": False, "crates": [current]})
            stage = self.root / name
            stage.mkdir()
            collector = bundle.Collector({}, project, stage)
            collector.exported_project, collector.cargo_lock = project, cargo_lock
            records = bundle.validate_vendor(vendor, cargo_lock, cache, {}, collector.gaps)
            self.assertIn("rust-vendor:crate@1.0.0: no retained license/copyright text", collector.gaps)
            collector.collect_rust_notices(vendor, records, review_name)
            return collector, records[0]

        collector, record = collect_review("valid-notice", review)
        self.assertEqual(collector.gaps, [])
        self.assertEqual(bundle.inventory(vendor), initial)
        self.assertEqual(record["noticeProvenance"]["commit"], commit)
        self.assertEqual(record["noticeProvenance"]["authors"], ["Fixture author"])
        self.assertEqual(record["noticeProvenance"]["selectedLicense"], "Apache-2.0")
        notice = record["supplementalNotices"][0]
        self.assertEqual(notice["origin"], "standard-license")
        self.assertEqual(notice["upstreamRepository"], "https://github.com/spdx/license-list-data")
        self.assertEqual((collector.stage / notice["path"]).read_bytes(), standard.read_bytes())
        for field, value, error in (("declaredLicense", "MIT", "exact Cargo source"),
                                   ("authors", ["Invented attribution"], "original crate license, authors"),
                                   ("selectedLicense", "MIT", "Unsupported explicit"),
                                   ("cargoChecksumFileSha256", "0" * 64, "exact Cargo source")):
            changed = copy.deepcopy(review); changed[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, error):
                collect_review("invalid-" + field, changed)
        with standard.open("ab") as stream:
            stream.write(b"changed standard text")
        with self.assertRaisesRegex(ValueError, "notice bytes differ"):
            collect_review("invalid-standard", review)

    def test_pinned_cargo_registry_filter_has_exact_path_boundaries(self):
        omitted = (".git", ".git/config", "nested/.git/HEAD", ".gitignore", "nested/.gitignore",
                   ".gitattributes", "nested/.gitattributes", ".cargo-ok")
        retained = (".GIT/config", ".Gitignore", "nested/.Gitattributes", "nested/.cargo-ok",
                    ".cargo_vcs_info.json", ".github/workflows/ci.yml", ".gitignored", ".gitignore/source.rs",
                    ".cargo-ok/source.rs", "src/lib.rs", "LICENSE")
        for name in omitted:
            with self.subTest(omitted=name):
                self.assertFalse(bundle.cargo_registry_vendor_file(name))
        for name in retained:
            with self.subTest(retained=name):
                self.assertTrue(bundle.cargo_registry_vendor_file(name))
        # The comparison filter does not relax the general archive safety gate.
        archive = self.root / "administrative.crate"
        source_archive(archive, {".git/config": "administrative data"})
        with self.assertRaisesRegex(ValueError, "Git administrative data"):
            bundle.extract_tar(archive, self.root / "unsafe", "crate-1.0.0")

    def test_registry_vendor_checks_all_retained_bytes_after_fixed_cargo_filtering(self):
        vendor, cache = self.root / "vendor", self.root / "cache"
        vendor.mkdir()
        cache.mkdir()
        crate = vendor / "crate-1.0.0"
        kept = {"Cargo.toml": '[package]\nname="crate"\nversion="1.0.0"\nlicense="MIT"\n',
                "src/lib.rs": "pub fn original() {}\n", "LICENSE": "Original test license\n",
                ".cargo_vcs_info.json": '{"git":{"sha1":"' + "1" * 40 + '"}}\n',
                ".github/workflows/ci.yml": "name: Contract fixture\n", "nested/.cargo-ok": "nested source\n",
                "nested/.Gitignore": "Case-sensitive source\n"}
        omitted = {".gitignore": "target/\n", "nested/.gitattributes": "*.rs text\n", ".cargo-ok": "marker\n"}
        archive = cache / "crate-1.0.0.crate"
        source_archive(archive, {**kept, **omitted})
        original_archive = archive.read_bytes()
        lock = self.root / "Cargo.lock"
        write(lock, 'version=4\n[[package]]\nname="crate"\nversion="1.0.0"\n'
                    'source="registry+https://github.com/rust-lang/crates.io-index"\nchecksum="' + bundle.digest(archive) + '"\n')

        def provide(files):
            if crate.exists():
                shutil.rmtree(crate)
            for name, value in files.items():
                write(crate / name, value)
            write_json(crate / ".cargo-checksum.json", {
                "package": bundle.digest(archive),
                "files": {name: hashlib.sha256(value.encode()).hexdigest() for name, value in files.items()},
            })

        # This is an explicit archive/directory contract fixture, not a claim
        # that the test executes Cargo. Both original and retained views are real.
        provide(kept)
        gaps = []
        self.assertEqual(len(bundle.validate_vendor(vendor, lock, cache, {}, gaps)), 1)
        self.assertEqual(gaps, [])
        self.assertEqual(archive.read_bytes(), original_archive)
        stage = self.root / "retained"
        stage.mkdir()
        collector = bundle.Collector({"rustVendor": {"directory": str(vendor), "registryCache": str(cache)}},
                                     self.root, stage)
        collector.cargo_lock = lock
        collector.lock = {"qtMigration": {"rustToolchain": "unverified-next-toolchain"}}
        with self.assertRaisesRegex(ValueError, "pinned to Cargo 1.97.1"):
            collector.collect_rust()
        collector.lock["qtMigration"]["rustToolchain"] = "1.97.1"
        collector.collect_rust()
        source = collector.components["rust-vendor"]["source"]
        self.assertEqual(source["registryComparisonPolicy"]["cargoVersion"], source["toolchain"])
        self.assertEqual(source["registryComparisonPolicy"]["cargoCommit"], "c980f4866141969fab6254a680546a277789d6f0")
        self.assertEqual((stage / source["registryArchives"][0]["archive"]["path"]).read_bytes(), original_archive)
        self.assertNotIn("replay", source)
        self.assertIn("rust-vendor: actual offline replay with the pinned Cargo toolchain is missing", collector.gaps)
        for name in ("src/lib.rs", "LICENSE", ".cargo_vcs_info.json", "nested/.cargo-ok"):
            with self.subTest(missing=name):
                provide({key: value for key, value in kept.items() if key != name})
                with self.assertRaisesRegex(ValueError, "missing=1") as error:
                    bundle.validate_vendor(vendor, lock, cache, {}, [])
                self.assertIn(repr(name), str(error.exception))
        for name in ("src/lib.rs", "nested/.Gitignore"):
            with self.subTest(changed=name):
                provide({**kept, name: "Rewritten bytes and checksum\n"})
                with self.assertRaisesRegex(ValueError, "changed=1") as error:
                    bundle.validate_vendor(vendor, lock, cache, {}, [])
                self.assertIn(repr(name), str(error.exception))
        for name in (".gitignore", "unexpected.rs"):
            with self.subTest(unexpected=name):
                provide({**kept, name: "Not in the fixed vendor view\n"})
                with self.assertRaisesRegex(ValueError, "unexpected=1") as error:
                    bundle.validate_vendor(vendor, lock, cache, {}, [])
                self.assertIn(repr(name), str(error.exception))
        provide({**kept, **{f"extra-{index:02d}.rs": "extra\n" for index in range(12)}})
        with self.assertRaisesRegex(ValueError, "unexpected=12") as error:
            bundle.validate_vendor(vendor, lock, cache, {}, [])
        self.assertIn("extra-04.rs", str(error.exception))
        self.assertNotIn("extra-05.rs", str(error.exception))
        self.assertEqual(archive.read_bytes(), original_archive)

    def test_unsafe_and_duplicate_archive_entries_are_rejected(self):
        for index, member in enumerate(("../outside.c", "/absolute.c", "a/../../outside.c")):
            archive = self.root / (str(index) + ".tar")
            with tarfile.open(archive, "w") as stream:
                info = tarfile.TarInfo(member)
                info.size = 1
                stream.addfile(info, io.BytesIO(b"x"))
            with self.assertRaisesRegex(ValueError, "Unsafe relative path"):
                bundle.extract_tar(archive, self.root / ("out" + str(index)))
        archive = self.root / "links.tar"
        with tarfile.open(archive, "w") as stream:
            info = tarfile.TarInfo("link")
            info.type, info.linkname = tarfile.SYMTYPE, "../outside"
            stream.addfile(info)
        with self.assertRaisesRegex(ValueError, "escapes"):
            bundle.extract_tar(archive, self.root / "link-out")
        source_archive(self.root / "collision.tar.gz", {"Code.c": "a", "code.c": "b"}, "source")
        with self.assertRaisesRegex(ValueError, "case-colliding"):
            bundle.extract_tar(self.root / "collision.tar.gz", self.root / "case-out", "source")

    def test_generated_archive_is_deterministic_and_preserves_internal_links(self):
        source = self.root / "source"
        write(source / "LICENSE", "Fixture license\n")
        write(source / "bin/configure", "#!/bin/sh\nexit 0\n")
        (source / "bin/configure").chmod(0o755)
        if os.name != "nt":
            os.symlink("../LICENSE", source / "bin/license-link")
        first = bundle.make_archive(source, self.root / "one.tar.gz", 1_700_000_000)
        os.utime(source / "LICENSE", (100, 200))
        second = bundle.make_archive(source, self.root / "two.tar.gz", 1_700_000_000)
        self.assertEqual(first["sha256"], second["sha256"])
        with self.assertRaises(FileExistsError):
            bundle.make_archive(source, self.root / "one.tar.gz", 1_700_000_000)

    @unittest.skipIf(os.name == "nt", "Source symlink chain construction requires Unix symlink privileges")
    def test_resolved_link_chain_cannot_escape_a_component_or_loop(self):
        source = self.root / "source"
        source.mkdir()
        os.symlink(".", source / "d")
        os.symlink("d/..", source / "escape")
        with self.assertRaisesRegex(ValueError, "Resolved source link escapes"):
            bundle.inventory(source)
        archive = self.root / "chain.tar"
        with tarfile.open(archive, "w") as stream:
            for name, target in (("d", "."), ("escape", "d/..")):
                info = tarfile.TarInfo(name)
                info.type, info.linkname = tarfile.SYMTYPE, target
                stream.addfile(info)
        with self.assertRaisesRegex(ValueError, "Resolved source link escapes"):
            bundle.extract_tar(archive, self.root / "out")
        (source / "escape").unlink()
        os.symlink("absent", source / "dangling")
        with self.assertRaisesRegex(ValueError, "Dangling or cyclic"):
            bundle.inventory(source)
        (source / "dangling").unlink()
        os.symlink("b", source / "a")
        os.symlink("a", source / "b")
        with self.assertRaisesRegex(ValueError, "Dangling or cyclic"):
            bundle.inventory(source)

    @unittest.skipIf(os.name == "nt", "Source directory symlink construction requires Unix symlink privileges")
    def test_actual_checkout_cannot_redirect_a_tracked_directory(self):
        source = self.root / "source"
        commit = repository(source, {"code/value.c": "int value;\n"})
        _, files = bundle.export_git(source, commit, self.root / "export")
        (source / "code").rename(self.root / "external-code")
        os.symlink(self.root / "external-code", source / "code")
        write(source / ".git/info/exclude", "code\n")
        with self.assertRaisesRegex(ValueError, "symlinked directory"):
            bundle.compare_checkout(source, files, commit)

    def test_vendor_config_retains_exact_git_replacement_and_is_relocatable(self):
        sha = "a" * 40
        config = self.root / "config.toml"
        write(config, '[source.crates-io]\nreplace-with="vendored-sources"\n'
                      '[source.git-source]\ngit="https://github.com/ImageOptim/gifski.git"\nrev="' + sha + '"\n'
                      'replace-with="vendored-sources"\n[source.vendored-sources]\ndirectory="/original/vendor"\n')
        records = [{"source": "registry+https://github.com/rust-lang/crates.io-index"},
                   {"source": "git+https://github.com/ImageOptim/gifski.git?rev=" + sha + "#" + sha}]
        result = bundle.validate_cargo_config(config, records).decode()
        self.assertIn('directory = "../rust-vendor"', result)
        self.assertIn("offline = true", result)
        write(config, config.read_text().replace(sha, "b" * 40))
        with self.assertRaisesRegex(ValueError, "exact locked Git source"):
            bundle.validate_cargo_config(config, records)

    def project_fixture(self):
        dep = self.root / "dependency"
        commit = repository(dep, {"source.c": "int dependency;\n", "LICENSE": "Fixture license\n"})
        pin = {"repository": "https://example.invalid/fixture.git", "commit": commit, "configure": ["--disable-example"]}
        cargo_lock = b"version = 4\n"
        lock = {"gifski": pin, "qtMigration": {
            "qt": {"qtbaseRepository": pin["repository"], "qtbaseCommit": commit, "version": "6.12.1"},
            "localsend": pin, "libportal": pin, "pipewire": {**pin, "mesonOptions": ["-Dtests=false"]},
            "softwareMedia": {"x264": pin, "ffmpeg": pin},
            "videoInspectionWindows": {"configure": ["--disable-example", "--enable-swscale"]},
            "nativeRust": {"cargoLockSha256": hashlib.sha256(cargo_lock).hexdigest()}, "rustToolchain": "1.97.1"}}
        project = self.root / "project"
        project_commit = repository(project, {
            "LICENSE": "Project fixture license\n", "source.c":
            '#ifndef BUILD_IDENTITY\n#define BUILD_IDENTITY "{}"\n#endif\n'
            'volatile const char identity[] = "HUHU_BUILD_IDENTITY_V1\\0" BUILD_IDENTITY "\\0HUHU_BUILD_IDENTITY_END";\n'
            "int main(void) { return identity[0] == 'H' ? 0 : 1; }\n",
            "build/version.json": bundle.canonical({"schemaVersion": 1, "version": "0.0.5"}),
            "build/dependencies.lock.json": bundle.canonical(lock),
            "build/package/dependencies.lock.json": bundle.canonical({"tools": {"appimageRuntime": pin}}),
            "third_party/localsend/Cargo.lock": cargo_lock,
            "third_party/localsend/upstream.json": bundle.canonical({"commit": commit}),
            "third_party/localsend/LICENSE": "Local crate fixture license\n",
            "third_party/localsend/patches/native-core.patch": "Fixture provenance only\n"})
        return project, project_commit, dep

    def test_incomplete_real_collection_never_claims_names_are_completeness(self):
        project, commit, dep = self.project_fixture()
        plan = {"schemaVersion": 1, "project": str(project), "platform": "linux",
                "sources": [{"id": "ffmpeg", "directory": str(dep)}]}
        path = self.root / "plan.json"
        write_json(path, plan)
        result = bundle.collect(path, self.root / "out")
        self.assertEqual(result["sourceCommit"], commit)
        self.assertFalse(result["complete"])
        self.assertTrue(any(gap.startswith("qtwayland:") for gap in result["gaps"]))
        self.assertTrue(any(gap.startswith("appimage-musl:") for gap in result["gaps"]))
        self.assertIn("appimage-mimalloc: required source component is missing", result["gaps"])
        self.assertTrue(any(gap.startswith("deployment:") for gap in result["gaps"]))
        self.assertEqual(result["architecture"], "amd64")
        self.assertEqual(result["archive"]["name"], "huhucapture-v0.0.5-linux-amd64-sources.tar.gz")
        self.assertTrue((self.root / "out/huhucapture-v0.0.5-linux-amd64-source-manifest.json").is_file())
        self.assertEqual(result["archive"]["sha256"], bundle.digest(self.root / "out" / result["archive"]["name"]))
        with self.assertRaisesRegex(ValueError, "existing results"):
            bundle.collect(path, self.root / "out")
        write_json(path, {**plan, "complete": True})
        with self.assertRaisesRegex(ValueError, "computed"):
            bundle.collect(path, self.root / "forged")

    def test_locked_runtime_tar_keeps_source_separate_from_build_closure(self):
        project, _, _ = self.project_fixture()
        original = self.root / "runtime-source.tar.gz"
        source_archive(original, {"src/runtime/runtime.c": "int runtime_fixture;\n",
                                  "LICENSE": "Runtime source fixture license\n"}, prefix="runtime-fixture")
        package_lock = project / "build/package/dependencies.lock.json"
        value = json.loads(package_lock.read_text())
        value["tools"]["appimageRuntime"]["sourceArchive"] = bundle.descriptor(original)
        write_json(package_lock, value)
        command(project, "git", "add", "build/package/dependencies.lock.json")
        command(project, "git", "commit", "--quiet", "-m", "Pin actual runtime source fixture archive")
        plan = {"schemaVersion": 1, "project": str(project), "platform": "linux", "sources": [
            {"id": "appimage-runtime", "kind": "tar", "archive": str(original), "prefix": "runtime-fixture"}]}
        path = self.root / "runtime-plan.json"
        write_json(path, plan)
        result = bundle.collect(path, self.root / "runtime-out")
        runtime = next(component for component in result["components"] if component["id"] == "appimage-runtime")
        self.assertEqual(runtime["source"]["archiveSha256"], bundle.digest(original))
        self.assertEqual(runtime["source"]["lockFile"], "sources/huhucapture/build/package/dependencies.lock.json")
        self.assertIn("LICENSE", runtime["licenses"])
        self.assertNotIn("appimage-runtime: required source component is missing", result["gaps"])
        self.assertIn("appimage-mimalloc: required source component is missing", result["gaps"])
        self.assertIn("appimage-runtime: matching source/build-parameter receipt is missing", result["gaps"])
        self.assertIn("appimage-runtime: matching built runtime/static-input closure receipt is missing", result["gaps"])
        self.assertFalse(result["complete"])
        self.assertEqual(result["buildReceipts"], {})

    def test_runtime_static_input_requirement_includes_mimalloc_and_runtime_binding(self):
        # This tests the final presence rule after receipt validation. These are
        # contract values, not invented files or an asserted official build.
        previous_five = {"appimage-musl", "appimage-fuse", "appimage-squashfuse", "appimage-zstd", "appimage-zlib"}
        receipt = {"sources": [{"id": "appimage-runtime"}],
                   "staticInputs": [{"component": name} for name in sorted(previous_five)]}
        self.assertFalse(bundle.appimage_static_inputs_present({"contract": receipt}))
        receipt["staticInputs"].append({"component": "appimage-mimalloc"})
        self.assertTrue(bundle.appimage_static_inputs_present({"contract": receipt}))
        receipt["sources"] = [{"id": "another-built-component"}]
        self.assertFalse(bundle.appimage_static_inputs_present({"contract": receipt}))

    def test_dirty_project_and_missing_exact_pin_are_not_silently_exported(self):
        project, _, dep = self.project_fixture()
        plan = {"schemaVersion": 1, "project": str(project), "platform": "windows",
                "sources": [{"id": "extra-source", "directory": str(dep)}]}
        path = self.root / "plan.json"
        write_json(path, plan)
        result = bundle.collect(path, self.root / "out")
        self.assertIn("extra-source: exact source pin is missing", result["gaps"])
        write(project / "source.c", "int changed;\n")
        with self.assertRaisesRegex(ValueError, "unrecorded source change"):
            bundle.collect(path, self.root / "dirty")

    @unittest.skipIf(os.name == "nt", "This actual C compiler/ELF byte-binding gate is for Linux")
    def test_actual_compiled_output_must_match_source_receipt_and_final_inventory(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.fail("The source/output binding gate requires the actual host C compiler")
        project, commit, _ = self.project_fixture()
        identity = {"schemaVersion": 1, "product": "huhucapture", "repository": "NAMEWTA/huhucapture",
                    "version": "0.0.5", "tag": "v0.0.5", "sourceCommit": commit, "sourceDirty": False,
                    "platform": "linux", "architecture": "amd64", "configuration": "Release", "qtVersion": "6.12.1",
                    "dependencyLockSha256": bundle.digest(project / "build/dependencies.lock.json"),
                    "cargoLockSha256": bundle.digest(project / "third_party/localsend/Cargo.lock")}
        identity_path = self.root / "build-identity.json"
        write_json(identity_path, identity)
        stage = self.root / "stage"
        stage.mkdir()
        collector = bundle.Collector({"project": str(project), "platform": "linux", "buildIdentity": str(identity_path)}, self.root, stage)
        collector.collect_project()
        collector.retain("package-metadata-helper", str(ROOT / "build/package/metadata.py"))
        installed = self.root / "installed"
        (installed / "bin").mkdir(parents=True)
        executable = installed / "bin/huhucapture"
        args = [compiler, str(project / "source.c"), "-DBUILD_IDENTITY=" + json.dumps(json.dumps(identity)), "-o", str(executable)]
        subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        output = bundle.descriptor(executable)
        write_json(installed / "bin/build-identity.json", identity)
        sidecar = bundle.descriptor(installed / "bin/build-identity.json")
        receipt = {"schemaVersion": 1, "kind": "huhucapture-source-build-receipt", "platform": "linux",
                   "commands": [args], "toolchain": {"cc": compiler}, "environment": {},
                   "sources": [{"id": "huhucapture", "sourceTreeSha256": collector.components["huhucapture"]["sourceTreeSha256"]}],
                   "staticInputs": [], "outputs": [
                       {"path": "bin/huhucapture", "size": output["size"], "sha256": output["sha256"]},
                       {"path": "bin/build-identity.json", "size": sidecar["size"], "sha256": sidecar["sha256"]}]}
        receipt_path = self.root / "receipt.json"
        write_json(receipt_path, receipt)
        collector.plan["buildReceipts"] = [{"id": "real-compiler-test", "path": str(receipt_path)}]
        collector.collect_receipts()
        inventory_path, map_path = self.root / "inventory.json", self.root / "map.json"
        write_json(inventory_path, bundle.inventory(installed))
        write_json(map_path, {name: {"receipt": "real-compiler-test", "output": name}
                              for name in ("bin/huhucapture", "bin/build-identity.json")})
        collector.plan["deployment"] = {"directory": str(installed), "inventory": str(inventory_path), "sourceMap": str(map_path)}
        collector.collect_deployment()
        self.assertFalse(list(stage.rglob("__pycache__")), "Retained metadata execution contaminated source evidence")
        self.assertTrue(any("actual aggregated Rust" in gap for gap in collector.gaps))
        def clear_deployment_materials():
            for name in ("deployment-inventory", "deployment-source-map"):
                record = collector.materials.pop(name, None)
                if record:
                    shutil.rmtree((stage / record["path"]).parent)
        single_specification = dict(collector.plan["deployment"])
        single_mapping = bundle.read_json(map_path)
        payloads = self.root / "payloads"
        products = [{"root": "deb/opt/huhucapture", "executable": "bin/huhucapture", "packageKind": "deb"},
                    {"root": "appimage/usr", "executable": "bin/huhucapture", "packageKind": "appimage"}]
        multiple_mapping = {}
        for product in products:
            product_root = payloads / product["root"]
            shutil.copytree(installed, product_root)
            write_json(product_root / "bin/installation.json", {"schemaVersion": 1, "product": "huhucapture",
                       "version": "0.0.5", "platform": "linux", "architecture": "amd64", "packageKind": product["packageKind"]})
            multiple_mapping.update({product["root"] + "/" + name: origin for name, origin in single_mapping.items()})
        collector.plan["deployment"] = {"directory": str(payloads), "products": products,
                                         "inventory": str(inventory_path), "sourceMap": str(map_path)}
        clear_deployment_materials()
        write_json(inventory_path, bundle.inventory(payloads)); write_json(map_path, multiple_mapping)
        deployment = collector.collect_deployment()
        self.assertEqual(deployment["products"], sorted(products, key=lambda product: product["root"]))
        self.assertEqual(deployment["files"], bundle.inventory(payloads))
        self.assertEqual(deployment["treeSha256"], bundle.tree_digest(deployment["files"]))
        self.assertTrue(any("incomplete for appimage/usr:" in gap for gap in collector.gaps))
        self.assertTrue(any("incomplete for deb/opt/huhucapture:" in gap for gap in collector.gaps))
        # The two real ELF identities are checked independently; neither a
        # changed second output nor a reused package marker can borrow the first.
        deb_binary = payloads / "deb/opt/huhucapture/bin/huhucapture"
        with deb_binary.open("ab") as stream:
            stream.write(b"changed second product output")
        clear_deployment_materials(); write_json(inventory_path, bundle.inventory(payloads))
        with self.assertRaisesRegex(ValueError, "differs from its source build receipt"):
            collector.collect_deployment()
        marker = payloads / "deb/opt/huhucapture/bin/installation.json"
        write_json(marker, {**bundle.read_json(marker), "packageKind": "appimage"})
        clear_deployment_materials()
        with self.assertRaisesRegex(ValueError, "different package kind"):
            collector.collect_deployment()
        collector.plan["deployment"] = single_specification
        clear_deployment_materials()
        write_json(inventory_path, bundle.inventory(installed)); write_json(map_path, single_mapping)
        valid_map = bundle.read_json(map_path)
        mixed = {key: dict(value) for key, value in valid_map.items()}
        mixed["bin/huhucapture"].update(component="huhucapture", path="source.c")
        clear_deployment_materials()
        write_json(map_path, mixed)
        with self.assertRaisesRegex(ValueError, "exactly the receipt/output"):
            collector.collect_deployment()
        clear_deployment_materials()
        write_json(map_path, valid_map)
        original_metadata = bundle.packaging_metadata
        def mutate_after_initial_inventory(path=None):
            with executable.open("ab") as stream:
                stream.write(b"changed during validation")
            return original_metadata(path)
        with mock.patch.object(bundle, "packaging_metadata", side_effect=mutate_after_initial_inventory):
            with self.assertRaisesRegex(ValueError, "tree changed during"):
                collector.collect_deployment()
        with executable.open("ab") as stream:
            stream.write(b"altered")
        write_json(inventory_path, bundle.inventory(installed))
        # Use a new collector-owned material identity to exercise the binary hash
        # comparison independently of the already-retained inventory identity.
        clear_deployment_materials()
        with self.assertRaisesRegex(ValueError, "differs from its source build receipt"):
            collector.collect_deployment()

    def test_empty_or_unrelated_deployment_cannot_satisfy_completion(self):
        stage, deployment = self.root / "stage", self.root / "deployment"
        stage.mkdir()
        deployment.mkdir()
        collector = bundle.Collector({"deployment": {"directory": str(deployment)}}, self.root, stage)
        collector.platform = "linux"
        with self.assertRaisesRegex(ValueError, "deployment is empty"):
            collector.collect_deployment()
        write(deployment / "unrelated-file", "not an installed product\n")
        with self.assertRaisesRegex(ValueError, "no actual huhucapture executable"):
            collector.collect_deployment()

    def test_deployment_product_roots_cannot_overlap_or_escape(self):
        stage, deployment = self.root / "stage", self.root / "payloads"
        stage.mkdir(); deployment.mkdir()
        write(deployment / "fixture.txt", "Only a path-validation fixture; not a product\n")
        collector = bundle.Collector({"deployment": {"directory": str(deployment)}}, self.root, stage)
        collector.platform = "linux"
        for roots, error in ((["app", "app/usr"], "roots overlap"),
                             (["../outside", "deb"], "Unsafe relative path")):
            collector.plan["deployment"]["products"] = [
                {"root": name, "executable": "bin/huhucapture", "packageKind": kind}
                for name, kind in zip(roots, ("appimage", "deb"))]
            with self.subTest(roots=roots), self.assertRaisesRegex(ValueError, error):
                collector.collect_deployment()

    def test_source_names_cannot_collide_between_platform_outputs(self):
        windows = bundle.source_output_names("1.2.3", "windows")
        linux = bundle.source_output_names("1.2.3", "linux")
        self.assertEqual(windows, ("huhucapture-v1.2.3-windows-x64-sources.tar.gz",
                                  "huhucapture-v1.2.3-windows-x64-source-manifest.json"))
        self.assertEqual(linux, ("huhucapture-v1.2.3-linux-amd64-sources.tar.gz",
                                "huhucapture-v1.2.3-linux-amd64-source-manifest.json"))
        self.assertFalse(set(windows) & set(linux))

    def test_source_spdx_checks_real_file_bytes(self):
        source = self.root / "source"
        write(source / "source.c", "int value;\n")
        spdx = self.root / "source.spdx"
        write(spdx, "SPDXVersion: SPDX-2.1\nFileName: ./source.c\nFileChecksum: SHA1: " +
                    bundle.digest(source / "source.c", "sha1") + "\n")
        self.assertEqual(bundle.verify_source_spdx(spdx, source), 1)
        write(source / "source.c", "int altered;\n")
        with self.assertRaisesRegex(ValueError, "Source SPDX byte mismatch"):
            bundle.verify_source_spdx(spdx, source)


if __name__ == "__main__":
    unittest.main()
