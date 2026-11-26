#!/usr/bin/env python3
"""Check the actual single version generator and native Windows build guards."""
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
        for relative in ("build/version.json", "build/windows/Version.props.in"):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        self.source({"schemaVersion": 1, "version": "0.0.5"})
        version_source.write_generated(self.root)

    def source(self, value):
        write(self.root / "build/version.json", json.dumps(value, indent=2) + "\n")

    def assert_stale(self):
        with self.assertRaises(ValueError):
            version_source.read_version(self.root)

    def test_source_is_propagated_to_actual_generated_props(self):
        self.assertEqual(version_source.read_version(ROOT), json.loads((ROOT / "build/version.json").read_text())["version"])
        self.assertEqual(version_source.read_version(self.root), "0.0.5")
        props = (self.root / "build/windows/Version.props").read_text()
        self.assertIn("<VersionPatch Condition=", props)
        self.assertIn(">5</VersionPatch>", props)
        self.assertIn(">0.0.5</_HuhuGeneratedProductVersion>", props)
        self.assertEqual((self.root / "VERSION").read_bytes(), b"0.0.5\n")

    def test_changed_source_requires_explicit_regeneration(self):
        self.source({"schemaVersion": 1, "version": "0.0.6"})
        self.assert_stale()
        self.assertEqual((self.root / "VERSION").read_bytes(), b"0.0.5\n")
        self.assertEqual(version_source.write_generated(self.root), "0.0.6")
        self.assertEqual(version_source.read_version(self.root), "0.0.6")
        props = (self.root / "build/windows/Version.props").read_text()
        self.assertIn(">6</VersionPatch>", props)
        self.assertIn(">0.0.6</_HuhuGeneratedProductVersion>", props)

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

    def test_invalid_versions_never_replace_derived_files(self):
        original = (self.root / "build/windows/Version.props").read_bytes()
        for value in ("01.0.5", "0.00.5", "0.0.05", "0.0.65536", "-1.0.5", "0.0.5.0",
                      "v0.0.5", "0.0.5-preview", "0.0.5\n", "", 5, None):
            with self.subTest(value=value):
                self.source({"schemaVersion": 1, "version": value})
                with self.assertRaises(ValueError):
                    version_source.write_generated(self.root)
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

    def test_missing_output_is_not_regenerated_by_a_build(self):
        props = self.root / "build/windows/Version.props"
        props.unlink()
        self.assert_stale()
        self.assertFalse(props.exists())
        version_source.write_generated(self.root)

    @unittest.skipUnless(os.name == "nt", "Native MSBuild and PowerShell checks run on the Windows build host")
    def test_native_windows_readers_and_overrides(self):
        msbuild = shutil.which("MSBuild.exe")
        pwsh = shutil.which("pwsh.exe")
        if not msbuild:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
            self.assertTrue(vswhere.is_file(), "The installed Visual Studio discovery tool is required")
            installation = subprocess.run([str(vswhere), "-latest", "-products", "*",
                "-requires", "Microsoft.Component.MSBuild", "-property", "installationPath"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout.strip()
            candidate = Path(installation) / "MSBuild/Current/Bin/MSBuild.exe"
            self.assertTrue(installation and candidate.is_file(), "An actual installed MSBuild is required")
            msbuild = str(candidate)
        self.assertTrue(msbuild and pwsh, "Windows version validation requires actual MSBuild and PowerShell")
        write(self.root / "validate.proj", '''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<Import Project="build/windows/Version.props" />
<Target Name="PrepareForBuild" /><Target Name="ResourceCompile" />
<Target Name="Build" DependsOnTargets="PrepareForBuild;ResourceCompile" />
</Project>
''')
        for relative in ("scripts/BuildCommon.ps1", "build/dependencies.lock.json"):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        write(self.root / "validate.ps1", ". (Join-Path $PSScriptRoot 'scripts/BuildCommon.ps1')\nGet-ProductVersion\n")

        def check(success, *properties):
            result = subprocess.run([msbuild, str(self.root / "validate.proj"), "/nologo", "/v:minimal", *properties],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode == 0, success, result.stdout)

        def powershell(success):
            result = subprocess.run([pwsh, "-NoProfile", "-File", str(self.root / "validate.ps1")],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode == 0, success, result.stdout)

        check(True)
        powershell(True)
        check(False, "/p:VersionPatch=6")
        check(False, "/p:VersionRevision=1")
        self.source({"schemaVersion": 1, "version": "0.0.6"})
        check(False)
        powershell(False)
        version_source.write_generated(self.root)
        check(True)
        powershell(True)
        props = self.root / "build/windows/Version.props"
        write(props, props.read_text().replace("0.0.6", "0.0.7").replace(">6</VersionPatch>", ">7</VersionPatch>"))
        write(self.root / "VERSION", "0.0.7\n")
        check(False)
        powershell(False)


if __name__ == "__main__":
    unittest.main()
