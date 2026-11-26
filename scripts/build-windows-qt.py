#!/usr/bin/env python3
"""Build the locked Windows Qt runtime and translation tools in one new prefix.

This is an actual native build, not a repair of signed SDK files. Its separate
producer receipts and generated SPDX records describe the newly built bytes.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
bundle = API['bundle']
CHECKOUT = runpy.run_path(str(ROOT / 'build/package/source_checkout.py'))
CONFIGURATIONS = ('RelWithDebInfo', 'Debug')


def module_pins(lock):
    qt = lock['qtMigration']['qt']
    bundle.require(qt['windowsCrt'] == 'MD/MDd', 'The Qt runtime must use the application dynamic CRT pair')
    pins = {'qtbase': {'repository': qt['qtbaseRepository'], 'commit': qt['qtbaseCommit']}}
    for name in ('qtsvg', 'qttranslations'):
        pins[name] = qt['sourceModules'][name]
    tools = qt.get('sourceTools', {}).get('qttools')
    bundle.require(isinstance(tools, dict) and tools.get('tag') == 'v' + qt['version'] and
                   bundle.HEX40.fullmatch(tools.get('tagObject', '')),
                   'The actual Qttools release commit/tag object must be in the dependency lock')
    pins['qttools'] = tools
    for name, pin in pins.items():
        bundle.require(pin['repository'] == 'https://github.com/qt/' + name + '.git' and
                       bundle.HEX40.fullmatch(pin['commit']), 'Unsupported Qt source identity: ' + name)
    return qt, pins


def clean_environment(reference, prefix):
    environment = os.environ.copy()
    # No installed Qt CMake override may redirect this build to the old SDK.
    for name in tuple(environment):
        if (name.startswith(('Qt6', 'QT6')) and name.endswith(('_DIR', '_ROOT'))) or name in (
                'QTDIR', 'Qt_DIR', 'QT_ROOT_DIR', 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH',
                'CMAKE_PREFIX_PATH', 'CMAKE_TOOLCHAIN_FILE', 'VCPKG_ROOT', 'QT_HOST_PATH'):
            del environment[name]
    paths = []
    for part in environment.get('PATH', '').split(os.pathsep):
        if not part:
            continue
        candidate = Path(part).resolve()
        if not candidate.is_relative_to(reference):
            paths.append(part)
    environment['PATH'] = os.pathsep.join([str(prefix / 'bin'), *paths])
    environment['CMAKE_PREFIX_PATH'] = str(prefix)
    environment['QT_ROOT_DIR'] = str(prefix)
    return environment


def configure_command(name, source, build, prefix, python):
    arguments = ['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja Multi-Config',
                 '-DCMAKE_CONFIGURATION_TYPES=' + ';'.join(CONFIGURATIONS),
                 '-DCMAKE_DEFAULT_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
                 '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_PREFIX_PATH=' + str(prefix),
                 '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
                 '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL',
                 '-DBUILD_SHARED_LIBS=ON', '-DBUILD_WITH_PCH=OFF', '-DFEATURE_msvc_obj_debug_info=ON',
                 '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                 '-DQT_INSTALL_CONFIG_INFO_FILES=ON', '-DQT_GENERATE_SBOM=ON', '-DQT_GENERATE_SOURCE_SBOM=ON',
                 '-DQT_SBOM_REQUIRE_GENERATE_SPDX_V2_JSON=ON', '-DQT_SBOM_REQUIRE_VERIFY_SPDX_V2=ON',
                 '-DQT_SBOM_PYTHON_INTERP=' + str(python)]
    if name == 'qtbase':
        arguments += ['-DFEATURE_shared=ON', '-DFEATURE_gui=ON', '-DFEATURE_widgets=ON',
                      '-DFEATURE_network=ON', '-DFEATURE_accessibility=ON', '-DFEATURE_thread=ON',
                      '-DFEATURE_opengl=ON', '-DFEATURE_schannel=ON', '-DFEATURE_openssl=OFF',
                      '-DFEATURE_windeployqt=ON', '-DFEATURE_tuiotouch=ON', '-DINPUT_zlib=qt', '-DINPUT_libpng=qt',
                      '-DINPUT_libjpeg=qt', '-DINPUT_pcre=qt', '-DINPUT_harfbuzz=qt', '-DINPUT_doubleconversion=qt']
    elif name == 'qttools':
        # Linguist's official CMake target builds and installs lrelease and its
        # CMake API. Do not replace it with a locally rewritten translation tool.
        arguments += ['-DFEATURE_linguist=ON']
        arguments += ['-DFEATURE_' + feature + '=OFF' for feature in
                      ('assistant', 'designer', 'distancefieldgenerator', 'kmap2qmap', 'pixeltool',
                       'qdbus', 'qdoc', 'qev', 'qtattributionsscanner', 'qtdiag', 'qtplugininfo', 'objcnamemangler')]
    return arguments


def checked_checkout(source, pin):
    bundle.require(source.is_dir() and not source.is_symlink(), 'Missing real Qt source checkout: ' + str(source))
    bundle.require(bundle.git(source, 'rev-parse', 'HEAD').decode().strip() == pin['commit'],
                   'Qt source checkout differs from its locked commit')
    bundle.require(bundle.git(source, 'remote', 'get-url', 'origin').decode().strip() == pin['repository'],
                   'Qt checkout has another original repository')
    CHECKOUT['verify_policy'](source)
    bundle.require(not bundle.git(source, 'status', '--porcelain', '--untracked-files=normal').strip(),
                   'Qt source checkout is not clean')


def restore_tools(source, pin, recorder, environment, *, component='Qttools'):
    source.mkdir()
    for arguments in (('init', '--quiet'), ('config', 'core.autocrlf', 'false')):
        recorder.run(bundle.git_command(source, *arguments), cwd=source, env=environment)
    CHECKOUT['install_policy'](source)
    CHECKOUT['retain_policy'](source, recorder)
    for arguments in (('remote', 'add', 'origin', pin['repository']),
                      ('-c', 'protocol.https.allow=always', 'fetch', '--depth=1', '--no-tags', 'origin', pin['commit']),
                      ('checkout', '--detach', 'FETCH_HEAD'),
                      ('-c', 'protocol.https.allow=always', 'fetch', '--depth=1', '--no-tags', 'origin',
                       'refs/tags/' + pin['tag'] + ':refs/tags/' + pin['tag'])):
        # The collector disables every transport. Only these two explicit
        # fetches of module_pins()'s fixed official HTTPS origin may use HTTPS.
        recorder.run(bundle.git_command(source, *arguments), cwd=source, env=environment)
    tag = 'refs/tags/' + pin['tag']
    bundle.require(bundle.git(source, 'rev-parse', tag).decode().strip() == pin['tagObject'] and
                   bundle.git(source, 'rev-parse', tag + '^{commit}').decode().strip() == pin['commit'],
                   'The real ' + component + ' release tag object differs from the reviewed lock')
    checked_checkout(source, pin)


def configuration_records(build):
    paths = [build / 'CMakeCache.txt', build / 'build.ninja', build / 'cmake_install.cmake']
    paths += sorted((build / 'CMakeFiles').glob('impl-*.ninja'))
    paths += [build / 'CMakeFiles/rules.ninja']
    paths += [build / name for name in ('compile_commands.json', 'config.summary') if (build / name).is_file()]
    paths += sorted(build.glob('install_manifest*.txt'))
    # Qttranslations generates English fallback input/catalog metadata from its
    # original CMake sources. Retain these real generated inputs as well.
    paths += sorted((build / 'translations/en_ts').glob('*.ts'))
    paths += [build / 'translations/catalogs.json'] if (build / 'translations/catalogs.json').is_file() else []
    return paths


def verify_generated_spdx(prefix, version, modules=('qtbase', 'qtsvg', 'qttools', 'qttranslations')):
    records = []
    bundle.require(modules and len(set(modules)) == len(modules) and
                   all(module in ('qtbase', 'qtsvg', 'qtwayland', 'qttools', 'qttranslations') for module in modules),
                   'Select each actual Qt SPDX module once')
    for module in modules:
        path = prefix / 'sbom' / (module + '-' + version + '.spdx.json')
        document = bundle.read_json(path)
        bundle.require(document.get('spdxVersion') == 'SPDX-2.3', 'Qt did not generate the required original SPDX format')
        seen = {}
        for item in document.get('files', []):
            name = bundle.relative(item['fileName'].removeprefix('./'))
            expected = [value['checksumValue'].lower() for value in item.get('checksums', [])
                        if value.get('algorithm') == 'SHA1']
            bundle.require(len(expected) == 1 and bundle.HEX40.fullmatch(expected[0]), 'Generated Qt file has no exact SPDX SHA1')
            actual = prefix / name
            bundle.require(actual.is_file() and actual.resolve().is_relative_to(prefix) and
                           bundle.digest(actual, 'sha1') == expected[0],
                           'Newly built Qt file differs from its generated SPDX: ' + name)
            bundle.require(name not in seen or seen[name] == expected[0], 'Conflicting multi-config Qt SPDX output')
            seen[name] = expected[0]
        bundle.require(seen, 'Generated Qt SPDX contains no installed files')
        records.append({'module': module, 'document': bundle.descriptor(path), 'verifiedFiles': len(seen)})
    return records


def verify_runtime(prefix):
    meta = bundle.packaging_metadata()
    files = []
    for suffix in ('', 'd'):
        files += ['bin/Qt6' + name + suffix + '.dll' for name in ('Core', 'Gui', 'Widgets', 'Network', 'Svg')]
        files += ['plugins/platforms/qwindows' + suffix + '.dll', 'plugins/tls/qschannelbackend' + suffix + '.dll',
                  'plugins/generic/qtuiotouchplugin' + suffix + '.dll']
    files += ['bin/windeployqt.exe', 'bin/lrelease.exe', 'bin/lconvert.exe']
    for name in files:
        path = prefix / name
        bundle.require(path.is_file() and meta.machine(path) == 'windows', 'Missing native Windows Qt output: ' + name)
    qms = sorted((prefix / 'translations').glob('*.qm'))
    bundle.require(qms and all((prefix / 'translations' / ('qtbase_' + language + '.qm')).is_file()
                             for language in ('zh_CN', 'zh_TW', 'ja', 'ru')),
                   'New Qt translations are missing the product languages')
    return {'files': [{'path': name, **API['identity'](prefix / name)} for name in files],
            'translations': [{'path': path.relative_to(prefix).as_posix(), **API['identity'](path)} for path in qms]}


def build(options):
    bundle.require(platform.system() == 'Windows' and platform.machine().lower() in ('amd64', 'x86_64'),
                   'Build this Qt runtime on the actual Windows x64 MSVC runner')
    bundle.require(os.environ.get('VCToolsVersion', '').startswith('14.') and shutil.which('cl') and shutil.which('link'),
                   'Initialize the real MSVC v143 x64 development environment before building Qt')
    lock_path = ROOT / 'build/dependencies.lock.json'
    lock_identity = API['identity'](lock_path)
    qt, pins = module_pins(bundle.read_json(lock_path))
    sources, reference = options.source_workspace.resolve(strict=True), options.reference_sdk.resolve(strict=True)
    workspace = options.workspace.resolve()
    bundle.require(1 <= options.jobs <= 64 and not workspace.exists() and
                   not any(workspace.is_relative_to(path) or path.is_relative_to(workspace) for path in (sources, reference)),
                   'Use a new separate Qt build workspace and a bounded parallel job count')
    for module in ('qtbase', 'qtsvg', 'qttranslations'):
        checked_checkout(sources / module, pins[module])
    baseline = reference / 'sbom' / ('qtbase-' + qt['version'] + '.spdx.json')
    baseline_identity = API['identity'](baseline)
    original = bundle.read_json(baseline)
    roots = [entry for entry in original['packages'] if entry['name'] == 'qtbase']
    bundle.require(len(roots) == 1 and roots[0]['downloadLocation'].endswith('@' + pins['qtbase']['commit']) and
                   'configuration for Windows' in roots[0].get('comment', ''),
                   'The supplied reference is not the original locked Windows Qt SDK')
    workspace.mkdir(parents=True)
    prefix = workspace / 'install'
    environment = clean_environment(reference, prefix)
    report = {'schemaVersion': 1, 'kind': 'huhucapture-built-windows-qt', 'qtVersion': qt['version'],
              'prefix': str(prefix), 'platform': 'windows', 'architecture': 'x64',
              'configurations': list(CONFIGURATIONS), 'sourceModules': [], 'buildComplete': False,
              'correspondingSourceComplete': False, 'failures': [],
              'scope': 'New shared MSVC Qt runtime, tools and translations; not a reproduction of the signed SDK',
              'sdkConfigurationDifferences': ['Actual local compiler/tools/prefix, not the old vendor paths',
                  'Original Git blob bytes on both hosts; upstream working-tree text/encoding/filter conversion is disabled',
                  'Schannel TLS used by this Windows product; no unrelated OpenSSL SDK or PostgreSQL input',
                  'Bundled Qt image/text dependencies retained inside the full pinned qtbase source',
                  'Only Linguist tools requested from Qttools; no QDoc/Designer/Assistant application'],
              'dependencyLockSha256': lock_identity['sha256']}
    control = API['ExecutionLog'](workspace / 'control', 'windows', __file__)
    control.retain_record(lock_path)
    control.retain_record(baseline)
    for name in ('config_qtbase.opt', 'config_qtbase.summary'):
        if (reference / name).is_file():
            control.retain_record(reference / name)
    try:
        control.run([sys.executable, '-c', 'import spdx_tools.spdx.clitools.pyspdxtools; '
                     'import importlib.metadata; print(importlib.metadata.version("spdx-tools"))'],
                    cwd=workspace, env=environment)
        control.run(['reuse', '--version'], cwd=workspace, env=environment)
        tools_source = workspace / 'qttools-source'
        restore_tools(tools_source, pins['qttools'], control, bundle.git_environment())
        for name in ('qtbase', 'qtsvg', 'qttools', 'qttranslations'):
            source = tools_source if name == 'qttools' else sources / name
            directory = workspace / (name + '-build')
            recorder = API['NativeBuild'](workspace / (name + '-source-receipt'), 'windows', __file__,
                                           name, source, pins[name], prefix)
            recorder.retain_record(lock_path)
            recorder.retain_record(baseline)
            CHECKOUT['retain_policy'](source, recorder)
            for program in ('cl', 'link', 'lib', 'ninja', 'reuse', sys.executable):
                recorder.observe_tool(program, environment)
            command = configure_command(name, source, directory, prefix, Path(sys.executable).resolve())
            configuration = recorder.run(command, cwd=workspace, env=environment)
            cache = (directory / 'CMakeCache.txt').read_text(encoding='utf-8')
            installed = re.findall(r'^CMAKE_INSTALL_PREFIX:PATH=(.*)$', cache, re.MULTILINE)
            bundle.require(len(installed) == 1 and Path(installed[0]).resolve() == prefix,
                           'Qt configure selected a different installation prefix')
            for variant in CONFIGURATIONS:
                recorder.run(['cmake', '--build', str(directory), '--config', variant, '--parallel', str(options.jobs)],
                             cwd=workspace, env=environment)
                recorder.run(['cmake', '--install', str(directory), '--config', variant], cwd=workspace, env=environment)
            CHECKOUT['verify_policy'](source)
            plan = recorder.finish(configure_index=configuration, configuration_files=configuration_records(directory))
            report['sourceModules'].append({'id': name, 'repository': pins[name]['repository'],
                'commit': pins[name]['commit'], 'sourceDirectory': str(source), 'sourceBuildReceipt': plan})
        report['generatedSbom'] = verify_generated_spdx(prefix, qt['version'])
        report['runtime'] = verify_runtime(prefix)
        for name, argument in (('windeployqt', '--version'), ('lrelease', '-version'), ('lconvert', '-help')):
            control.run([str(prefix / 'bin' / (name + '.exe')), argument],
                        cwd=workspace, env=environment)
        reference_qms = {path.name: API['identity'](path) for path in (reference / 'translations').glob('*.qm')}
        built_qms = {Path(item['path']).name: {key: item[key] for key in ('size', 'sha256')}
                     for item in report['runtime']['translations']}
        report['referenceTranslationComparison'] = {'referenceFiles': len(reference_qms), 'builtFiles': len(built_qms),
            'identicalNames': sorted(name for name, value in reference_qms.items() if built_qms.get(name) == value),
            'differentOrAbsentNames': sorted(name for name, value in reference_qms.items() if built_qms.get(name) != value),
            'meaning': 'Observed byte comparison only; the new build is independently attributed to its actual source'}
        bundle.require(API['identity'](lock_path) == lock_identity and API['identity'](baseline) == baseline_identity,
                       'Original source lock or reference SDK evidence changed')
        for item in report['sourceModules']:
            checked_checkout(Path(item['sourceDirectory']), pins[item['id']])
        record = control.raw_record({'dependencyLock': lock_identity, 'referenceSdk': bundle.descriptor(baseline)},
                                    {'prefix': str(prefix), 'files': bundle.inventory(prefix)})
        report['executionRecord'] = {'path': str(record), **API['identity'](record)}
        report['buildComplete'] = True
        return report
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report['failures'].append(str(error))
        raise
    finally:
        API['write_new'](workspace / 'build-info.json', report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-workspace', type=Path, required=True,
                        help='Original locked qtbase/qtsvg/qttranslations Git checkouts from restore-qt-module-sources.py')
    parser.add_argument('--reference-sdk', type=Path, required=True, help='Unmodified original Windows SDK used only as configuration evidence')
    parser.add_argument('--workspace', type=Path, required=True, help='New empty native build workspace; output prefix is install/')
    parser.add_argument('--jobs', type=int, default=2)
    options = parser.parse_args()
    try:
        result = build(options)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('Windows Qt source build failed: ' + str(error), file=sys.stderr)
        return 1
    API['write_module_plan'](options.workspace, result, 'windows')
    print(json.dumps({'prefix': result['prefix'], 'buildComplete': True, 'correspondingSourceComplete': False,
                      'sourceModules': len(result['sourceModules']), 'translations': len(result['runtime']['translations'])}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
