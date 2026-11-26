#!/usr/bin/env python3
"""Produce a new type-2 runtime from a real musl toolchain and original sources.

This does not reproduce the previous official AppImage asset. Every final
archive/CRT input must match a recorded source producer and the actual link map.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HELPER = runpy.run_path(str(ROOT / 'build/package/appimage_runtime.py'))
API, bundle = HELPER['API'], HELPER['bundle']
PATCHES = HELPER['PATCHES']
HEADERS = HELPER['HEADERS']
TARGET = 'x86_64-linux-musl'
TOOLCHAIN_INPUTS = ('appimage-toolchain-build', 'appimage-gcc', 'appimage-binutils', 'appimage-musl',
                    'appimage-gmp', 'appimage-mpc', 'appimage-mpfr', 'appimage-linux-headers')


def descriptor(path):
    return {'path': str(Path(path).resolve()), **API['identity'](path)}


def clean_environment():
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith(('CMAKE_', 'QT_', 'QT6', 'Qt6', 'GIT_', 'PKG_CONFIG_', 'LD_')) or \
                name.endswith(('_FOR_BUILD', '_FOR_TARGET')) or name in (
                'CC', 'CXX', 'CPP', 'AR', 'AS', 'LD', 'NM', 'RANLIB', 'STRIP', 'OBJCOPY',
                'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBRARY_PATH', 'CPATH',
                'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_RUN_PATH',
                'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'CONFIG_SITE', 'CONFIG_SHELL', 'DESTDIR', 'MAKEFLAGS',
                'MFLAGS', 'VCPKG_ROOT', 'QTDIR', 'QT_HOST_PATH', 'BASH_ENV', 'ENV', 'SHELL'):
            del environment[name]
    # This Linux producer's documented bootstrap programs are installed by the
    # native package manager. Do not inherit an SDK's prepended compiler PATH.
    environment['PATH'] = os.defpath
    environment['LC_ALL'] = 'C.UTF-8'
    environment['CONFIG_SITE'] = os.devnull
    return environment


def new_path(path):
    path = Path(path)
    bundle.require(not path.is_symlink() and not path.exists(), 'Use a new separate runtime build workspace')
    value = path.resolve()
    bundle.require(re.fullmatch(r'/[A-Za-z0-9_./-]+', str(value)),
                   'Use a runtime workspace path without Make/shell whitespace or metacharacters')
    return value


def fetch_archive(pin, path, recorder, environment):
    spec = pin['sourceArchive']
    recorder.run(['curl', '--proto', '=https', '--proto-redir', '=https', '--fail', '--location',
                  '--retry', '3', '--retry-delay', '2', '--connect-timeout', '30', '--max-time', '600',
                  '--max-filesize', str(spec.get('maximumBytes', spec.get('size', 256 * 1024**2))),
                  '--output', str(path), spec['url']], cwd=path.parent, env=environment)
    bundle.require(bundle.digest(path) == spec['sha256'] and
                   ('size' not in spec or path.stat().st_size == spec['size']),
                   'The original runtime/toolchain source archive differs from its exact pin')
    if spec.get('sha1'):
        bundle.require(bundle.digest(path, 'sha1') == spec['sha1'], 'The canonical cross-make archive hash differs')
    return path


def restore_input(name, pin, workspace, recorder, environment, *, original_archive=None):
    directory = workspace / 'sources' / name
    patch_replay = None
    if 'sourceArchive' in pin:
        archive = Path(original_archive) if original_archive is not None else workspace / 'downloads' / (name + '-' + pin['sourceArchive']['url'].split('/')[-1])
        if original_archive is None:
            fetch_archive(pin, archive, recorder, environment)
        spec = pin['sourceArchive']
        bundle.require(bundle.digest(archive) == spec['sha256'] and
                       ('size' not in spec or archive.stat().st_size == spec['size']), 'Original source archive changed')
        files = bundle.extract_tar(archive, directory, pin['archivePrefix'],
                                   path_profile=HEADERS['source_profile'](name, pin))
        patches = []
        for item in pin.get('patches', []):
            path = ROOT / bundle.relative(item['path'])
            bundle.require(bundle.digest(path) == item['sha256'], 'An original source patch changed')
            recorder.retain_record(path)
            patches.append(path)
        if 'patchApplication' in pin:
            actual = PATCHES['apply'](name, directory, patches, pin, ROOT, bundle.__dict__,
                                      recorder=recorder, environment=environment)
            replay_path = workspace / 'source-inputs' / (name + '-patch-replay.json')
            API['write_new'](replay_path, actual)
            recorder.retain_record(replay_path)
            patch_replay = descriptor(replay_path)
        else:
            bundle.apply_patches(directory, patches)
        files = HELPER['source_inventory'](name, pin, directory)
        collection = {'id': name, 'kind': 'tar', 'archive': str(archive), 'prefix': pin['archivePrefix']}
    else:
        checkout = workspace / 'checkouts' / name
        checkout.mkdir()
        git_env = bundle.git_environment()
        for arguments in (('init', '--quiet'), ('config', 'core.autocrlf', 'false'),
                          ('remote', 'add', 'origin', pin['repository']),
                          ('-c', 'protocol.https.allow=always', 'fetch', '--depth=1', '--no-tags', 'origin', pin['commit']),
                          ('checkout', '--detach', 'FETCH_HEAD')):
            recorder.run(bundle.git_command(checkout, *arguments), cwd=checkout, env=git_env)
        if pin.get('tag'):
            ref = 'refs/tags/' + pin['tag']
            recorder.run(bundle.git_command(checkout, '-c', 'protocol.https.allow=always', 'fetch', '--depth=1',
                                           '--no-tags', 'origin', ref + ':' + ref), cwd=checkout, env=git_env)
            bundle.require(bundle.git(checkout, 'rev-parse', ref).decode().strip() == pin['tagObject'] and
                           bundle.git(checkout, 'rev-parse', ref + '^{commit}').decode().strip() == pin['commit'],
                           'The actual original library tag differs from the exact source pin')
        bundle.require(bundle.git(checkout, 'rev-parse', 'HEAD').decode().strip() == pin['commit'] and
                       not bundle.git(checkout, 'status', '--porcelain').strip(), 'The original Git source is not the exact clean input')
        _, files = bundle.export_git(checkout, pin['commit'], directory)
        collection = {'id': name, 'directory': str(checkout)}
    source_hash = bundle.tree_digest(files)
    record = workspace / 'source-inputs' / (name + '.json')
    API['write_new'](record, {'id': name, 'pin': pin, 'sourceTreeSha256': source_hash, 'files': files,
                            **({'patchReplay': patch_replay} if patch_replay else {})})
    recorder.retain_record(record)
    return {'id': name, 'directory': directory, 'files': files, 'sourceTreeSha256': source_hash,
            'manifest': descriptor(record), 'collection': collection, 'pin': pin}


class Build(API['ExecutionLog']):
    """One actually executed build, possibly consuming several original sources."""
    def __init__(self, directory, name, inputs, prefix):
        super().__init__(directory, 'linux', __file__)
        self.name, self.inputs, self.prefix = name, list(inputs), Path(prefix)
        self.before = self.output_inventory() if prefix.exists() else []
        self.install_phases = []
        self.retain_record(ROOT / 'build/package/dependencies.lock.json')
        self.retain_record(ROOT / 'build/package/appimage_runtime.py')
        self.retain_record(ROOT / 'build/package/linux_header_paths.py')
        for item in self.inputs:
            self.retain_record(item['manifest']['path'])
        self.check_sources()

    def check_sources(self):
        for item in self.inputs:
            bundle.require(HELPER['source_inventory'](item['id'], item['pin'], item['directory']) == item['files'],
                           'An actual source input changed: ' + item['id'])

    def output_inventory(self):
        return bundle.inventory(self.prefix, path_profile=HEADERS['SYSROOT'] if self.name == 'appimage-toolchain' else None)

    def installation(self, component, command, cwd, environment):
        before = self.output_inventory() if self.prefix.exists() else []
        index = self.run(command, cwd=cwd, env=environment)
        after = self.output_inventory()
        old = {item['path']: item for item in before}
        changed = [item for item in after if item != old.get(item['path'])]
        record = {'component': component, 'commandIndex': index, 'prefix': str(self.prefix),
                  'before': before, 'after': after, 'changed': changed}
        path = self.directory / ('installation-' + str(len(self.install_phases)) + '.json')
        API['write_new'](path, record)
        self.retain_record(path)
        self.install_phases.append(record)
        return changed

    def finish(self, configuration_files, *, static_inputs=(), output_owners=None):
        self.check_sources()
        for path in configuration_files:
            self.retain_record(path)
        after = self.output_inventory()
        before = {item['path']: item for item in self.before}
        changed = [item for item in after if item != before.get(item['path'])]
        outputs = [{key: item[key] for key in ('path', 'size', 'sha256')} for item in changed
                   if item['type'] == 'file' and item['size']]
        bundle.require(outputs, 'No actual source-build outputs were produced')
        sources = [{'id': item['id'], 'sourceTreeSha256': item['sourceTreeSha256']} for item in self.inputs]
        execution = self.raw_record({'sources': sources, 'prefixBefore': self.before},
                                    {'prefix': str(self.prefix), 'files': changed, 'installationPhases': self.install_phases})
        self.retain_record(execution)
        static = [{key: item[key] for key in ('id', 'component', 'sourceTreeSha256', 'size', 'sha256')} for item in static_inputs]
        value = {'schemaVersion': 1, 'kind': 'huhucapture-source-build-receipt', 'platform': 'linux',
                 'commands': self.commands, 'commandOrigins': self.origins, 'toolchain': self.tools,
                 'environment': {'executions': [item['environment'] for item in self.executions]},
                 'sources': sources, 'staticInputs': static, 'outputs': outputs, 'outputOwners': output_owners or {}}
        path = self.directory / 'source-build-receipt.json'
        API['write_new'](path, value)
        plan = {'id': self.name, 'path': str(path),
                'buildRecords': [{'path': item['path'], 'sha256': item['sha256']} for item in self.records],
                'staticFiles': {item['id']: item['path'] for item in static_inputs}}
        API['write_new'](self.directory / 'receipt-plan.json', plan)
        bundle.require(self.output_inventory() == after, 'Actual source-build outputs changed while recording')
        self.check_sources()
        report = {'id': self.name, 'receipt': descriptor(path), 'execution': descriptor(execution),
                  'prefix': str(self.prefix), 'buildRecords': [descriptor(item['path']) for item in self.records]}
        return plan, report, value


def build_toolchain(workspace, inputs, specification, environment, jobs):
    directory, prefix = workspace / 'toolchain-build', workspace / 'toolchain'
    directory.mkdir()
    # Canonical litecross passes $(PWD)/obj_binutils to GCC configure. A
    # subprocess cwd alone does not update an inherited shell PWD variable.
    environment = {**environment, 'PWD': str(directory)}
    build = Build(workspace / 'toolchain-source-receipt', 'appimage-toolchain',
                  [inputs[name] for name in TOOLCHAIN_INPUTS], prefix)
    for program in ('gcc', 'g++', 'make', 'ar', 'as', 'ld', 'ranlib', 'awk', 'sed', 'perl', 'sh'):
        build.observe_tool(program, environment)
    options = specification['toolchainConfiguration']
    lines = ['TARGET = ' + TARGET, 'OUTPUT = ' + str(prefix),
             'COMMON_CONFIG = ' + ' '.join(options['common']), 'GCC_CONFIG = ' + ' '.join(options['gcc']),
             'MUSL_CONFIG = ' + ' '.join(options['musl'])]
    for variable, component in (('GCC', 'gcc'), ('BINUTILS', 'binutils'), ('MUSL', 'musl'),
                                ('GMP', 'gmp'), ('MPC', 'mpc'), ('MPFR', 'mpfr'), ('LINUX', 'linux-headers')):
        lines.append(variable + '_SRCDIR = ' + str(inputs['appimage-' + component]['directory']))
    config = directory / 'config.mak'
    config.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    makefile = inputs['appimage-toolchain-build']['directory'] / 'litecross/Makefile'
    command = ['make', '-f', str(makefile), '-j' + str(jobs)]
    build.run([*command, 'all'], cwd=directory, env=environment)
    owners = {}
    for target, component in (('install-musl', 'appimage-musl'), ('install-gcc', 'appimage-gcc'),
                              ('install-binutils', 'appimage-binutils'), ('install-kernel-headers', 'appimage-linux-headers')):
        changed = build.installation(component, [*command, 'OUTPUT=' + str(prefix), target], directory, environment)
        for item in changed:
            if item['type'] == 'file':
                owners[item['path']] = component
    compiler = prefix / 'bin' / (TARGET + '-gcc')
    actual, _ = build.run([compiler, '-dumpmachine'], cwd=directory, env=environment, capture=True)
    bundle.require(actual.strip() == TARGET, 'The actual new compiler targets another platform')
    actual, _ = build.run([compiler, '-print-sysroot'], cwd=directory, env=environment, capture=True)
    sysroot = Path(actual.strip()).resolve(strict=True)
    bundle.require(sysroot == prefix / TARGET, 'The actual new compiler selected an external sysroot')
    for name in ('libgcc.a', 'libgcc_eh.a', 'crtbeginS.o', 'crtendS.o', 'rcrt1.o', 'crti.o', 'crtn.o', 'libc.a'):
        actual, _ = build.run([compiler, '-print-file-name=' + name], cwd=directory, env=environment, capture=True)
        path = Path(actual.strip()).resolve(strict=True)
        bundle.require(path.is_relative_to(prefix) and path.is_file() and path.relative_to(prefix).as_posix() in owners,
                       'A compiler support archive or CRT object has no actual new producer output: ' + name)
    for name in ('ld', 'as', 'cc1', 'cc1plus', 'collect2'):
        actual, _ = build.run([compiler, '-print-prog-name=' + name], cwd=directory, env=environment, capture=True)
        path = Path(actual.strip()).resolve(strict=True)
        bundle.require(path.is_relative_to(prefix) and path.is_file() and path.relative_to(prefix).as_posix() in owners,
                       'The actual new compiler selected an external compiler/linker program: ' + name)
        build.observe_tool(path, environment)
    records = [config, directory / 'obj_musl/config.mak']
    records += sorted(directory.glob('obj_*/config.log')) + sorted(directory.glob('obj_*/config.status'))
    plan, report, receipt = build.finish(records, output_owners=owners)
    return prefix, sysroot, owners, plan, report, receipt


def target_environment(workspace, toolchain, prefix, original):
    environment = dict(original)
    environment['PATH'] = os.pathsep.join([str(toolchain / 'bin'), environment['PATH']])
    # Put -static-pie in the compiler command as well as linker flags: zlib's
    # actual configure probes do not consistently consume LDFLAGS.
    environment['CC'] = str(toolchain / 'bin' / (TARGET + '-gcc')) + ' -static-pie'
    environment['CXX'] = str(toolchain / 'bin' / (TARGET + '-g++')) + ' -static-pie'
    for variable, leaf in (('AR', 'ar'), ('AS', 'as'), ('LD', 'ld'), ('NM', 'nm'), ('RANLIB', 'ranlib'),
                           ('STRIP', 'strip'), ('OBJCOPY', 'objcopy')):
        environment[variable] = str(toolchain / 'bin' / (TARGET + '-' + leaf))
    environment.update(CFLAGS='-Os -fPIC -ffunction-sections -fdata-sections',
                       CXXFLAGS='-Os -fPIC -ffunction-sections -fdata-sections',
                       CPPFLAGS='-I' + str(prefix / 'include'),
                       LDFLAGS='-static-pie -L' + str(prefix / 'lib'),
                       PKG_CONFIG_PATH=str(prefix / 'lib/pkgconfig'),
                       PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'))
    return environment


def cmake_command(source, directory, prefix, toolchain, sysroot):
    return ['cmake', '-S', str(source), '-B', str(directory), '-G', 'Ninja',
            '-DCMAKE_C_COMPILER=' + str(toolchain / 'bin' / (TARGET + '-gcc')),
            '-DCMAKE_CXX_COMPILER=' + str(toolchain / 'bin' / (TARGET + '-g++')),
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
            '-DCMAKE_EXE_LINKER_FLAGS=-static-pie', '-DCMAKE_SYSROOT=' + str(sysroot),
            '-DCMAKE_FIND_ROOT_PATH=' + str(prefix) + ';' + str(sysroot),
            '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER', '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']


def build_library(name, workspace, inputs, prefix, toolchain, sysroot, environment, jobs):
    source, directory = inputs[name]['directory'], workspace / (name + '-build')
    build = Build(workspace / (name + '-source-receipt'), name, [inputs[name]], prefix)
    for program in ('make', 'cmake', 'ninja', 'pkg-config', environment['AR'], environment['RANLIB'],
                    toolchain / 'bin' / (TARGET + '-gcc'), toolchain / 'bin' / (TARGET + '-g++')):
        build.observe_tool(program, environment)
    records = []
    if name == 'appimage-fuse':
        command = ['meson', 'setup', str(directory), str(source), '--prefix=' + str(prefix), '--libdir=lib',
                   '--buildtype=release', '--default-library=static', '-Db_staticpic=true',
                   '-Dutils=false', '-Dexamples=false', '-Dtests=false', '-Duseroot=false',
                   '-Ddisable-libc-symbol-version=true']
        build.run(command, cwd=workspace, env=environment)
        build.run(['ninja', '-C', str(directory), '-j' + str(jobs)], cwd=workspace, env=environment)
        build.run(['meson', 'install', '-C', str(directory)], cwd=workspace, env=environment)
        records = [directory / 'build.ninja', directory / 'meson-info/intro-buildoptions.json',
                   directory / 'meson-info/intro-buildsystem_files.json', directory / 'meson-logs/meson-log.txt']
    elif name == 'appimage-zlib':
        directory.mkdir()
        build.run([str(source / 'configure'), '--static', '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib')],
                  cwd=directory, env=environment)
        build.run(['make', '-j' + str(jobs), 'libz.a'], cwd=directory, env=environment)
        build.run(['make', 'install'], cwd=directory, env=environment)
        records = [directory / 'Makefile', directory / 'configure.log', directory / 'zconf.h']
    elif name in ('appimage-zstd', 'appimage-mimalloc'):
        location = source / 'build/cmake' if name == 'appimage-zstd' else source
        command = cmake_command(location, directory, prefix, toolchain, sysroot)
        if name == 'appimage-zstd':
            command += ['-DZSTD_BUILD_STATIC=ON', '-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_PROGRAMS=OFF',
                        '-DZSTD_BUILD_TESTS=OFF', '-DZSTD_BUILD_CONTRIB=OFF']
        else:
            command += ['-DMI_BUILD_STATIC=ON', '-DMI_BUILD_SHARED=OFF', '-DMI_BUILD_OBJECT=OFF',
                        '-DMI_BUILD_TESTS=OFF', '-DMI_LIBC_MUSL=ON', '-DMI_INSTALL_TOPLEVEL=ON']
        build.run(command, cwd=workspace, env=environment)
        build.run(['cmake', '--build', str(directory), '--parallel', str(jobs)], cwd=workspace, env=environment)
        build.run(['cmake', '--install', str(directory)], cwd=workspace, env=environment)
        records = [directory / 'CMakeCache.txt', directory / 'build.ninja', directory / 'CMakeFiles/rules.ninja',
                   directory / 'compile_commands.json', directory / 'install_manifest.txt']
    elif name == 'appimage-squashfuse':
        # Upstream autogen creates configure and auxiliary files in its source
        # directory. Generate them in an owned exact copy, preserving originals.
        generated = workspace / 'squashfuse-generated'
        bundle.copy_verified_tree(source, generated, inputs[name]['files'])
        for program in ('autoconf', 'autoreconf', 'automake', 'aclocal', 'libtoolize', 'm4'):
            build.observe_tool(program, environment)
        build.run(['sh', str(generated / 'autogen.sh')], cwd=generated, env=environment)
        directory.mkdir()
        command = [str(generated / 'configure'), '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'),
                   '--disable-shared', '--enable-static', '--disable-demo', '--without-xz', '--without-lzo', '--without-lz4',
                   '--with-zstd=' + str(prefix), '--with-zlib=' + str(prefix)]
        build.run(command, cwd=directory, env=environment)
        build.run(['make', '-j' + str(jobs)], cwd=directory, env=environment)
        build.run(['make', 'install'], cwd=directory, env=environment)
        headers = prefix / 'include/squashfuse'
        headers.mkdir(parents=True, exist_ok=True)
        for path in sorted(generated.glob('*.h')):
            target = headers / path.name
            if target.exists():
                bundle.require(API['identity'](target) == API['identity'](path), 'Squashfuse header installation differs')
            else:
                shutil.copyfile(path, target)
        # config.h is an actual generated input used by installed squashfuse
        # private headers and the runtime; keep that output/configuration too.
        shutil.copyfile(directory / 'config.h', headers / 'config.h')
        records = [directory / 'Makefile', directory / 'config.log', directory / 'config.status', directory / 'config.h',
                   generated / 'configure']
    else:
        raise ValueError('Unknown runtime library producer')
    plan, report, receipt = build.finish(records)
    return plan, report, receipt


def write_notice(inputs, output):
    with output.open('xb') as stream:
        stream.write(b'New huhucapture AppImage type-2 runtime: original source notices\n'
                     b'This runtime is rebuilt from the exact source pins in the accompanying source bundle.\n'
                     b'GCC support objects retain the GCC Runtime Library Exception and their original notices below.\n')
        for name in sorted(HELPER['STATIC_IDS'] | {'appimage-runtime'}):
            item = inputs[name]
            notices = bundle.license_files(item['directory'], item['files'])
            bundle.require(notices, 'A runtime/static component has no retained original license notice: ' + name)
            for relative in notices:
                stream.write(('\n\n=== ' + name + '/' + relative + ' ===\n').encode('utf-8'))
                stream.write((item['directory'] / relative).read_bytes())
                stream.write(b'\n')


def probe_runtime(build, workspace, runtime, appimagetool, environment):
    root = workspace / 'probe'
    root.mkdir()
    appdir = root / 'AppDir'
    appdir.mkdir()
    runner = appdir / 'AppRun'
    runner.write_text('#!/bin/sh\nprintf "%s\\n" "huhucapture-runtime-source-probe"\n', encoding='utf-8')
    runner.chmod(0o755)
    (appdir / 'runtime-probe.desktop').write_text('[Desktop Entry]\nType=Application\nName=Runtime Source Probe\n'
        'Exec=AppRun\nIcon=runtime-probe\nTerminal=false\nCategories=Utility;\n', encoding='utf-8')
    (appdir / 'runtime-probe.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">'
                                            '<rect width="16" height="16" fill="#333"/></svg>\n', encoding='utf-8')
    image = root / 'probe.AppImage'
    selected = dict(environment)
    selected.update(APPIMAGE_EXTRACT_AND_RUN='1', ARCH='x86_64')
    packaging = build.run([appimagetool, '--runtime-file', runtime, '--comp', 'zstd', '--mksquashfs-opt', '-processors',
                           '--mksquashfs-opt', '1', appdir, image], cwd=root, env=selected)
    embedding = HELPER['runtime_embedding'](runtime, image)
    extracted = root / 'extract'
    extracted.mkdir()
    extraction = len(build.commands)
    build.run([image, '--appimage-extract'], cwd=extracted, env=selected)
    bundle.require(bundle.inventory(appdir) == bundle.inventory(extracted / 'squashfs-root'),
                   'The actual new runtime extraction differs from its complete probe payload')
    execution = len(build.commands)
    stdout, _ = build.run([image], cwd=root, env=selected, capture=True)
    bundle.require(stdout.strip() == 'huhucapture-runtime-source-probe', 'The actual new runtime did not execute AppRun')
    stdout_path = build.executions[execution]['stdout']
    retained = next(item for item in build.records if item['inputPath'] == stdout_path)
    report = {'schemaVersion': 1, 'runtimeSha256': bundle.digest(runtime), 'appImage': descriptor(image),
              'extractionMatched': True, 'executionMatched': True,
              'buildCommandIndex': packaging, 'runtimeEmbedding': embedding,
              'extractionCommandIndex': extraction, 'executionCommandIndex': execution,
              'stdout': descriptor(retained['path']), 'payloadTreeSha256': bundle.tree_digest(bundle.inventory(appdir)),
              'scope': 'Actual compressed payload extraction and AppRun execution without FUSE; not a desktop mount or product test'}
    path = root / 'probe.json'
    API['write_new'](path, report)
    build.retain_record(path)
    return path


def collect_link_inputs(map_path, owners, inputs):
    text = map_path.read_text(encoding='utf-8')
    result = []
    for path in HELPER['link_loads'](text):
        bundle.require(str(path) in owners, 'The linker consumed a static input with no actual producer: ' + str(path))
        owner = owners[str(path)]
        identity = API['identity'](path)
        bundle.require(identity == owner['identity'], 'A static producer output changed before final link')
        record = {'id': 'runtime-link-' + str(len(result)), 'component': owner['component'],
                  'sourceTreeSha256': inputs[owner['component']]['sourceTreeSha256'],
                  'path': str(path), **identity, 'producerReceipt': owner['receipt'], 'producerOutput': owner['output']}
        if path.suffix == '.a':
            record['members'] = HELPER['archive_members'](path)
            # A GNU link map also lists discarded sections. This field describes
            # observed member mentions, not a claim that all their sections survived.
            record['membersMentionedInMap'] = sorted(set(re.findall(re.escape(str(path)) + r'\(([^)]+)\)', text)))
            bundle.require(set(record['membersMentionedInMap']) <= {item['name'] for item in record['members']},
                           'The actual link map names a member absent from its retained archive')
        result.append(record)
    bundle.require(HELPER['STATIC_IDS'] <= {item['component'] for item in result},
                   'The actual runtime link omitted a required static library or compiler-support producer')
    return result


def register_outputs(owners, prefix, receipt, receipt_id, default_component=None):
    for item in receipt['outputs']:
        path = (prefix / item['path']).resolve(strict=True)
        if path.suffix not in ('.a', '.o'):
            continue
        component = default_component or receipt['outputOwners'].get(item['path'])
        bundle.require(component, 'A produced archive/CRT lacks its actual source installation owner')
        actual = API['identity'](path)
        bundle.require(actual == {key: item[key] for key in ('size', 'sha256')}, 'An actual producer output changed')
        owners[str(path)] = {'component': component, 'receipt': receipt_id, 'output': item['path'], 'identity': actual}


def link_runtime(workspace, inputs, prefix, toolchain, libraries, environment, specification, owners, appimagetool):
    prefix.mkdir()
    source = inputs['appimage-runtime']['directory'] / 'src/runtime'
    build = Build(workspace / 'runtime-source-receipt', 'appimage-runtime', [inputs['appimage-runtime']], prefix)
    compiler = toolchain / 'bin' / (TARGET + '-gcc')
    for program in (compiler, environment['AR'], environment['LD'], environment['STRIP'], environment['OBJCOPY'], sys.executable):
        build.observe_tool(program, environment)
    object_path = prefix / 'runtime.o'
    map_path = workspace / 'runtime-link.map'
    commit = inputs['appimage-runtime']['pin']['commit']
    defines = '-DGIT_COMMIT="https://github.com/AppImage/type2-runtime/commit/' + commit + '"'
    options = specification['runtimeLinkProfile']
    compilation = [argument for argument in options if not argument.startswith('-Wl,') and argument not in ('-static', '-static-pie')]
    build.run([compiler, *compilation, defines, '-I' + str(libraries / 'include'),
               '-I' + str(libraries / 'include/squashfuse'), '-I' + str(libraries / 'include/fuse3'),
               '-c', source / 'runtime.c', '-o', object_path], cwd=prefix, env=environment)
    owners[str(object_path)] = {'component': 'appimage-runtime', 'receipt': 'appimage-runtime',
                               'output': 'runtime.o', 'identity': API['identity'](object_path)}
    mimalloc, _ = build.run(['pkg-config', '--libs', '--static', 'mimalloc'], cwd=prefix, env=environment, capture=True)
    selected = shlex.split(mimalloc)
    allowed = {'-L' + str(libraries / 'lib'), '-lmimalloc', '-lpthread', '-lrt', '-latomic'}
    bundle.require('-lmimalloc' in selected and set(selected) <= allowed,
                   'Mimalloc selected an unreviewed target dependency or host search path')
    archive_names = ('libsquashfuse.a', 'libsquashfuse_ll.a', 'libzstd.a', 'libz.a', 'libfuse3.a')
    archives = [libraries / 'lib' / name for name in archive_names]
    bundle.require(all(path.is_file() and str(path) in owners for path in archives), 'An actual runtime static library was not produced')
    unstripped = prefix / 'runtime.unstripped'
    build.run([compiler, '-static', '-static-pie', '-Wl,--gc-sections', '-T', source / 'data_sections.ld',
               '-Wl,-Map=' + str(map_path) + ',--cref,-t,-t', object_path, *archives, *selected, '-o', unstripped],
              cwd=prefix, env=environment)
    HELPER['inspect_elf'](unstripped, marked=False)
    actual_inputs = collect_link_inputs(map_path, owners, inputs)
    closure = {'schemaVersion': 1, 'kind': 'huhucapture-runtime-static-link-closure',
               'map': descriptor(map_path), 'inputs': actual_inputs,
               'meaning': 'Every actual GNU LOAD archive/object, exact producer output and complete archive member bytes'}
    closure_path = workspace / 'runtime-link-closure.json'
    API['write_new'](closure_path, closure)
    runtime, debug = prefix / 'runtime-x86_64', prefix / 'runtime-x86_64.debug'
    build.run([environment['OBJCOPY'], '--only-keep-debug', unstripped, debug], cwd=prefix, env=environment)
    shutil.copyfile(unstripped, runtime)
    runtime.chmod(0o755)
    build.run([environment['STRIP'], '--strip-debug', '--strip-unneeded', runtime], cwd=prefix, env=environment)
    build.run([environment['OBJCOPY'], '--add-gnu-debuglink=' + str(debug), runtime], cwd=prefix, env=environment)
    # Upstream performs this exact type-2 marker write after strip/objcopy.
    marking = 'from pathlib import Path\nimport sys\np = Path(sys.argv[1])\nwith p.open("r+b") as f:\n f.seek(8)\n f.write(b"AI\\x02")\n'
    build.run([sys.executable, '-c', marking, runtime], cwd=prefix, env=environment)
    HELPER['inspect_elf'](runtime)
    version, errors = build.run([runtime, '--appimage-version'], cwd=prefix, env=environment, capture=True)
    bundle.require(commit in version + errors, 'The real runtime does not report its actual fixed original commit')
    offset, _ = build.run([runtime, '--appimage-offset'], cwd=prefix, env=environment, capture=True)
    bundle.require(int(offset.strip()) == runtime.stat().st_size, 'The real runtime reports a different appended payload offset')
    notice = prefix / 'LICENSE.runtime-sources'
    write_notice(inputs, notice)
    probe = probe_runtime(build, workspace, runtime, appimagetool, environment)
    plan, report, receipt = build.finish([map_path, closure_path, source / 'Makefile', source / 'data_sections.ld'], static_inputs=actual_inputs)
    return plan, report, receipt, runtime, notice, closure_path, probe


def build_runtime(options):
    bundle.require(platform.system() == 'Linux' and platform.machine().lower() in ('amd64', 'x86_64') and
                   1 <= options.jobs <= 64, 'Use a real Linux amd64 host and a bounded native job count')
    workspace = new_path(options.workspace)
    tools = options.tools.resolve(strict=True)
    bundle.require(not workspace.is_relative_to(tools) and not tools.is_relative_to(workspace),
                   'Keep original packaging inputs separate from the new source build')
    lock_path, lock, specification = HELPER['profile']()
    workspace.mkdir(parents=True)
    for name in ('sources', 'source-inputs', 'checkouts', 'downloads'):
        (workspace / name).mkdir()
    environment = clean_environment()
    report = {'schemaVersion': 1, 'kind': 'huhucapture-built-appimage-runtime', 'platform': 'linux',
              'architecture': 'amd64', 'runtimeCommit': lock['tools']['appimageRuntime']['commit'],
              'packageLockSha256': bundle.digest(lock_path), 'producerFiles': HELPER['producer_identity'](),
              'sourcePins': specification['sources'], 'sourceInputs': {}, 'receipts': [],
              'buildComplete': False, 'staticClosureComplete': False, 'correspondingSourceComplete': False, 'failures': [],
              'scope': 'A new static PIE runtime from complete actual source producers; not reproduction of the old official asset',
              'configurationDifferences': ['GCC 9.4.0/musl 1.2.6 source-built static target toolchain replaces floating Alpine/Clang inputs',
                  'Every canonical cross-make patch is retained; target shared toolchain libraries are disabled',
                  'Original fuse 3.15.0 patch and squashfuse 0.5.2 retained; zstd 1.5.6, zlib 1.3.1 and mimalloc 2.1.7 are exact new Git sources',
                  'Real compressed extraction/AppRun execution is checked; this does not establish a FUSE desktop mount or complete application release']}
    control = API['ExecutionLog'](workspace / 'control', 'linux', __file__)
    plans, inputs, owners = [], {}, {}
    try:
        for path in (lock_path, ROOT / 'build/package/appimage_runtime.py', ROOT / 'third_party/appimage/source-reviews.json',
                     ROOT / 'third_party/appimage/toolchain-patches/upstream.json', ROOT / 'build/package/linux_header_paths.py'):
            control.retain_record(path)
        for program in ('curl', 'git', 'gcc', 'g++', 'make', 'ar', 'ld', 'cmake', 'ninja', 'meson', 'pkg-config'):
            control.observe_tool(program, environment)
        for key, pin in lock['tools'].items():
            root = tools / key
            for name, expected in ((pin['name'], pin), ('source.tar.gz', pin['sourceArchive']), (pin['licenseFile'], pin['licenseRecord'])):
                path = root / name
                bundle.require(API['identity'](path) == {field: expected[field] for field in ('size', 'sha256')},
                               'An original official packaging asset/source/license differs from its retained pin: ' + key)
        original = lock['tools']['appimageRuntime']
        runtime_pin = {**original, 'archivePrefix': 'type2-runtime-' + original['commit']}
        inputs['appimage-runtime'] = restore_input('appimage-runtime', runtime_pin, workspace, control, environment,
            original_archive=tools / 'appimageRuntime/source.tar.gz')
        for name, pin in specification['sources'].items():
            inputs[name] = restore_input(name, pin, workspace, control, environment)
        cross = inputs['appimage-toolchain-build']['directory']
        for name, pin in specification['sources'].items():
            if pin.get('crossMakeHashFile'):
                words = (cross / pin['crossMakeHashFile']).read_text(encoding='ascii').split()
                bundle.require(words == [pin['sourceArchive']['sha1'], pin['sourceArchive']['url'].split('/')[-1]],
                               'The exact source archive differs from canonical cross-make provenance')
        patch_provenance = bundle.read_json(ROOT / 'third_party/appimage/toolchain-patches/upstream.json')
        for patch in patch_provenance['patches']:
            bundle.require((ROOT / patch['path']).read_bytes() == (cross / patch['upstreamPath']).read_bytes(),
                           'A retained toolchain patch differs from the actual fixed original Git source')
        for source in ('makefile', 'cowpatch'):
            original = patch_provenance['application'][source]
            data = (cross / original['upstreamPath']).read_bytes()
            bundle.require(bundle.object_digest('blob', data) == original['blob'],
                           'The actual canonical toolchain patch recipe changed')
            if source == 'cowpatch':
                bundle.require(data == (ROOT / original['path']).read_bytes(),
                               'The executed original cowpatch script differs from its restored source')
        bundle.require((ROOT / 'third_party/appimage/libfuse-mount.patch').read_bytes() ==
                       (inputs['appimage-runtime']['directory'] / 'patches/libfuse/mount.c.diff').read_bytes(),
                       'The retained fuse patch differs from the locked original runtime source')
        report['sourceInputs'] = {name: {key: item[key] for key in ('sourceTreeSha256', 'manifest')} for name, item in inputs.items()}
        toolchain, sysroot, _, plan, produced, receipt = build_toolchain(workspace, inputs, specification, environment, options.jobs)
        plans.append(plan); report['receipts'].append(produced)
        register_outputs(owners, toolchain, receipt, plan['id'])
        libraries = workspace / 'libraries'
        selected = target_environment(workspace, toolchain, libraries, environment)
        for name in ('appimage-fuse', 'appimage-zlib', 'appimage-zstd', 'appimage-mimalloc', 'appimage-squashfuse'):
            plan, produced, receipt = build_library(name, workspace, inputs, libraries, toolchain, sysroot, selected, options.jobs)
            plans.append(plan); report['receipts'].append(produced)
            register_outputs(owners, libraries, receipt, plan['id'], name)
        product = link_runtime(workspace, inputs, workspace / 'install', toolchain, libraries, selected, specification,
                               owners, tools / 'appimagetool' / lock['tools']['appimagetool']['name'])
        plan, produced, _, runtime, notice, closure, probe = product
        plans.append(plan); report['receipts'].append(produced)
        report.update(runtime=descriptor(runtime), notice=descriptor(notice), linkClosure=descriptor(closure), probe=descriptor(probe))
        for item in inputs.values():
            bundle.require(HELPER['source_inventory'](item['id'], item['pin'], item['directory']) == item['files'],
                           'An original runtime source changed')
        bundle.require(bundle.digest(lock_path) == report['packageLockSha256'] and
                       HELPER['producer_identity']() == report['producerFiles'], 'Runtime builder or locked inputs changed')
        execution = control.raw_record({'sources': report['sourceInputs']}, {'runtime': report['runtime'], 'linkClosure': report['linkClosure']})
        report['controlExecution'] = descriptor(execution)
        report['buildComplete'] = True
        report['staticClosureComplete'] = True
        HELPER['validate_report'](workspace, report)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report['buildComplete'] = False
        report['staticClosureComplete'] = False
        report['failures'].append(str(error))
        raise
    finally:
        API['write_new'](workspace / 'build-info.json', report)
    fragment = {'schemaVersion': 1, 'platform': 'linux', 'project': str(ROOT),
                'sources': [item['collection'] for item in inputs.values()], 'buildReceipts': plans,
                'materials': [{'id': 'appimage-runtime-source-build', 'path': str(workspace / 'build-info.json')},
                              {'id': 'appimage-runtime-link-closure', 'path': str(closure)},
                              {'id': 'appimage-runtime-native-probe', 'path': str(probe)}]}
    API['write_new'](workspace / 'source-plan-fragment.json', fragment)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True, help='Existing fixed original packaging tools and source archives')
    parser.add_argument('--workspace', type=Path, required=True, help='New separate native runtime/toolchain build workspace')
    parser.add_argument('--jobs', type=int, default=2)
    options = parser.parse_args()
    try:
        result = build_runtime(options)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('AppImage runtime source build failed: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps({'runtime': result['runtime']['path'], 'buildComplete': True, 'staticClosureComplete': True,
                      'correspondingSourceComplete': False,
                      'sourcePlan': str(options.workspace.resolve() / 'source-plan-fragment.json')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
