#!/usr/bin/env python3
"""Real source-notice, ELF mismatch and dpkg attribution checks; no product claim."""
from __future__ import annotations

import argparse
import copy
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
specification = importlib.util.spec_from_file_location('runtime_materials', ROOT / 'scripts/collect-runtime-materials.py')
materials = importlib.util.module_from_spec(specification)
specification.loader.exec_module(materials)
qt_materials = materials.module('huhucapture_qt_materials_tests', ROOT / 'build/package/qt_materials.py')
rust_materials = materials.module('huhucapture_rust_materials_tests', ROOT / 'build/package/rust_materials.py')


def host_library():
    cache = materials.loader_cache()
    paths = cache.get('libz.so.1', [])
    if not paths:
        raise AssertionError('The real Linux validation runner must provide zlib through its loader cache')
    return paths[0], cache


class ProjectNotices(unittest.TestCase):
    def test_original_notice_bytes_and_previous_results_survive(self):
        with tempfile.TemporaryDirectory() as directory:
            output = materials.new_output(Path(directory) / 'retained')
            records = materials.project_notices(output)
            paths = {record['source'] for record in records}
            required = {
                'third_party/kwayland-parent/LICENSES/LGPL-2.0-or-later.txt',
                'third_party/kwayland-activation/LICENSES/LGPL-2.0-or-later.txt',
                'third_party/ffmpeg-examples/demux-decode-license.txt',
                'third_party/ffmpeg-examples/remux-license.txt',
                'third_party/wayland-protocols/xdg-activation-v1.xml',
                'third_party/wayland-protocols/xdg-foreign-unstable-v2.xml',
            }
            self.assertLessEqual(required, paths)
            for record in records:
                original, retained = ROOT / record['source'], output / record['path']
                self.assertEqual(retained.read_bytes(), original.read_bytes())
                self.assertEqual(materials.sdk.digest(retained), record['sha256'])
            snapshot = (output / 'project-notices.json').read_bytes()
            self.assertIs(json.loads(snapshot)['correspondingSourceComplete'], False)
            with self.assertRaisesRegex(ValueError, 'earlier evidence'):
                materials.new_output(output)
            self.assertEqual(snapshot, (output / 'project-notices.json').read_bytes())

    def test_actual_cmake_installs_the_same_previously_omitted_notices(self):
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake, 'The real CMake executable is required for source-notice installation')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, build, installed = root / 'source', root / 'build', root / 'installed'
            source.mkdir()
            helper = (ROOT / 'build/cmake/runtime-notices.cmake').as_posix()
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.25)\nproject(NoticeInstallation NONE)\n'
                f'include("{helper}")\nhuhucapture_install_runtime_notices("licenses")\n', encoding='utf-8')
            subprocess.run([cmake, '-S', str(source), '-B', str(build)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            subprocess.run([cmake, '--install', str(build), '--prefix', str(installed), '--component', 'Runtime'],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            inventory = json.loads((ROOT / 'build/package/runtime-notices.json').read_text())
            for name in inventory['files']:
                retained = installed / 'licenses/project' / Path(name).relative_to('third_party')
                self.assertEqual(retained.read_bytes(), (ROOT / name).read_bytes(), name)


@unittest.skipUnless(sys.platform.startswith('linux'), 'Real ELF and dpkg checks run on Linux')
class ElfInputs(unittest.TestCase):
    def test_actual_host_library_requires_its_exact_bytes(self):
        original, cache = host_library()
        original_hash = materials.sdk.digest(original)
        with tempfile.TemporaryDirectory() as directory:
            installed = Path(directory) / original.name
            shutil.copyfile(original, installed)
            result = materials.original_system_library(installed, cache)
            self.assertEqual(result['sha256'], original_hash)
            self.assertEqual(materials.sdk.digest(Path(result['originalPath'])), original_hash)
            # Keep the actual ELF and SONAME while changing bytes. A matching
            # file name/SONAME must never establish this new file's attribution.
            with installed.open('ab') as output:
                output.write(b'changed after original deployment')
            with self.assertRaisesRegex(ValueError, 'exact original host ELF'):
                materials.original_system_library(installed, cache)
        self.assertEqual(materials.sdk.digest(original), original_hash)

    def test_original_prefix_requires_matching_bytes_and_has_no_source_complete_claim(self):
        original, _ = host_library()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / 'input'; (prefix / 'lib').mkdir(parents=True)
            installed = root / 'installed.so'
            shutil.copyfile(original, prefix / 'lib' / original.name)
            shutil.copyfile(original, installed)
            relative = 'lib/' + original.name
            record = materials.prefix_origin(installed, relative, {'qt': prefix})
            self.assertEqual(record['sourceMaterialStatus'], 'not-collected-by-system-command')
            with (prefix / relative).open('ab') as output:
                output.write(b'changed input')
            self.assertIsNone(materials.prefix_origin(installed, relative, {'qt': prefix}))


@unittest.skipUnless(sys.platform.startswith('linux'), 'Actual package attribution runs on the original Linux runner')
class DpkgOrigins(unittest.TestCase):
    def test_actual_package_owner_source_version_and_notice_bytes(self):
        original, cache = host_library()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); installed = root / original.name
            shutil.copyfile(original, installed)
            origin = materials.system_origin(installed, cache)
            package = materials.package_notice(origin['package'], root / 'evidence')
            self.assertEqual(package['binaryPackage'], origin['package'])
            self.assertTrue(package['version'] and package['sourcePackage'] and package['sourceVersion'])
            self.assertEqual(package['sourceMaterialStatus'], 'not-collected')
            retained = root / 'evidence' / package['copyright']['path']
            self.assertEqual(retained.read_bytes(), Path(package['copyright']['originalPath']).read_bytes())
            common = materials.common_notices(root / 'evidence')
            self.assertTrue(common)
            for record in common:
                self.assertEqual(materials.sdk.digest(root / 'evidence' / record['path']), record['sha256'])


class QtOriginals(unittest.TestCase):
    prefix = None

    def test_original_platform_tls_and_translation_files_match_official_spdx(self):
        version = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
        documents, files = qt_materials.sdk_spdx(materials, self.prefix, version)
        self.assertIn('qtbase', documents)
        names = (['plugins/platforms/qwindows.dll', 'plugins/tls/qschannelbackend.dll'] if os.name == 'nt' else
                 ['plugins/platforms/libqxcb.so', 'plugins/platforms/libqwayland.so', 'plugins/tls/libqopensslbackend.so'])
        names.append('translations/qtbase_zh_CN.qm')
        for relative in names:
            record = qt_materials.original_sdk_file(materials, self.prefix, relative, files)
            self.assertEqual(record['sha256'], materials.sdk.digest(self.prefix / relative))
            self.assertTrue(record['packageIds'] and record['copyrightText'])

    def test_changed_real_plugin_and_missing_spdx_entry_are_rejected(self):
        version = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
        _, files = qt_materials.sdk_spdx(materials, self.prefix, version)
        relative = 'plugins/platforms/qwindows.dll' if os.name == 'nt' else 'plugins/platforms/libqxcb.so'
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            actual = prefix / relative; actual.parent.mkdir(parents=True)
            shutil.copyfile(self.prefix / relative, actual)
            self.assertEqual(qt_materials.original_sdk_file(materials, prefix, relative, files)['sha256'],
                             materials.sdk.digest(self.prefix / relative))
            with actual.open('ab') as output:
                output.write(b'not an original SDK byte')
            with self.assertRaisesRegex(ValueError, 'official SPDX checksum'):
                qt_materials.original_sdk_file(materials, prefix, relative, files)
            with self.assertRaisesRegex(ValueError, 'No original Qt SPDX'):
                qt_materials.original_sdk_file(materials, prefix, relative, {})


class ActualCargoGraph(unittest.TestCase):
    metadata = None

    def test_original_graph_rejects_missing_dependency_and_preserves_license_expressions(self):
        original = materials.meta.json_read(self.metadata, 32 * 1024 * 1024)
        baseline = rust_materials.graph_identity(materials, original)
        changed = copy.deepcopy(original)
        changed['resolve']['nodes'].pop()
        with self.assertRaisesRegex(ValueError, 'node inventories differ'):
            rust_materials.graph_identity(materials, changed)
        changed = copy.deepcopy(original)
        licensed = next(entry for entry in changed['packages'] if entry['name'] == 'gifski')
        self.assertEqual(licensed['license'], 'AGPL-3.0-or-later')
        licensed['license'] = 'MIT'
        self.assertNotEqual(rust_materials.graph_identity(materials, changed), baseline)
        changed = copy.deepcopy(original)
        for entry in changed['packages']:
            entry['manifest_path'] = '/different/runner/' + entry['name'] + '/Cargo.toml'
        self.assertEqual(rust_materials.graph_identity(materials, changed), baseline)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--without-dpkg', action='store_true',
                        help='Explicit limited local checks for containers without a package database; CI never uses this')
    parser.add_argument('--qt-prefix', type=Path, default=Path(os.environ['QT_ROOT_DIR']) if os.environ.get('QT_ROOT_DIR') else None,
                        help='Actual SDK; defaults to QT_ROOT_DIR from the original runner')
    parser.add_argument('--cargo-metadata', type=Path, help='Actual native build cargo-metadata.json, not a generated fixture')
    options = parser.parse_args()
    classes = [ProjectNotices, ElfInputs]
    if options.without_dpkg:
        print('NOT RUN: actual dpkg owner/source-version/notice validation was explicitly excluded.', flush=True)
    else:
        classes.append(DpkgOrigins)
    if options.qt_prefix:
        QtOriginals.prefix = materials.real_directory(options.qt_prefix)
        classes.append(QtOriginals)
    else:
        print('NOT RUN: original Qt SDK/SPDX byte checks require --qt-prefix or QT_ROOT_DIR.', flush=True)
    if options.cargo_metadata:
        ActualCargoGraph.metadata = options.cargo_metadata
        classes.append(ActualCargoGraph)
    else:
        print('NOT RUN: actual Cargo graph policy check requires --cargo-metadata.', flush=True)
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(value) for value in classes)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
