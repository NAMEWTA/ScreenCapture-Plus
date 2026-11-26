#!/usr/bin/env python3
"""Offline input/output contracts for the native Windows Qt build entry point.

These tests do not build Qt or turn placeholder files into runtime evidence.
"""
import hashlib
import json
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
build = runpy.run_path(str(ROOT / 'scripts/build-windows-qt.py'))
bundle = build['bundle']


class WindowsQtBuilderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='windows-qt-source-contract-')
        self.root = Path(self.temporary.name).resolve(strict=True)

    def tearDown(self):
        self.temporary.cleanup()

    def spdx_fixture(self):
        prefix = self.root / 'prefix'
        (prefix / 'sbom').mkdir(parents=True)
        for module in ('qtbase', 'qtsvg', 'qttools', 'qttranslations'):
            path = prefix / (module + '.contract-input')
            path.write_bytes(('Actual fixture bytes for ' + module + '\n').encode())
            record = {'fileName': './' + path.name, 'checksums': [
                {'algorithm': 'SHA1', 'checksumValue': hashlib.sha1(path.read_bytes()).hexdigest()}]}
            value = {'spdxVersion': 'SPDX-2.3', 'files': [record, dict(record)]}
            (prefix / 'sbom' / (module + '-6.12.1.spdx.json')).write_bytes(bundle.canonical(value))
        return prefix

    def test_generated_sbom_compares_every_whole_file_and_rejects_changed_bytes(self):
        prefix = self.spdx_fixture()
        verified = build['verify_generated_spdx'](prefix, '6.12.1')
        self.assertEqual(len(verified), 4)
        self.assertTrue(all(item['verifiedFiles'] == 1 for item in verified))
        with (prefix / 'qtbase.contract-input').open('ab') as stream:
            stream.write(b'changed after generated SPDX')
        with self.assertRaisesRegex(ValueError, 'differs from its generated SPDX'):
            build['verify_generated_spdx'](prefix, '6.12.1')

    def test_generated_sbom_does_not_accept_a_path_outside_the_new_prefix(self):
        prefix = self.spdx_fixture()
        path = prefix / 'sbom/qtbase-6.12.1.spdx.json'
        document = bundle.read_json(path)
        document['files'][0]['fileName'] = '../outside.dll'
        path.write_bytes(bundle.canonical(document))
        with self.assertRaisesRegex(ValueError, 'Unsafe relative path'):
            build['verify_generated_spdx'](prefix, '6.12.1')

    def test_environment_removes_old_qt_resolution_without_changing_caller_environment(self):
        reference, prefix = self.root / 'old SDK', self.root / 'new SDK'
        unrelated = self.root / 'old SDK-other'
        inherited = {'PATH': os.pathsep.join([str(reference / 'bin'), str(unrelated), '/toolchain']),
                     'Qt6_DIR': str(reference / 'lib/cmake/Qt6'), 'QT_ROOT_DIR': str(reference),
                     'CMAKE_PREFIX_PATH': str(reference), 'QT_PLUGIN_PATH': str(reference / 'plugins'),
                     'CMAKE_TOOLCHAIN_FILE': '/unexpected/toolchain.cmake', 'VCPKG_ROOT': '/external/vcpkg'}
        with mock.patch.dict(os.environ, inherited):
            environment = build['clean_environment'](reference, prefix)
            self.assertEqual(os.environ['Qt6_DIR'], inherited['Qt6_DIR'])
        for name in ('Qt6_DIR', 'QT_PLUGIN_PATH', 'CMAKE_TOOLCHAIN_FILE', 'VCPKG_ROOT'):
            self.assertNotIn(name, environment)
        self.assertEqual(environment['QT_ROOT_DIR'], str(prefix))
        self.assertEqual(environment['CMAKE_PREFIX_PATH'], str(prefix))
        self.assertEqual(environment['PATH'].split(os.pathsep)[0], str(prefix / 'bin'))
        self.assertNotIn(str(reference / 'bin'), environment['PATH'].split(os.pathsep))
        self.assertIn(str(unrelated), environment['PATH'].split(os.pathsep))

    def test_locked_sources_cannot_be_replaced_by_a_version_only_tools_input(self):
        lock = bundle.read_json(ROOT / 'build/dependencies.lock.json')
        qt, pins = build['module_pins'](lock)
        self.assertEqual(pins['qtbase']['commit'], qt['qtbaseCommit'])
        self.assertEqual(pins['qttools'], qt['sourceTools']['qttools'])
        del lock['qtMigration']['qt']['sourceTools']['qttools']['tagObject']
        with self.assertRaisesRegex(ValueError, 'release commit/tag object'):
            build['module_pins'](lock)

    def test_native_command_preserves_argument_boundaries_and_dynamic_crt_pair(self):
        source = self.root / 'sources with spaces/qtbase'
        target = self.root / 'new prefix'
        command = build['configure_command']('qtbase', source, self.root / 'build', target, Path('/actual/python'))
        self.assertEqual(command[command.index('-S') + 1], str(source))
        self.assertIn('-DCMAKE_INSTALL_PREFIX=' + str(target), command)
        self.assertIn('-DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo;Debug', command)
        self.assertIn('-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL', command)
        self.assertIn('-DFEATURE_schannel=ON', command)
        self.assertIn('-DFEATURE_windeployqt=ON', command)
        self.assertIn('-DQT_SBOM_REQUIRE_GENERATE_SPDX_V2_JSON=ON', command)
        self.assertIn('-DQT_SBOM_REQUIRE_VERIFY_SPDX_V2=ON', command)

    def test_only_locked_qttools_fetches_enable_the_official_https_transport(self):
        lock = bundle.read_json(ROOT / 'build/dependencies.lock.json')
        _, pins = build['module_pins'](lock)
        pin = pins['qttools']
        source = self.root / 'qttools'
        recorder = mock.Mock()
        checked = mock.Mock()
        environment = {'GIT_CONFIG_NOSYSTEM': '1'}
        with mock.patch.object(bundle, 'git', side_effect=[pin['tagObject'].encode(), pin['commit'].encode()]), \
                mock.patch.dict(build['restore_tools'].__globals__, {'checked_checkout': checked}):
            build['restore_tools'](source, pin, recorder, environment)
        commands = [call.args[0] for call in recorder.run.call_args_list]
        fetched = [command for command in commands if 'fetch' in command]
        self.assertEqual(len(fetched), 2)
        self.assertIn(['remote', 'add', 'origin', 'https://github.com/qt/qttools.git'],
                      [command[command.index('-C') + 2:] for command in commands])
        self.assertEqual(fetched[0][fetched[0].index('fetch'):],
                         ['fetch', '--depth=1', '--no-tags', 'origin', pin['commit']])
        self.assertEqual(fetched[1][fetched[1].index('fetch'):],
                         ['fetch', '--depth=1', '--no-tags', 'origin',
                          'refs/tags/' + pin['tag'] + ':refs/tags/' + pin['tag']])
        for command in commands:
            self.assertIn('protocol.allow=never', command)
            self.assertEqual(command.count('protocol.https.allow=always'), int('fetch' in command))
        for call in recorder.run.call_args_list:
            self.assertEqual(call.kwargs, {'cwd': source, 'env': environment})
        checked.assert_called_once_with(source, pin)
        self.assertNotIn('protocol.https.allow=always', bundle.git_command(source, 'status', '--porcelain'))


if __name__ == '__main__':
    unittest.main()
