#!/usr/bin/env python3
"""Offline contracts for the real Linux Qt/ICU producer.

The small text fixtures below test paths, configuration and checksums. They
never become native Qt libraries, successful build reports or release evidence.
"""
import hashlib
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
build = runpy.run_path(str(ROOT / 'scripts/build-linux-qt.py'))
bundle = build['bundle']


class LinuxQtBuilderTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='linux qt source contract ')
        self.root = Path(self.temporary.name).resolve(strict=True)

    def tearDown(self):
        self.temporary.cleanup()

    def configuration_fixture(self, name='qtbase'):
        prefix, directory = self.root / 'new prefix', self.root / name
        (prefix / 'include').mkdir(parents=True, exist_ok=True)
        (prefix / 'lib').mkdir(exist_ok=True)
        directory.mkdir()
        values = {'CMAKE_INSTALL_PREFIX': str(prefix), 'CMAKE_INSTALL_LIBDIR': 'lib',
                  'CMAKE_BUILD_TYPE': 'RelWithDebInfo', 'BUILD_SHARED_LIBS': 'ON'}
        if name == 'qtbase':
            values.update({'QT_FEATURE_' + key: 'ON' for key in build['BASE_REQUIRED']})
            values.update({'QT_FEATURE_' + key: 'OFF' for key in build['BASE_DISABLED']})
            values['ICU_INCLUDE_DIR'] = str(prefix / 'include')
            for component, library in (('UC', 'icuuc'), ('I18N', 'icui18n'), ('DATA', 'icudata')):
                path = prefix / 'lib' / ('lib' + library + '.so.73.2')
                path.write_text('Configuration-path fixture, not an ELF library.\n', encoding='utf-8')
                values['ICU_' + component + '_LIBRARY_RELEASE'] = str(path)
        else:
            values.update({'Qt6_DIR': str(prefix / 'lib/cmake/Qt6'),
                           'Qt6Core_DIR': str(prefix / 'lib/cmake/Qt6Core'),
                           'Qt6Quick_DIR': 'Qt6Quick_DIR-NOTFOUND'})
            if name in ('qttools', 'qtwayland'):
                values['QT_FEATURE_' + ('linguist' if name == 'qttools' else 'wayland_server')] = 'ON'
        self.write_cache(directory, values)
        return directory, prefix, values

    @staticmethod
    def write_cache(directory, values):
        (directory / 'CMakeCache.txt').write_text(
            '// Contract cache\n' + ''.join(key + ':STRING=' + value + '\n' for key, value in values.items()),
            encoding='utf-8')

    def test_six_sources_keep_exact_icu_version_tag_and_profile(self):
        lock = bundle.read_json(ROOT / 'build/dependencies.lock.json')
        qt, pins = build['module_pins'](lock)
        self.assertEqual(set(pins), {'icu', *build['QT_MODULES']})
        self.assertEqual(pins['qtbase']['commit'], qt['qtbaseCommit'])
        self.assertEqual(pins['icu']['commit'], '680f521746a3bd6a86f25f25ee50a62d88b489cf')
        for key, value in [('version', '74.1'), ('tagObject', '0' * 40),
                           ('repository', 'https://example.invalid/icu.git'),
                           ('configure', ['--disable-shared', '--enable-static'])]:
            original = pins['icu'][key]
            with self.subTest(key=key):
                pins['icu'][key] = value
                with self.assertRaisesRegex(ValueError, 'ICU must retain'):
                    build['module_pins'](lock)
                pins['icu'][key] = original

    def test_environment_removes_loader_headers_toolchain_and_old_qt_overrides(self):
        reference, prefix = self.root / 'original SDK', self.root / 'new prefix'
        sibling = self.root / 'original SDK-other'
        inherited = {'PATH': os.pathsep.join([str(reference / 'bin'), str(sibling), '/usr/bin']),
                     'CMAKE_TOOLCHAIN_FILE': '/old/toolchain.cmake', 'CMAKE_LIBRARY_PATH': '/old/lib',
                     'Qt6_DIR': '/old/cmake/Qt6', 'QT_HOST_PATH': '/old', 'QT_PLUGIN_PATH': '/old/plugins',
                     'LD_LIBRARY_PATH': '/old/lib', 'LD_PRELOAD': '/old/injection.so',
                     'PKG_CONFIG_PATH': '/old/pkgconfig', 'PKG_CONFIG_LIBDIR': '/old/sysroot',
                     'PKG_CONFIG_SYSROOT_DIR': '/old', 'LIBRARY_PATH': '/old/lib', 'CPATH': '/old/include',
                     'C_INCLUDE_PATH': '/old/include', 'CPLUS_INCLUDE_PATH': '/old/include',
                     'ICU_ROOT': '/old/icu', 'CC': 'unexpected-cc', 'CXX': 'unexpected-cxx',
                     'CFLAGS': '--sysroot=/old', 'LDFLAGS': '-L/old/lib', 'DESTDIR': '/other/install'}
        with mock.patch.dict(os.environ, inherited):
            environment = build['clean_environment'](reference, prefix)
            self.assertEqual(os.environ['LD_LIBRARY_PATH'], '/old/lib')
        for name in ('CMAKE_TOOLCHAIN_FILE', 'CMAKE_LIBRARY_PATH', 'Qt6_DIR', 'QT_HOST_PATH',
                     'QT_PLUGIN_PATH', 'LD_PRELOAD', 'PKG_CONFIG_LIBDIR', 'PKG_CONFIG_SYSROOT_DIR',
                     'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'CFLAGS', 'LDFLAGS', 'DESTDIR'):
            self.assertNotIn(name, environment)
        for name in ('CMAKE_PREFIX_PATH', 'QT_ROOT_DIR', 'ICU_ROOT'):
            self.assertEqual(environment[name], str(prefix))
        self.assertEqual(environment['LD_LIBRARY_PATH'], str(prefix / 'lib'))
        self.assertEqual(environment['PKG_CONFIG_PATH'], str(prefix / 'lib/pkgconfig'))
        self.assertEqual((environment['CC'], environment['CXX']), ('gcc', 'g++'))
        paths = environment['PATH'].split(os.pathsep)
        self.assertEqual(paths[0], str(prefix / 'bin'))
        self.assertNotIn(str(reference / 'bin'), paths)
        self.assertIn(str(sibling), paths)

    def test_cmake_cache_rejects_disabled_icu_and_external_icu_libraries(self):
        directory, prefix, values = self.configuration_fixture()
        checked = build['verify_configuration']('qtbase', directory, prefix)
        self.assertEqual(checked['requiredFeatures']['QT_FEATURE_icu'], 'ON')
        values['QT_FEATURE_icu'] = 'OFF'
        self.write_cache(directory, values)
        with self.assertRaisesRegex(ValueError, 'lost a required capability'):
            build['verify_configuration']('qtbase', directory, prefix)
        values['QT_FEATURE_icu'] = 'ON'
        outside = self.root / 'old-libicuuc.so.73.2'
        outside.write_text('Old SDK path fixture\n', encoding='utf-8')
        values['ICU_UC_LIBRARY_RELEASE'] = str(outside)
        self.write_cache(directory, values)
        with self.assertRaisesRegex(ValueError, 'another ICU library'):
            build['verify_configuration']('qtbase', directory, prefix)

    def test_module_cache_rejects_old_qt_even_when_other_packages_use_new_prefix(self):
        directory, prefix, values = self.configuration_fixture('qtwayland')
        build['verify_configuration']('qtwayland', directory, prefix)
        values['Qt6Core_DIR'] = str(self.root / 'old SDK/lib/cmake/Qt6Core')
        self.write_cache(directory, values)
        with self.assertRaisesRegex(ValueError, 'old or external Qt'):
            build['verify_configuration']('qtwayland', directory, prefix)

    def test_cache_cannot_hide_duplicate_entries_behind_later_values(self):
        directory, _, _ = self.configuration_fixture()
        path = directory / 'CMakeCache.txt'
        original = path.read_text(encoding='utf-8')
        for name in ('QT_FEATURE_icu', '"QT_FEATURE_icu"'):
            with self.subTest(name=name):
                path.write_text(original + name + ':BOOL=OFF\n', encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'Duplicate CMake cache entry'):
                    build['cmake_values'](path)

    def test_real_cmake_cache_retains_distinct_quoted_target_names_and_values(self):
        cmake = shutil.which(os.environ.get('HUHU_CMAKE', 'cmake'))
        if cmake is None:
            self.skipTest('Real CMake is required to generate the cache-format regression')
        source, directory = self.root / 'cache source', self.root / 'cache build'
        source.mkdir()
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(cache_format NONE)
set("QT_QMAKE_LIB_OF_TARGET_GLIB2::GIO" "gio" CACHE INTERNAL "Original target name")
set("QT_QMAKE_LIB_OF_TARGET_GLIB2::GLIB2" "glib" CACHE INTERNAL "Original target name")
set("QT_QMAKE_LIB_OF_TARGET_GLIB2::GOBJECT" "gobject" CACHE INTERNAL "Original target name")
set(QT_FEATURE_icu ON CACHE BOOL "Required capability")
set(CONTRACT_VALUE "first:part=second part " CACHE STRING "Keep the complete value")
''', encoding='utf-8')
        subprocess.run([cmake, '-S', str(source), '-B', str(directory)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        path = directory / 'CMakeCache.txt'
        original = path.read_text(encoding='utf-8')
        self.assertIn('"QT_QMAKE_LIB_OF_TARGET_GLIB2::GIO":INTERNAL=gio', original)
        values = build['cmake_values'](path)
        for name, value in (('GIO', 'gio'), ('GLIB2', 'glib'), ('GOBJECT', 'gobject')):
            self.assertEqual(values['QT_QMAKE_LIB_OF_TARGET_GLIB2::' + name], value)
        self.assertEqual(values['QT_FEATURE_icu'], 'ON')
        self.assertEqual(values['CONTRACT_VALUE'], 'first:part=second part ')
        with path.open('a', encoding='utf-8') as stream:
            stream.write('"QT_QMAKE_LIB_OF_TARGET_GLIB2::GIO":INTERNAL=other\n')
        with self.assertRaisesRegex(ValueError, 'Duplicate CMake cache entry'):
            build['cmake_values'](path)

    def test_source_configuration_keeps_argument_boundaries_and_native_icu_binding(self):
        source, prefix = self.root / 'sources with spaces', self.root / 'new prefix'
        command = build['configure_command']('qtbase', source, self.root / 'build', prefix, Path('/python'))
        self.assertEqual(command[command.index('-S') + 1], str(source))
        self.assertEqual(command[command.index('-G') + 1], 'Ninja')
        self.assertIn('-DCMAKE_BUILD_TYPE=RelWithDebInfo', command)
        self.assertIn('-DCMAKE_INSTALL_LIBDIR=lib', command)
        self.assertIn('-DICU_UC_LIBRARY_RELEASE=' + str(prefix / 'lib/libicuuc.so'), command)
        self.assertIn('-DFEATURE_openssl_runtime=ON', command)
        self.assertIn('-DFEATURE_xcb=ON', command)
        self.assertIn('-DFEATURE_wayland_client=ON', command)
        self.assertIn('-DQT_SBOM_REQUIRE_VERIFY_SPDX_V2=ON', command)
        tools = build['configure_command']('qttools', source, self.root / 'tools-build', prefix, Path('/python'))
        self.assertIn('-DFEATURE_linguist=ON', tools)
        self.assertFalse(any('msvc' in argument or 'MSVC' in argument or 'schannel' in argument for argument in tools))

    def test_all_five_generated_spdx_documents_compare_actual_fixture_bytes(self):
        prefix = self.root / 'prefix'
        (prefix / 'sbom').mkdir(parents=True)
        for module in build['QT_MODULES']:
            path = prefix / (module + '.contract-input')
            path.write_bytes(('SPDX contract input for ' + module + '\n').encode())
            value = {'spdxVersion': 'SPDX-2.3', 'files': [{'fileName': './' + path.name, 'checksums': [
                {'algorithm': 'SHA1', 'checksumValue': hashlib.sha1(path.read_bytes()).hexdigest()}]}]}
            (prefix / 'sbom' / (module + '-6.12.1.spdx.json')).write_bytes(bundle.canonical(value))
        verify = build['SHARED']['verify_generated_spdx']
        self.assertEqual(len(verify(prefix, '6.12.1', build['QT_MODULES'])), 5)
        (prefix / 'qtwayland.contract-input').write_bytes(b'changed after SPDX creation\n')
        with self.assertRaisesRegex(ValueError, 'differs from its generated SPDX'):
            verify(prefix, '6.12.1', build['QT_MODULES'])
        with self.assertRaisesRegex(ValueError, 'Select each actual Qt SPDX module once'):
            verify(prefix, '6.12.1', ('qtbase', 'qtbase'))

    def test_configuration_fixtures_cannot_pass_as_actual_runtime_outputs(self):
        _, prefix, _ = self.configuration_fixture()
        with self.assertRaises(ValueError):
            build['native_file'](prefix, 'lib/libicuuc.so.73.2')


if __name__ == '__main__':
    unittest.main()
