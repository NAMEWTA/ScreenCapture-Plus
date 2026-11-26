#!/usr/bin/env python3
"""Build the locked Linux Qt modules and ICU from their real original sources.

The new prefix has its own native producer receipts. It is never represented as
a byte-for-byte reconstruction of the reference SDK or a complete app release.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WINDOWS_HELPER = ROOT / 'scripts/build-windows-qt.py'
RESTORE_HELPER = ROOT / 'scripts/restore-qt-module-sources.py'
SHARED = runpy.run_path(str(WINDOWS_HELPER))
RESTORE = runpy.run_path(str(RESTORE_HELPER))
API, bundle = SHARED['API'], SHARED['bundle']
QT_MODULES = ('qtbase', 'qtsvg', 'qtwayland', 'qttools', 'qttranslations')
ICU_OPTIONS = ('--enable-shared', '--disable-static', '--disable-tests', '--disable-samples')
BASE_REQUIRED = ('gui', 'widgets', 'network', 'thread', 'dbus', 'dbus_linked', 'accessibility',
                 'opengl', 'opengl_desktop', 'egl', 'icu', 'xcb', 'xcb_xlib', 'xcb_glx_plugin',
                 'xcb_egl_plugin', 'xkbcommon', 'xkbcommon_x11', 'wayland', 'wayland_client',
                 'waylandscanner', 'wayland_egl', 'wayland_client_xdg_shell', 'openssl',
                 'openssl_runtime', 'opensslv30', 'glib', 'fontconfig', 'freetype',
                 'system_freetype', 'jpeg', 'png', 'pcre2', 'harfbuzz', 'doubleconversion',
                 'system_zlib', 'zstd', 'brotli', 'vulkan')
BASE_DISABLED = ('system_jpeg', 'system_png', 'system_pcre2', 'system_harfbuzz',
                 'system_doubleconversion', 'system_xcb_xinput', 'libudev', 'liburing',
                 'openssl_linked', 'opengles2')


def module_pins(lock):
    qt = lock['qtMigration']['qt']
    pins = {item['module']: item for item in RESTORE['pinned_modules'](qt, 'linux')}
    # Preserve the same reviewed Qttools tag/commit validation as Windows.
    _, common = SHARED['module_pins'](lock)
    pins['qttools'] = common['qttools']
    icu = qt.get('sourceDependencies', {}).get('icu')
    bundle.require(isinstance(icu, dict) and icu.get('repository') == 'https://github.com/unicode-org/icu.git' and
                   icu.get('version') == '73.2' and icu.get('tag') == 'release-73-2' and
                   bundle.HEX40.fullmatch(icu.get('commit', '')) and icu.get('tagObject') == icu['commit'] and
                   icu.get('configure') == list(ICU_OPTIONS),
                   'ICU must retain the reviewed 73.2 lightweight tag, full commit and shared-library profile')
    pins['icu'] = icu
    return qt, pins


def clean_environment(reference, prefix):
    environment = SHARED['clean_environment'](reference, prefix)
    # No previous SDK, compiler flags, pkg-config sysroot or loader injection
    # may silently provide the ICU/Qt libraries built by this new producer.
    for name in tuple(environment):
        if name.startswith(('CMAKE_', 'QT_', 'QT6', 'Qt6')) or name in (
                'QTDIR', 'Qt_DIR', 'QMAKE', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH',
                'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_RUN_PATH', 'LIBRARY_PATH', 'CPATH',
                'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR',
                'PKG_CONFIG_SYSROOT_DIR', 'ICU_ROOT', 'ICU_DATA', 'CC', 'CXX', 'AR', 'LD',
                'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'DESTDIR'):
            del environment[name]
    environment.update({'CMAKE_PREFIX_PATH': str(prefix), 'QT_ROOT_DIR': str(prefix),
                        'ICU_ROOT': str(prefix), 'PKG_CONFIG_PATH': str(prefix / 'lib/pkgconfig'),
                        'LD_LIBRARY_PATH': str(prefix / 'lib'), 'LIBRARY_PATH': str(prefix / 'lib'),
                        'CC': 'gcc', 'CXX': 'g++', 'LC_ALL': 'C.UTF-8'})
    return environment


def configure_command(name, source, build, prefix, python):
    bundle.require(name in QT_MODULES, 'Unknown Linux Qt source module')
    arguments = ['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_COMPILER=gcc', '-DCMAKE_CXX_COMPILER=g++',
                 '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
                 '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
                 '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF', '-DBUILD_SHARED_LIBS=ON',
                 '-DBUILD_WITH_PCH=OFF', '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF',
                 '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DQT_INSTALL_CONFIG_INFO_FILES=ON',
                 '-DQT_GENERATE_SBOM=ON', '-DQT_GENERATE_SOURCE_SBOM=ON',
                 '-DQT_SBOM_REQUIRE_GENERATE_SPDX_V2_JSON=ON', '-DQT_SBOM_REQUIRE_VERIFY_SPDX_V2=ON',
                 '-DQT_SBOM_PYTHON_INTERP=' + str(python)]
    if name == 'qtbase':
        arguments += ['-DFEATURE_' + feature + '=ON' for feature in BASE_REQUIRED]
        arguments += ['-DFEATURE_' + feature + '=OFF' for feature in BASE_DISABLED]
        arguments += ['-DINPUT_opengl=desktop', '-DOpenGL_GL_PREFERENCE=LEGACY', '-DINPUT_openssl=runtime',
                      '-DINPUT_dbus=linked', '-DINPUT_bundled_xcb_xinput=yes', '-DINPUT_zlib=system',
                      '-DINPUT_freetype=system', '-DINPUT_libpng=qt', '-DINPUT_libjpeg=qt',
                      '-DINPUT_pcre=qt', '-DINPUT_harfbuzz=qt', '-DINPUT_doubleconversion=qt',
                      '-DICU_ROOT=' + str(prefix), '-DICU_INCLUDE_DIR=' + str(prefix / 'include')]
        for component, leaf in (('UC', 'icuuc'), ('I18N', 'icui18n'), ('DATA', 'icudata')):
            arguments.append('-DICU_' + component + '_LIBRARY_RELEASE=' + str(prefix / 'lib' / ('lib' + leaf + '.so')))
    elif name == 'qttools':
        # Reuse only the reviewed Linguist feature selection, without Windows
        # generator/CRT/TLS arguments. The tools themselves are built natively.
        windows_arguments = SHARED['configure_command'](name, source, build, prefix, python)
        arguments += [argument for argument in windows_arguments if argument.startswith('-DFEATURE_') and
                      not argument.startswith('-DFEATURE_msvc_')]
    elif name == 'qtwayland':
        arguments += ['-DFEATURE_wayland_server=ON']
    return arguments


def cmake_values(path):
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'([^/#:][^:]*):[^=]+=(.*)', line)
        if match:
            bundle.require(match[1] not in values, 'Duplicate CMake cache entry: ' + match[1])
            values[match[1]] = match[2]
    return values


def verify_configuration(name, build, prefix):
    values = cmake_values(build / 'CMakeCache.txt')
    bundle.require(Path(values.get('CMAKE_INSTALL_PREFIX', '')).resolve() == prefix and
                   values.get('CMAKE_INSTALL_LIBDIR') == 'lib' and
                   values.get('CMAKE_BUILD_TYPE') == 'RelWithDebInfo' and values.get('BUILD_SHARED_LIBS') == 'ON',
                   'Qt configured another prefix, library directory, configuration or linkage')
    if name == 'qtbase':
        expected = {**{feature: 'ON' for feature in BASE_REQUIRED},
                    **{feature: 'OFF' for feature in BASE_DISABLED}}
        bundle.require(all(values.get('QT_FEATURE_' + feature) == enabled for feature, enabled in expected.items()),
                       'The actual Qt feature cache lost a required capability or changed a reviewed library choice')
        bundle.require(Path(values.get('ICU_INCLUDE_DIR', '')).resolve() == prefix / 'include',
                       'Qt found ICU headers outside the new source-built prefix')
        for component, leaf in (('UC', 'icuuc'), ('I18N', 'icui18n'), ('DATA', 'icudata')):
            actual = Path(values.get('ICU_' + component + '_LIBRARY_RELEASE', '')).resolve(strict=True)
            expected_path = prefix / 'lib' / ('lib' + leaf + '.so.73.2')
            bundle.require(actual == expected_path and actual.is_file(),
                           'Qt resolved another ICU library instead of the new 73.2 producer output')
    else:
        package_paths = {key: value for key, value in values.items() if key.startswith('Qt6') and key.endswith('_DIR')
                         and value and not value.endswith('-NOTFOUND')}
        bundle.require(package_paths and all(Path(value).resolve().is_relative_to(prefix) for value in package_paths.values()),
                       'A later Qt module resolved an old or external Qt CMake package')
        if name in ('qttools', 'qtwayland'):
            feature = 'linguist' if name == 'qttools' else 'wayland_server'
            bundle.require(values.get('QT_FEATURE_' + feature) == 'ON', 'The requested native Qt module was disabled')
    return {'requiredFeatures': {key: value for key, value in values.items() if key.startswith('QT_FEATURE_')},
            'icuLibraries': {key: value for key, value in values.items() if key.startswith('ICU_') and 'LIBRARY' in key}}


def native_file(prefix, relative):
    path = prefix / relative
    actual = path.resolve(strict=True)
    bundle.require(actual.is_relative_to(prefix) and actual.is_file() and
                   bundle.packaging_metadata().machine(actual) == 'linux',
                   'Missing actual Linux amd64 ELF output inside the new prefix: ' + relative)
    return {'path': relative, 'resolvedPath': actual.relative_to(prefix).as_posix(), **API['identity'](actual)}


def verify_icu(prefix, pin, recorder, environment):
    files = [native_file(prefix, 'lib/lib' + name + '.so.' + pin['version'])
             for name in ('icuuc', 'icui18n', 'icudata')]
    for package in ('icu-uc', 'icu-i18n'):
        version, _ = recorder.run(['pkg-config', '--modversion', package], cwd=prefix, env=environment, capture=True)
        actual, _ = recorder.run(['pkg-config', '--variable=prefix', package], cwd=prefix, env=environment, capture=True)
        bundle.require(version.strip() == pin['version'] and Path(actual.strip()).resolve() == prefix,
                       'The actual ICU pkg-config output is from a different version or prefix')
    return files


def build_icu(workspace, source, prefix, pin, environment, control, jobs, lock_path):
    SHARED['restore_tools'](source, pin, control, bundle.git_environment(), component='ICU')
    build = workspace / 'icu-build'
    build.mkdir()
    recorder = API['NativeBuild'](workspace / 'icu-source-receipt', 'linux', __file__, 'icu', source, pin, prefix)
    recorder.retain_record(lock_path)
    recorder.retain_record(WINDOWS_HELPER)
    SHARED['CHECKOUT']['retain_policy'](source, recorder)
    for program in ('gcc', 'g++', 'make', 'ar', 'ld', 'pkg-config', sys.executable):
        recorder.observe_tool(program, environment)
    command = [str(source / 'icu4c/source/runConfigureICU'), 'Linux', '--prefix=' + str(prefix),
               '--libdir=' + str(prefix / 'lib'), *pin['configure']]
    configuration = recorder.run(command, cwd=build, env=environment)
    recorder.run(['make', '-j' + str(jobs)], cwd=build, env=environment)
    recorder.run(['make', 'install'], cwd=build, env=environment)
    runtime = verify_icu(prefix, pin, recorder, environment)
    records = [build / name for name in ('config.log', 'config.status', 'Makefile', 'icudefs.mk',
                                        'config/Makefile.inc', 'config/icu.pc', 'config/pkgdata.inc')]
    SHARED['CHECKOUT']['verify_policy'](source)
    plan = recorder.finish(configure_index=configuration, configuration_files=records)
    return {'id': 'icu', 'repository': pin['repository'], 'commit': pin['commit'],
            'sourceDirectory': str(source), 'sourceBuildReceipt': plan, 'runtime': runtime}


def verify_runtime(prefix, version, control, environment):
    names = ['lib/libQt6' + module + '.so.' + version for module in
             ('Core', 'Gui', 'Widgets', 'Network', 'DBus', 'OpenGL', 'Svg', 'Test', 'WaylandClient', 'WaylandCompositor')]
    names += ['plugins/platforms/libqxcb.so', 'plugins/platforms/libqwayland.so',
              'plugins/tls/libqopensslbackend.so', 'plugins/imageformats/libqsvg.so',
              'plugins/wayland-shell-integration/libxdg-shell.so', 'bin/lrelease', 'bin/lconvert', 'bin/qmake6']
    files = [native_file(prefix, name) for name in names]
    qms = sorted((prefix / 'translations').glob('*.qm'))
    bundle.require(qms and all((prefix / 'translations' / ('qtbase_' + language + '.qm')).is_file()
                             for language in ('zh_CN', 'zh_TW', 'ja', 'ru')),
                   'New Qt translations are missing the product languages')
    core = prefix / 'lib' / ('libQt6Core.so.' + version)
    dynamic, _ = control.run(['readelf', '-d', str(core)], cwd=prefix, env=environment, capture=True)
    needed = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic)
    bundle.require({'libicuuc.so.73', 'libicui18n.so.73'} <= set(needed),
                   'The actual QtCore ELF no longer depends on the new ICU 73 runtime')
    for name in ('lib/libQt6Core.so.' + version, 'plugins/platforms/libqxcb.so',
                 'plugins/platforms/libqwayland.so', 'plugins/tls/libqopensslbackend.so'):
        loaded, _ = control.run(['ldd', str(prefix / name)], cwd=prefix, env=environment, capture=True)
        bundle.require('not found' not in loaded, 'A new Qt runtime dependency cannot be loaded: ' + name)
        for leaf, resolved in re.findall(r'^\s*(lib(?:Qt6|icu)\S+) => (.*?)\s+\(0x[0-9a-fA-F]+\)', loaded, re.MULTILINE):
            bundle.require(Path(resolved).resolve().is_relative_to(prefix),
                           'The native loader selected an external Qt/ICU library: ' + leaf)
    for program, argument in (('qmake6', '-query'), ('lrelease', '-version'), ('lconvert', '-help')):
        control.run([str(prefix / 'bin' / program), argument], cwd=prefix, env=environment)
    return {'files': files, 'qtCoreNeeded': needed,
            'translations': [{'path': path.relative_to(prefix).as_posix(), **API['identity'](path)} for path in qms]}


def build(options):
    bundle.require(platform.system() == 'Linux' and platform.machine().lower() in ('amd64', 'x86_64'),
                   'Build this runtime on the actual Linux amd64 host')
    bundle.require('write_module_plan' in API, 'The real source receipt module-plan helper is required')
    lock_path = ROOT / 'build/dependencies.lock.json'
    lock_identity = API['identity'](lock_path)
    qt, pins = module_pins(bundle.read_json(lock_path))
    sources, reference = options.source_workspace.resolve(strict=True), options.reference_sdk.resolve(strict=True)
    workspace = options.workspace.resolve()
    bundle.require(1 <= options.jobs <= 64 and not options.workspace.is_symlink() and not workspace.exists() and
                   not any(workspace.is_relative_to(path) or path.is_relative_to(workspace) for path in (sources, reference)),
                   'Use a new separate Qt/ICU build workspace and a bounded parallel job count')
    for name in ('qtbase', 'qtsvg', 'qtwayland', 'qttranslations'):
        SHARED['checked_checkout'](sources / name, pins[name])
    baseline = reference / 'sbom' / ('qtbase-' + qt['version'] + '.spdx.json')
    baseline_identity = API['identity'](baseline)
    original = bundle.read_json(baseline)
    roots = [entry for entry in original['packages'] if entry['name'] == 'qtbase']
    icu_records = [entry for entry in original['packages'] if entry['name'].lower() == 'icu']
    bundle.require(len(roots) == 1 and roots[0]['downloadLocation'].endswith('@' + pins['qtbase']['commit']) and
                   'configuration for Linux' in roots[0].get('comment', '') and
                   icu_records and all(entry.get('versionInfo') == pins['icu']['version'] for entry in icu_records),
                   'The reference must remain the original locked Linux Qt SDK with ICU 73.2')
    workspace.mkdir(parents=True)
    prefix = workspace / 'install'
    environment = clean_environment(reference, prefix)
    report = {'schemaVersion': 1, 'kind': 'huhucapture-built-linux-qt', 'qtVersion': qt['version'],
              'prefix': str(prefix), 'platform': 'linux', 'architecture': 'amd64',
              'configurations': ['RelWithDebInfo'], 'sourceModules': [], 'buildComplete': False,
              'correspondingSourceComplete': False, 'failures': [],
              'dependencyLockSha256': lock_identity['sha256'],
              'scope': 'New native Linux Qt/ICU runtime, tools and translations; not a reproduction of vendor SDK bytes',
              'sdkConfigurationDifferences': ['Actual local GCC/tools/prefix, not the old vendor paths or compiler build',
                  'Original Git blob bytes; upstream working-tree text/encoding/filter conversion is disabled',
                  'ICU 73.2 comes from its exact official source/tag and newly recorded shared-library build',
                  'Qt keeps bundled JPEG/PNG/PCRE/HarfBuzz/DoubleConversion and system zlib/FreeType/Fontconfig',
                  'Only Linguist tools requested from Qttools; no QDoc/Designer/Assistant application',
                  'Optional system integrations follow this host; the actual complete CMake feature cache is retained'],
              'icuBuildScope': 'Shared ICU runtime/data/tools; tests and samples are not built or claimed to have passed'}
    control = API['ExecutionLog'](workspace / 'control', 'linux', __file__)
    try:
        for path in (lock_path, baseline, WINDOWS_HELPER, RESTORE_HELPER,
                     reference / 'config_qtbase.opt', reference / 'config_qtbase.summary'):
            control.retain_record(path)
        for program in ('gcc', 'g++', 'cmake', 'ninja', 'make', 'ar', 'ld', 'readelf', 'ldd', 'pkg-config', 'reuse', sys.executable):
            control.observe_tool(program, environment)
        control.run([sys.executable, '-c', 'import spdx_tools.spdx.clitools.pyspdxtools; '
                     'import importlib.metadata; print(importlib.metadata.version("spdx-tools"))'], cwd=workspace, env=environment)
        control.run(['reuse', '--version'], cwd=workspace, env=environment)
        report['sourceModules'].append(build_icu(workspace, workspace / 'icu-source', prefix, pins['icu'],
                                                environment, control, options.jobs, lock_path))
        tools_source = workspace / 'qttools-source'
        SHARED['restore_tools'](tools_source, pins['qttools'], control, bundle.git_environment())
        for name in QT_MODULES:
            source = tools_source if name == 'qttools' else sources / name
            directory = workspace / (name + '-build')
            recorder = API['NativeBuild'](workspace / (name + '-source-receipt'), 'linux', __file__,
                                           name, source, pins[name], prefix)
            for path in (lock_path, baseline, WINDOWS_HELPER, RESTORE_HELPER):
                recorder.retain_record(path)
            SHARED['CHECKOUT']['retain_policy'](source, recorder)
            for program in ('gcc', 'g++', 'ninja', 'pkg-config', 'reuse', sys.executable):
                recorder.observe_tool(program, environment)
            configuration = recorder.run(configure_command(name, source, directory, prefix, Path(sys.executable).resolve()),
                                         cwd=workspace, env=environment)
            features = verify_configuration(name, directory, prefix)
            recorder.run(['cmake', '--build', str(directory), '--parallel', str(options.jobs)], cwd=workspace, env=environment)
            recorder.run(['cmake', '--install', str(directory)], cwd=workspace, env=environment)
            SHARED['CHECKOUT']['verify_policy'](source)
            plan = recorder.finish(configure_index=configuration,
                                   configuration_files=SHARED['configuration_records'](directory))
            report['sourceModules'].append({'id': name, 'repository': pins[name]['repository'], 'commit': pins[name]['commit'],
                'sourceDirectory': str(source), 'sourceBuildReceipt': plan, 'actualConfiguration': features})
        report['generatedSbom'] = SHARED['verify_generated_spdx'](prefix, qt['version'], QT_MODULES)
        report['runtime'] = verify_runtime(prefix, qt['version'], control, environment)
        reference_qms = {path.name: API['identity'](path) for path in (reference / 'translations').glob('*.qm')}
        built_qms = {Path(item['path']).name: {key: item[key] for key in ('size', 'sha256')}
                     for item in report['runtime']['translations']}
        report['referenceTranslationComparison'] = {'referenceFiles': len(reference_qms), 'builtFiles': len(built_qms),
            'identicalNames': sorted(name for name, value in reference_qms.items() if built_qms.get(name) == value),
            'differentOrAbsentNames': sorted(name for name, value in reference_qms.items() if built_qms.get(name) != value),
            'meaning': 'Actual byte comparison only; the new catalogs are attributed to this source build'}
        bundle.require(API['identity'](lock_path) == lock_identity and API['identity'](baseline) == baseline_identity,
                       'Original source lock or reference SDK evidence changed')
        for item in report['sourceModules']:
            SHARED['checked_checkout'](Path(item['sourceDirectory']), pins[item['id']])
        record = control.raw_record({'dependencyLock': lock_identity, 'referenceSdk': bundle.descriptor(baseline)},
                                    {'prefix': str(prefix), 'files': bundle.inventory(prefix)})
        report['executionRecord'] = {'path': str(record), **API['identity'](record)}
        report['buildComplete'] = True
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report['failures'].append(str(error))
        raise
    finally:
        API['write_new'](workspace / 'build-info.json', report)
    API['write_module_plan'](workspace, report, 'linux')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-workspace', type=Path, required=True,
                        help='Original four locked Git modules from restore-qt-module-sources.py --platform linux')
    parser.add_argument('--reference-sdk', type=Path, required=True, help='Unmodified original Linux Qt SDK used only as evidence')
    parser.add_argument('--workspace', type=Path, required=True, help='New empty build workspace; output prefix is install/')
    parser.add_argument('--jobs', type=int, default=2)
    options = parser.parse_args()
    try:
        result = build(options)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('Linux Qt/ICU source build failed: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps({'prefix': result['prefix'], 'buildComplete': True, 'correspondingSourceComplete': False,
                      'sourceModules': len(result['sourceModules']),
                      'sourcePlan': str(options.workspace.resolve() / 'source-plan-fragment.json')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
