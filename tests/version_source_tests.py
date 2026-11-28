#!/usr/bin/env python3
"""Check the real version generator, CMake reader and native MSBuild guard."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("version_source", ROOT / "scripts/version_source.py")
version_source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version_source)


def write(path, text):
    path.write_bytes(text.encode("utf-8"))


class VersionSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="huhu-version-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative in ("build/version.json", "build/windows/Version.props.in",
                         "build/cmake/version.cmake", "src/app/Version.h.in"):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        write(self.root / "CMakeLists.txt", '''cmake_minimum_required(VERSION 3.25)
include(build/cmake/version.cmake)
project(version_contract VERSION "${HUHUCAPTURE_VERSION}" LANGUAGES NONE)
configure_file(src/app/Version.h.in generated/Version.h @ONLY)
file(WRITE "${CMAKE_BINARY_DIR}/actual-version.txt" "${PROJECT_VERSION}")
''')
        self.source({"schemaVersion": 1, "version": "0.0.5"})
        version_source.write_generated(self.root)
        self.cmake = os.environ.get("HUHU_CMAKE") or shutil.which("cmake")
        self.assertTrue(self.cmake, "The actual CMake executable is required for this build contract")

    def configure(self, success):
        result = subprocess.run([self.cmake, "-S", str(self.root), "-B", str(self.root / "output")],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode == 0, success, result.stdout)
        return result.stdout

    def source(self, value):
        write(self.root / "build/version.json", json.dumps(value, indent=2) + "\n")

    def assert_stale(self):
        with self.assertRaises(ValueError):
            version_source.read_version(self.root)
        self.configure(False)

    def test_source_is_propagated_to_actual_generated_header(self):
        self.assertEqual(version_source.read_version(ROOT), json.loads((ROOT / "build/version.json").read_text())["version"])
        self.assertEqual(version_source.read_version(self.root), "0.0.5")
        self.configure(True)
        header = (self.root / "output/generated/Version.h").read_text()
        self.assertIn('text{"0.0.5"}', header)
        self.assertIn("patch{5}", header)
        self.assertEqual((self.root / "output/actual-version.txt").read_text(), "0.0.5")

    def test_changed_source_requires_explicit_regeneration(self):
        self.source({"schemaVersion": 1, "version": "0.0.6"})
        self.assert_stale()
        self.assertEqual((self.root / "VERSION").read_bytes(), b"0.0.5\n")
        self.assertEqual(version_source.write_generated(self.root), "0.0.6")
        self.assertEqual(version_source.read_version(self.root), "0.0.6")
        self.configure(True)
        header = (self.root / "output/generated/Version.h").read_text()
        self.assertIn('text{"0.0.6"}', header)
        self.assertIn("patch{6}", header)

    def test_compatibility_file_cannot_be_an_independent_version(self):
        write(self.root / "VERSION", "0.0.6\n")
        self.assert_stale()

    def test_edited_props_and_compatibility_values_are_rejected(self):
        props = self.root / "build/windows/Version.props"
        write(props, props.read_text().replace("0.0.5", "0.0.6").replace(">5</VersionPatch>", ">6</VersionPatch>"))
        write(self.root / "VERSION", "0.0.6\n")
        self.assert_stale()

    def test_changed_template_requires_regeneration(self):
        template = self.root / "build/windows/Version.props.in"
        write(template, template.read_text().replace("Do not edit.", "Generated file; do not edit."))
        self.assert_stale()
        version_source.write_generated(self.root)
        self.configure(True)

    def test_invalid_versions_never_replace_derived_files(self):
        original = (self.root / "build/windows/Version.props").read_bytes()
        for value in ("01.0.5", "0.00.5", "0.0.05", "0.0.65536", "-1.0.5", "0.0.5.0",
                      "v0.0.5", "0.0.5-preview", "0.0.5\n", "", 5, None):
            with self.subTest(value=value):
                self.source({"schemaVersion": 1, "version": value})
                with self.assertRaises(ValueError):
                    version_source.write_generated(self.root)
                self.configure(False)
                self.assertEqual((self.root / "VERSION").read_bytes(), b"0.0.5\n")
                self.assertEqual((self.root / "build/windows/Version.props").read_bytes(), original)

    def test_schema_and_duplicate_keys_are_rejected(self):
        for document in ('{"schemaVersion":1,"version":"0.0.5","version":"0.0.6"}',
                         '{"schemaVersion":2,"version":"0.0.5"}',
                         '{"schemaVersion":true,"version":"0.0.5"}',
                         '{"schemaVersion":1,"version":"0.0.5","revision":0}',
                         '{"schemaVersion":1}', '[]', '{'):
            with self.subTest(document=document):
                write(self.root / "build/version.json", document)
                with self.assertRaises(ValueError):
                    version_source.write_generated(self.root)
                self.configure(False)

    def test_missing_output_is_not_regenerated_by_a_build(self):
        props = self.root / "build/windows/Version.props"
        props.unlink()
        self.assert_stale()
        self.assertFalse(props.exists())
        version_source.write_generated(self.root)
        self.configure(True)

    @unittest.skipUnless(os.name == "nt", "The generated MSBuild property contract runs on Windows")
    def test_native_windows_readers_and_overrides(self):
        msbuild = shutil.which("MSBuild.exe")
        if not msbuild:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
            self.assertTrue(vswhere.is_file(), "The installed Visual Studio discovery tool is required")
            installation = subprocess.run([str(vswhere), "-latest", "-products", "*",
                "-requires", "Microsoft.Component.MSBuild", "-property", "installationPath"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout.strip()
            candidate = Path(installation) / "MSBuild/Current/Bin/MSBuild.exe"
            self.assertTrue(installation and candidate.is_file(), "An actual installed MSBuild is required")
            msbuild = str(candidate)
        write(self.root / "validate.proj", '''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<Import Project="build/windows/Version.props" />
<Target Name="PrepareForBuild" /><Target Name="ResourceCompile" />
<Target Name="Build" DependsOnTargets="PrepareForBuild;ResourceCompile" />
</Project>
''')
        def check(success, *properties):
            result = subprocess.run([msbuild, str(self.root / "validate.proj"), "/nologo", "/v:minimal", *properties],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode == 0, success, result.stdout)

        check(True)
        check(False, "/p:VersionPatch=6")
        check(False, "/p:VersionRevision=1")
        self.source({"schemaVersion": 1, "version": "0.0.6"})
        check(False)
        version_source.write_generated(self.root)
        check(True)
        props = self.root / "build/windows/Version.props"
        write(props, props.read_text().replace("0.0.6", "0.0.7").replace(">6</VersionPatch>", ">7</VersionPatch>"))
        write(self.root / "VERSION", "0.0.7\n")
        check(False)


if __name__ == "__main__":
    unittest.main()
