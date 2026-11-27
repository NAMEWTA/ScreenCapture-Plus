#!/usr/bin/env python3
"""Configure, build and install the product with actual pre/post source receipts."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import runpy
import shlex

ROOT = Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(ROOT / 'build/package/application_receipts.py'))
API, bundle = helpers['API'], helpers['bundle']


def linked_native_library(build, configuration, library):
    """Read CMake's actual generated link model, not a requested cache option."""
    reply = build / '.cmake/api/v1/reply'
    indices = sorted(reply.glob('index-*.json'))
    bundle.require(len(indices) == 1, 'Expected one new configure file-API index')
    index = bundle.read_json(indices[0])
    model = index['reply']['codemodel-v2']['jsonFile']
    model_path = reply / bundle.relative(model)
    configurations = bundle.read_json(model_path)['configurations']
    selected = [c for c in configurations if c['name'] == configuration]
    bundle.require(len(selected) == 1, 'CMake link model lacks the actual product configuration')
    targets = [t for t in selected[0]['targets'] if t['name'] == 'huhucapture']
    bundle.require(len(targets) == 1, 'CMake did not generate the one product executable')
    target_path = reply / bundle.relative(targets[0]['jsonFile'])
    target = bundle.read_json(target_path)
    bundle.require(target['type'] == 'EXECUTABLE', 'The product target is not an executable')
    paths = []
    for fragment in target.get('link', {}).get('commandFragments', []):
        if fragment['role'] != 'libraries':
            continue
        for token in shlex.split(fragment['fragment'], posix=os.name != 'nt'):
            token = token.strip('"')
            candidate = Path(token)
            if candidate.is_absolute() and candidate.is_file():
                paths.append(candidate.resolve())
            elif not candidate.is_absolute() and (build / candidate).is_file():
                paths.append((build / candidate).resolve())
    bundle.require(library in paths, 'The actual generated linker inputs must contain the verified Rust aggregate')
    return [indices[0], model_path, target_path]


def observe_compilers(log, directory, configuration, toolchains):
    for tool in bundle.read_json(toolchains)['toolchains']:
        compiler = tool['compiler']['path']
        if os.name == 'nt' and tool['language'] == 'RC' and compiler == 'rc':
            # The Visual Studio generator delegates this tool lookup to the
            # generated MSBuild project rather than using the caller's PATH.
            command = directory / 'package/msbuild-command.txt'
            log.retain_record(command)
            project = directory / 'huhucapture.vcxproj'
            output, _ = log.run([command.read_text(encoding='utf-8').strip(), str(project),
                '-nologo', '-p:Configuration=' + configuration, '-p:Platform=x64',
                '-getProperty:WindowsSdkDir,WindowsTargetPlatformVersion,PreferredToolArchitecture'],
                cwd=directory, capture=True)
            properties = json.loads(output)['Properties']
            sdk = Path(properties['WindowsSdkDir'])
            version = properties['WindowsTargetPlatformVersion']
            architecture = properties['PreferredToolArchitecture']
            bundle.require(sdk.is_absolute() and version and architecture in ('x64', 'x86', 'arm64'),
                           'MSBuild did not select a versioned Windows SDK resource compiler')
            compiler = sdk / 'bin' / version / architecture / 'rc.exe'
        log.observe_tool(compiler)


def build(options):
    directory, installed = options.build_directory.resolve(), options.installed.resolve()
    bundle.require(not directory.exists() and not installed.exists() and 1 <= options.jobs <= 64,
                   'Use new build/install directories and a bounded parallel job count')
    bundle.require(options.configuration in ('Release', 'RelWithDebInfo'), 'Rust requires a production CRT configuration')
    evidence = options.evidence.resolve()
    log = API['ExecutionLog'](evidence, 'windows' if os.name == 'nt' else 'linux', __file__)
    sources = helpers['SourceInputs'](options.source_plan, log)
    library = sources.checked_native_proof(options.native_receipt.resolve(strict=True))
    native_identity = API['identity'](library)
    query = directory / '.cmake/api/v1/query'
    query.mkdir(parents=True)
    (query / 'codemodel-v2').touch(exist_ok=False)
    (query / 'toolchains-v1').touch(exist_ok=False)
    generator = ['-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'v143'] if os.name == 'nt' else ['-G', 'Ninja']
    configure = ['cmake', '-S', str(ROOT), '-B', str(directory), *generator,
                 '-DCMAKE_BUILD_TYPE=' + options.configuration, '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
                 '-DHUHUCAPTURE_BUILD_APP=ON', '-DBUILD_TESTING=ON',
                 '-DHUHUCAPTURE_RUST_METADATA=' + str(options.native_receipt.resolve(strict=True)),
                 *options.cmake_argument]
    log.run(configure, cwd=ROOT)
    for path in linked_native_library(directory, options.configuration, library):
        log.retain_record(path)
    reply = directory / '.cmake/api/v1/reply'
    index = bundle.read_json(next(reply.glob('index-*.json')))
    toolchains = reply / bundle.relative(index['reply']['toolchains-v1']['jsonFile'])
    log.retain_record(toolchains)
    observe_compilers(log, directory, options.configuration, toolchains)
    log.run(['cmake', '--build', str(directory), '--config', options.configuration,
             '--parallel', str(options.jobs)], cwd=ROOT)
    log.run(['cmake', '--install', str(directory), '--config', options.configuration,
             '--prefix', str(installed)], cwd=ROOT)
    sources.verify()
    for tool in log.tools.values():
        bundle.require(API['identity'](tool['path']) == {key: tool[key] for key in ('size', 'sha256')},
                       'An actual CMake/compiler tool changed during the product build')
    bundle.require(API['identity'](library) == native_identity, 'The actual linked Rust input changed during the C++ build')
    name = 'huhucapture.exe' if os.name == 'nt' else 'bin/huhucapture'
    binary = installed / name
    meta = bundle.packaging_metadata()
    identity = meta.binary_identity(binary)
    meta.identity(identity)
    meta.verify_binary(binary, bundle.read_json(binary.parent / 'build-identity.json'))
    bundle.require(identity['sourceCommit'] == sources.commit and identity['sourceDirty'] is False and
                   identity['platform'] == sources.platform and identity['configuration'] == options.configuration and
                   identity['dependencyLockSha256'] == bundle.digest(ROOT / 'build/dependencies.lock.json') and
                   identity['cargoLockSha256'] == bundle.digest(ROOT / 'third_party/localsend/Cargo.lock'),
                   'Installed product has a different embedded source/build identity')
    for path in (directory / 'CMakeCache.txt', directory / 'install_manifest.txt',
                 directory / 'cmake_install.cmake', binary.parent / 'build-identity.json'):
        log.retain_record(path)
    for pattern in ('compile_commands.json', 'build.ninja', 'CMakeFiles/rules.ninja', '**/*.vcxproj', '**/*.rsp'):
        for path in sorted(directory.glob(pattern)):
            if path.is_file():
                log.retain_record(path)
    outputs = [{'path': name, **API['identity'](binary)}]
    for filename, original in (('build-identity.json', directory / 'package' / options.configuration / 'build-identity.json'),
                               ('installation.json', directory / 'package/installation.json')):
        copied = binary.parent / filename
        bundle.require(API['identity'](original) == API['identity'](copied),
                       'Installed application metadata differs from its actual CMake-generated file')
        log.retain_record(original)
        outputs.append({'path': copied.relative_to(installed).as_posix(), **API['identity'](copied)})
    raw = log.raw_record({'sources': sources.sources, 'nativeLibrary': {'path': str(library), **native_identity}},
                         {'prefix': str(installed), 'files': outputs})
    log.retain_record(raw)
    rust_source = sources.components['rust-vendor']['sourceTreeSha256']
    receipt = {'schemaVersion': 1, 'kind': 'huhucapture-source-build-receipt', 'platform': sources.platform,
               'commands': log.commands, 'toolchain': log.tools,
               'environment': {'executions': [e['environment'] for e in log.executions]},
               'sources': sources.sources, 'staticInputs': [{'id': 'huhucapture-native', 'component': 'rust-vendor',
                   'sourceTreeSha256': rust_source, **native_identity}], 'outputs': outputs}
    path = evidence / 'source-build-receipt.json'
    API['write_new'](path, receipt)
    plan = {'id': 'application-' + sources.platform, 'path': str(path),
            'buildRecords': [{'path': r['path'], 'sha256': r['sha256']} for r in log.records],
            'staticFiles': {'huhucapture-native': str(library)}}
    API['write_new'](evidence / 'receipt-plan.json', plan)
    print(evidence / 'receipt-plan.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-plan', type=Path, required=True)
    parser.add_argument('--native-receipt', type=Path, required=True)
    parser.add_argument('--build-directory', type=Path, required=True)
    parser.add_argument('--installed', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--configuration', default='RelWithDebInfo')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cmake-argument', action='append', default=[], help='One actual additional configure argument')
    build(parser.parse_args())


if __name__ == '__main__':
    main()
