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
native_materials = materials.module('huhucapture_native_materials_tests', ROOT / 'scripts/collect-native-source-materials.py')


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


class NativeSourceIndex(unittest.TestCase):
    def test_existing_crlf_blob_is_preserved_but_real_changes_are_rejected(self):
        # Reproduce the actual FFmpeg Windows false mismatch without fetching
        # dependencies or using an unrelated generated build receipt.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / 'source'
            source.mkdir()

            def git(*args):
                return native_materials.git(list(args), cwd=source)

            git('-c', 'init.templateDir=', 'init', '--quiet')
            git('config', 'core.autocrlf', 'false')
            repository = 'https://example.invalid/ffmpeg.git'
            git('remote', 'add', 'origin', repository)
            for name in ('LICENSE.md', 'COPYING.LGPLv2.1', 'COPYING.GPLv3'):
                (source / name).write_bytes(b'Fixture notice\n')
            (source / '.gitignore').write_bytes(b'ignored.tmp\n')
            fixture = source / 'wavpack-cuesheet-tags'
            fixture.write_bytes(b'artist=original\r\ntitle=fixture\r\n')
            ordinary = source / 'ordinary.txt'
            ordinary.write_bytes(b'normal\ntext\n')
            git('add', '-A')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                '-c', 'commit.gpgsign=false', 'commit', '--quiet', '-m', 'source fixture')
            package = {'repository': repository, 'commit': git('rev-parse', 'HEAD')}
            expected = git('rev-parse', 'HEAD^{tree}')
            git('config', 'core.autocrlf', 'true')
            ordinary.write_bytes(b'normal\r\ntext\r\n')

            def collect(label):
                output, source_output = root / (label + '-notices'), root / (label + '-sources')
                output.mkdir()
                source_output.mkdir()
                return native_materials.archive_source('ffmpeg', source, package, output, source_output, {})

            controls = {name: (source / '.git' / name).read_bytes()
                        for name in ('HEAD', 'config', 'index')}
            result = collect('original')
            self.assertEqual(result['patchedTree'], expected)
            self.assertTrue(result['originalWorktreeAndIndexUnchanged'])
            self.assertEqual(fixture.read_bytes(), b'artist=original\r\ntitle=fixture\r\n')
            self.assertEqual(ordinary.read_bytes(), b'normal\r\ntext\r\n')

            fixture.write_bytes(b'artist=modified\r\ntitle=fixture\r\n')
            with self.assertRaisesRegex(ValueError, 'changes beyond the exact ordered locked patches'):
                collect('modified')
            fixture.write_bytes(b'artist=original\r\ntitle=fixture\r\n')
            (source / 'ignored.tmp').write_bytes(b'unexpected build output')
            with self.assertRaisesRegex(ValueError, 'Untracked files in native source'):
                collect('extra')
            self.assertEqual(controls, {name: (source / '.git' / name).read_bytes()
                                        for name in controls})


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
            self.assertTrue(record['spdxIds'])

    def test_actual_config_aliases_preserve_all_original_ids_and_properties(self):
        version = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
        documents, files = qt_materials.sdk_spdx(materials, self.prefix, version)
        aliases = {path: record for path, record in files.items() if len(record['spdxIds']) > 1}
        if os.name == 'nt':
            self.assertTrue(aliases, 'The selected real Windows SDK contains multi-config QM records')
        for path, record in aliases.items():
            value = documents[record['module']]['value']
            entries = [entry for entry in value['files']
                       if entry['fileName'].removeprefix('./') == path]
            self.assertEqual(record['spdxIds'], sorted(entry['SPDXID'] for entry in entries))
            for entry in entries:
                self.assertEqual(record['fileProperties'],
                                 {key: item for key, item in entry.items() if key != 'SPDXID'})
                parents = {relation['spdxElementId'] for relation in value['relationships']
                           if relation['relationshipType'] == 'CONTAINS' and
                           relation['relatedSpdxElement'] == entry['SPDXID'] and
                           relation['spdxElementId'] in documents[record['module']]['packages']}
                self.assertEqual(record['packageIds'], sorted(parents))
            actual = qt_materials.original_sdk_file(materials, self.prefix, path, files)
            self.assertEqual(actual['sha256'], materials.sdk.digest(self.prefix / path))
        if not aliases:
            print('NOT PRESENT: this original Qt SDK has no duplicate configuration file records.', flush=True)

    def test_conflicting_copies_of_real_spdx_evidence_are_rejected(self):
        version = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
        documents, _ = qt_materials.sdk_spdx(materials, self.prefix, version)
        document = documents['qttranslations']
        entry = next(value for value in document['value']['files']
                     if value['fileName'].removeprefix('./') == 'translations/assistant_ar.qm')
        original_documents = {name: materials.unchanged_digest(value['path']) for name, value in documents.items()}
        for changed_field in ('checksums', 'licenseConcluded', 'copyrightText', 'licenseInfoInFiles',
                              'packageIds', 'repeatedId'):
            with self.subTest(changed_field=changed_field), tempfile.TemporaryDirectory() as directory:
                prefix = Path(directory); (prefix / 'sbom').mkdir()
                changed = copy.deepcopy(document['value'])
                duplicate = copy.deepcopy(entry)
                if changed_field != 'repeatedId':
                    duplicate['SPDXID'] += '-conflicting-evidence-test'
                if changed_field == 'checksums':
                    checksum = next(value for value in duplicate['checksums'] if value['algorithm'] == 'SHA1')
                    checksum['checksumValue'] = ('1' if checksum['checksumValue'][0] == '0' else '0') + \
                        checksum['checksumValue'][1:]
                elif changed_field == 'licenseConcluded':
                    duplicate[changed_field] = 'NOASSERTION'
                elif changed_field == 'copyrightText':
                    duplicate[changed_field] += ' changed evidence'
                elif changed_field == 'licenseInfoInFiles':
                    duplicate[changed_field] = ['LicenseRef-conflicting-evidence-test']
                changed['files'].append(duplicate)
                parents = [copy.deepcopy(relation) for relation in document['value']['relationships']
                           if relation['relationshipType'] == 'CONTAINS' and
                           relation['relatedSpdxElement'] == entry['SPDXID']]
                self.assertTrue(parents)
                for relation in parents:
                    relation['relatedSpdxElement'] = duplicate['SPDXID']
                if changed_field == 'packageIds':
                    original_parent = parents[0]['spdxElementId']
                    parents[0]['spdxElementId'] = document['package']['SPDXID']
                    self.assertNotEqual(original_parent, parents[0]['spdxElementId'])
                changed['relationships'].extend(parents)
                for name, source in documents.items():
                    target = prefix / 'sbom' / source['path'].name
                    if name == 'qttranslations':
                        materials.meta.json_write(target, changed)
                    else:
                        shutil.copyfile(source['path'], target)
                error = 'Repeated or invalid original Qt SPDX ID' if changed_field == 'repeatedId' else \
                    'Conflicting original Qt SPDX records'
                with self.assertRaisesRegex(ValueError, error):
                    qt_materials.sdk_spdx(materials, prefix, version)
        for name, document in documents.items():
            self.assertEqual(materials.unchanged_digest(document['path']), original_documents[name])

    def test_changed_real_plugin_and_missing_spdx_entry_are_rejected(self):
        version = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
        _, files = qt_materials.sdk_spdx(materials, self.prefix, version)
        relative = 'plugins/platforms/qwindows.dll' if os.name == 'nt' else 'plugins/platforms/libqxcb.so'
        with tempfile.TemporaryDirectory() as directory:
            prefix = materials.real_directory(Path(directory))
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
    classes = [ProjectNotices, NativeSourceIndex, ElfInputs]
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
