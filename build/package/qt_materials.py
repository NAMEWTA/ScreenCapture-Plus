"""Bind this product's actual Qt deployment to the SDK's original SPDX files.

The public Qt deployment function performs all binary transformations. This
module never normalizes ELF/PE bytes or treats a matching filename as evidence.
"""
from __future__ import annotations

import gzip
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def digest_file(path, algorithm):
    digest = hashlib.new(algorithm)
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            digest.update(data)
    return digest.hexdigest()


def sdk_spdx(api, prefix, version):
    """Index original files without discarding Qt's multi-config SPDX IDs.

    QtPublicSbomFileHelpers adds a distinct ID for each installed build
    configuration even when its custom file is identical (for example a QM).
    Only same-document records with every non-ID field and owning package in
    agreement may share one path. The original document and all IDs remain.
    """
    documents, by_file = {}, {}
    for path in sorted((prefix / 'sbom').glob('*.spdx.json')):
        value = api.meta.json_read(path, 32 * 1024 * 1024)
        api.meta.require(value.get('spdxVersion') == 'SPDX-2.3', 'Unsupported original Qt SPDX schema')
        packages = {entry['SPDXID']: entry for entry in value.get('packages', [])}
        described = {entry['relatedSpdxElement'] for entry in value.get('relationships', [])
                     if entry.get('spdxElementId') == 'SPDXRef-DOCUMENT' and
                     entry.get('relationshipType') == 'DESCRIBES'}
        roots = [packages[key] for key in described if key in packages]
        api.meta.require(len(roots) == 1 and roots[0]['name'] in
                         {'qtbase', 'qtsvg', 'qtwayland', 'qttranslations', 'qttools'},
                         'An original Qt SBOM has no unambiguous supported module')
        name = roots[0]['name']
        api.meta.require(path.name == f'{name}-{version}.spdx.json' and name not in documents,
                         'Qt SPDX module/version differs from the selected SDK')
        containers = {}
        for relation in value.get('relationships', []):
            if relation.get('relationshipType') == 'CONTAINS' and relation['spdxElementId'] in packages:
                containers.setdefault(relation['relatedSpdxElement'], set()).add(relation['spdxElementId'])
        document = {'name': name, 'path': path, 'value': value, 'package': roots[0], 'packages': packages}
        documents[name] = document
        seen_ids = set(packages)
        for entry in value.get('files', []):
            spdx_id = entry['SPDXID']
            api.meta.require(isinstance(spdx_id, str) and spdx_id.startswith('SPDXRef-') and
                             spdx_id not in seen_ids,
                             'Repeated or invalid original Qt SPDX ID in ' + path.name)
            seen_ids.add(spdx_id)
            relative = api.meta.safe_relative(entry['fileName'].removeprefix('./'))
            checksums = [item['checksumValue'].lower() for item in entry.get('checksums', [])
                         if item.get('algorithm') == 'SHA1']
            api.meta.require(len(checksums) == 1 and re.fullmatch(r'[0-9a-f]{40}', checksums[0]),
                             'An original Qt file lacks one exact SPDX checksum')
            record = {'module': name, 'document': path.name,
                      'sha1': checksums[0], 'licenseConcluded': entry.get('licenseConcluded', 'NOASSERTION'),
                      'copyrightText': entry.get('copyrightText', 'NOASSERTION'),
                      'packageIds': sorted(containers.get(spdx_id, set())),
                      'fileProperties': {key: item for key, item in entry.items() if key != 'SPDXID'}}
            previous = by_file.get(relative)
            if previous is None:
                by_file[relative] = {**record, 'spdxIds': [spdx_id]}
            else:
                api.meta.require(record == {key: item for key, item in previous.items() if key != 'spdxIds'},
                                 'Conflicting original Qt SPDX records for file: ' + relative)
                previous['spdxIds'].append(spdx_id)
                previous['spdxIds'].sort()
    api.meta.require({'qtbase', 'qtsvg', 'qttranslations'} <= documents.keys(),
                     'The actual SDK lacks the required original Qt SPDX documents')
    return documents, by_file


def original_sdk_file(api, prefix, relative, index):
    candidate = prefix / relative
    api.meta.require(candidate.is_file(), 'The deployed Qt file has no original SDK input: ' + relative)
    original = candidate.resolve(strict=True)
    api.meta.require(original.is_relative_to(prefix), 'An SDK input escapes its declared prefix')
    canonical = original.relative_to(prefix).as_posix()
    api.meta.require(canonical in index, 'No original Qt SPDX file record: ' + canonical)
    expected = api.unchanged_digest(original)
    actual_sha1 = digest_file(original, 'sha1')
    api.meta.require(actual_sha1 == index[canonical]['sha1'] and api.unchanged_digest(original) == expected,
                     'Original Qt SDK bytes differ from its official SPDX checksum: ' + canonical)
    return {'path': canonical, **expected, **index[canonical]}


def cmake_literal(api, value):
    api.meta.require(isinstance(value, str) and '\0' not in value and ']===]' not in value,
                     'Invalid literal deployment argument')
    return '[===[' + value + ']===]'


def deployment_input(api, build_directory, prefix, platform_name, configuration):
    """Accept only the generated call for this product; never run its includes.

    Generated support contains actual SDK/build variables. Only literal values
    are retained, paths are checked, and the only executed include is the SDK's
    own Qt6CoreDeploySupport.cmake. In particular, arbitrary deployment hooks,
    build targets or optional plugin scripts cannot write outside our prefix.
    """
    folder = api.real_directory(build_directory / '.qt')
    candidates = sorted(folder.glob('deploy_huhucapture_*.cmake'))
    if len(candidates) > 1:
        candidates = [path for path in candidates if path.stem.endswith('-' + configuration)]
    api.meta.require(len(candidates) == 1, 'Expected one actual product deployment script for the configuration')
    generated = candidates[0]
    body = generated.read_text(encoding='utf-8')
    header = re.match(r'\Ainclude\("([^"\n]+/QtDeploySupport(?:-[^"/]+)?\.cmake)"\)\n'
                      r'include\("\$\{CMAKE_CURRENT_LIST_DIR\}/(huhucapture-plugins(?:-[A-Za-z]+)?\.cmake)" OPTIONAL\)\n'
                      r'set\(__QT_DEPLOY_I18N_CATALOGS "qtbase"\)\n', body)
    api.meta.require(header is not None, 'Unexpected generated Qt deployment preamble')
    support = Path(header[1]).resolve(strict=True)
    multi_config = generated.stem.endswith('-' + configuration)
    plugin_script = 'huhucapture-plugins' + ('-' + configuration if multi_config else '') + '.cmake'
    api.meta.require(support.parent == folder and header[2] == plugin_script and not (folder / plugin_script).exists(),
                     'Additional product deployment scripts need explicit source/destination review')
    bin_directory = '.' if platform_name == 'windows' else 'bin'
    plugins = 'qwindows qschannelbackend' if platform_name == 'windows' else 'qxcb qwayland qopensslbackend'
    remainder = body[header.end():]
    executable_match = re.search(r'^    EXECUTABLE "([^"$;\n]+)"$', remainder, re.MULTILINE)
    api.meta.require(executable_match is not None, 'Expected one literal source executable')
    executable = Path(executable_match[1]).resolve(strict=True)
    api.meta.require(executable.is_relative_to(build_directory) and executable.is_file(),
                     'Qt replay executable must belong to the actual build directory')
    expected = f'''set(QT_DEPLOY_BIN_DIR "{bin_directory}")
set(QT_DEPLOY_LIB_DIR "lib")
set(QT_DEPLOY_PLUGINS_DIR "plugins")
set(QT_DEPLOY_TRANSLATIONS_DIR "translations")
qt_deploy_runtime_dependencies(
    EXECUTABLE "{executable_match[1]}"
    BIN_DIR "{bin_directory}"
    LIB_DIR "lib"
    PLUGINS_DIR "plugins"
    GENERATE_QT_CONF
    NO_COMPILER_RUNTIME
    NO_TRANSLATIONS
    INCLUDE_PLUGIN_TYPES platforms imageformats tls
    INCLUDE_PLUGINS {plugins}
    PRE_EXCLUDE_REGEXES "^ld-linux.*" "^libc[.]so.*" "^libm[.]so.*" "^libpthread[.]so.*" "^libdl[.]so.*" "^librt[.]so.*" "^libGL.*" "^libEGL.*" "^libvulkan.*" "^libdrm.*"
)
'''
    api.meta.require(remainder == expected, 'The product deployment call changed; review its destinations before replay')
    text = support.read_text(encoding='utf-8')
    values = dict(re.findall(r'^set\((__QT_[A-Za-z0-9_]+) "([^"$\\\r\n]*)"\)$', text, re.MULTILINE))
    api.meta.require(values.get('__QT_DEPLOY_SYSTEM_NAME') == ('Windows' if platform_name == 'windows' else 'Linux') and
                     values.get('__QT_DEFAULT_MAJOR_VERSION') == '6' and
                     values.get('__QT_DEPLOY_IS_SHARED_LIBS_BUILD') == 'ON' and
                     values.get('__QT_DEPLOY_ACTIVE_CONFIG') == configuration,
                     'The original Qt deployment platform/configuration differs')
    api.meta.require(Path(values.get('__QT_DEPLOY_QT_INSTALL_PREFIX', '')).resolve() == prefix,
                     'The generated deployment uses a different original Qt SDK')
    for key, wanted in {'BINS': 'bin', 'DATA': '.', 'DESCRIPTIONSDIR': 'modules',
                        'PLUGINS': 'plugins', 'TRANSLATIONS': 'translations'}.items():
        api.meta.require(values.get('__QT_DEPLOY_QT_INSTALL_' + key) == wanted,
                         'Unexpected original Qt SDK directory: ' + key)
    api.meta.require(not values.get('__QT_DEPLOY_QT_ADDITIONAL_PACKAGES_PREFIX_PATH') and
                     not values.get('__QT_DEPLOY_USE_PATCHELF'),
                     'Extra SDK prefixes or a different RPATH tool require separate replay review')
    inputs = [generated, support, executable, prefix / 'lib/cmake/Qt6Core/Qt6CoreDeploySupport.cmake']
    tool = values.get('__QT_DEPLOY_TOOL', '')
    if platform_name == 'linux':
        api.meta.require(tool == 'GRD', 'Expected the actual Qt generic Linux deployment tool')
    else:
        tool_path = Path(tool).resolve(strict=True)
        api.meta.require(tool_path.is_relative_to(prefix / 'bin') and tool_path.name.lower() == 'windeployqt.exe',
                         'Expected the SDK-owned Windows deployment executable')
        inputs.append(tool_path)
    qtpaths = Path(values.get('__QT_DEPLOY_TARGET_QT_PATHS_PATH', '')).resolve(strict=True)
    api.meta.require(qtpaths.is_relative_to(prefix / 'bin') and qtpaths.is_file(), 'Missing original SDK qtpaths')
    inputs.append(qtpaths)
    ignored = re.findall(r'^    set\(QT_DEPLOY_IGNORED_LIB_DIRS "([^"$\\\r\n]*)"\)$', text, re.MULTILINE)
    api.meta.require(len(ignored) <= 1, 'Ambiguous original system library search exclusions')
    # Copy only known data keys. No generated include/loop/hook is executed.
    values.pop('__QT_DEPLOY_IMPL_DIR', None)
    values['QT_DEPLOY_IGNORED_LIB_DIRS'] = ignored[0] if ignored else ''
    values['QT_DEPLOY_LIBEXEC_DIR'] = 'libexec'
    values['QT_DEPLOY_QML_DIR'] = 'qml'
    return {'script': generated, 'support': support, 'executable': executable,
            'body': remainder, 'values': values, 'inputs': inputs}


def replay_deployment(api, plan, prefix, installed, build_identity, output, callback):
    """Replay into a fresh, collector-owned tree and discard only that tree."""
    api.meta.require(api.meta.binary_identity(plan['executable']) == build_identity,
                     'Deployment replay input is not the installed product build')
    receipts = {str(path): api.unchanged_digest(path) for path in plan['inputs']}
    evidence = output / 'evidence'; evidence.mkdir()
    for path in (plan['script'], plan['support']):
        api.copy_original(path, evidence / path.name)
    cmake = shutil.which('cmake')
    api.meta.require(cmake, 'The original CMake deployment tool is unavailable')
    environment = os.environ.copy(); environment.pop('DESTDIR', None)
    environment['LC_ALL'] = 'C.UTF-8'
    with tempfile.TemporaryDirectory(prefix='qt-replay-', dir=output) as directory:
        owned = Path(directory).resolve()
        replay = owned / 'prefix'; replay.mkdir()
        api.meta.require(not replay.is_relative_to(installed) and not replay.is_relative_to(prefix),
                         'Replay destination overlaps an original input')
        literal = lambda value: cmake_literal(api, value)
        lines = ['cmake_minimum_required(VERSION 3.25)',
                 'set(CMAKE_INSTALL_PREFIX ' + literal(replay.as_posix()) + ')',
                 'set(QT_DEPLOY_PREFIX ' + literal(replay.as_posix()) + ')']
        lines.extend('set(' + key + ' ' + literal(value) + ')' for key, value in plan['values'].items())
        lines.append('include(' + literal((prefix / 'lib/cmake/Qt6Core/Qt6CoreDeploySupport.cmake').as_posix()) + ')')
        lines.append(plan['body'])
        script = owned / 'replay.cmake'; script.write_text('\n'.join(lines), encoding='utf-8')
        api.copy_original(script, evidence / 'replay.cmake')
        command = [cmake, '-P', str(script)]
        with (evidence / 'deployment.log').open('xb') as log:
            result = subprocess.run(command, cwd=owned, env=environment, stdout=log, stderr=subprocess.STDOUT)
        api.meta.require(result.returncode == 0, 'Official Qt deployment replay failed; see evidence/deployment.log')
        # The inventory also rejects symlinks escaping the owned output.
        replay_files = api.meta.inventory(replay)
        callback(replay, replay_files)
    for path, expected in receipts.items():
        api.meta.require(api.unchanged_digest(Path(path)) == expected,
                         'An original build/SDK input changed during deployment replay')
    return {'tool': str(Path(cmake).resolve()), 'version': api.command(cmake, '--version').splitlines()[0],
            'inputs': [{'path': path, **value} for path, value in receipts.items()],
            'scope': 'Original public Qt deployment call with audited literal inputs; output prefix owned by collector'}


def retain_module_source(api, name, checkout, document, source_output, output, version):
    """Archive a clean real Git checkout; preserve unresolved source bindings."""
    checkout = api.real_directory(checkout)
    api.meta.require(not source_output.is_relative_to(checkout) and not output.is_relative_to(checkout),
                     'Material outputs cannot change the original Qt source checkout')
    api.meta.require(api.command('git', '-C', checkout, 'status', '--porcelain', '--untracked-files=normal') == '',
                     'Qt source checkout must be clean: ' + name)
    commit = api.command('git', '-C', checkout, 'rev-parse', 'HEAD')
    api.meta.require(re.fullmatch(r'[0-9a-f]{40}', commit), 'Qt source has no exact Git commit')
    tree = api.command('git', '-C', checkout, 'ls-tree', '-r', 'HEAD')
    api.meta.require(not re.search(r'^160000 ', tree, re.MULTILINE),
                     'Qt source has uncollected Git submodules: ' + name)
    location = document['package'].get('downloadLocation', '')
    expected = re.fullmatch(r'git://code\.qt\.io/qt/' + re.escape(name) + r'\.git@([0-9a-f]{40})', location)
    binding = 'version-tag-only-not-bound-to-sdk'
    if expected:
        api.meta.require(commit == expected[1], 'Qt source commit differs from original SDK SPDX: ' + name)
        binding = 'original-sdk-spdx-commit'
    else:
        api.meta.require(location == 'git://code.qt.io/qt/' + name + '.git', 'Unexpected Qt source repository')
        source_spdx = document['path'].with_name(f'{name}-{version}.source.spdx')
        if source_spdx.is_file():
            source_text = source_spdx.read_text(encoding='utf-8')
            tag_record = re.search(r'^FileName: (?:\./)?\.tag\n(?:(?!\n\n).)*?'
                                   r'^FileChecksum: SHA1: ([0-9a-f]{40})$', source_text, re.MULTILINE | re.DOTALL)
            if tag_record:
                api.meta.require(hashlib.sha1((commit + '\n').encode('ascii')).hexdigest() == tag_record[1],
                                 'Qt source commit differs from original source SPDX .tag: ' + name)
                binding = 'original-sdk-source-spdx-tag'
        if binding == 'version-tag-only-not-bound-to-sdk':
            tags = api.command('git', '-C', checkout, 'tag', '--points-at', commit).splitlines()
            api.meta.require('v' + version in tags, 'Unbound Qt source checkout lacks the selected release tag')
    destination = source_output / (name + '-' + commit + '.tar.gz')
    process = subprocess.Popen(['git', '-C', str(checkout), 'archive', '--format=tar',
                                '--prefix=' + name + '/', commit], stdout=subprocess.PIPE)
    try:
        with destination.open('xb') as target, gzip.GzipFile(filename='', mode='wb', fileobj=target, mtime=0) as archive:
            shutil.copyfileobj(process.stdout, archive, 1024 * 1024)
        process.stdout.close()
        api.meta.require(process.wait() == 0, 'Git could not archive the actual Qt source')
    finally:
        if process.poll() is None:
            process.terminate(); process.wait()
    notices = []
    candidates = sorted({path for path in checkout.iterdir() if path.is_file() and
                         path.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE'))} |
                        {path for path in (checkout / 'LICENSES').rglob('*') if path.is_file()})
    for path in candidates:
        api.meta.require(not path.is_symlink(), 'Qt license input cannot point outside its source snapshot')
        relative = path.relative_to(checkout)
        retained = Path('notices/qt/modules') / name / relative
        notices.append({'source': relative.as_posix(), 'path': retained.as_posix(),
                        **api.copy_original(path, output / retained)})
    api.meta.require(notices, 'The actual Qt source checkout has no retained license texts')
    api.meta.require(api.command('git', '-C', checkout, 'rev-parse', 'HEAD') == commit and
                     api.command('git', '-C', checkout, 'status', '--porcelain', '--untracked-files=normal') == '',
                     'Qt source checkout changed during collection')
    return {'module': name, 'repository': 'https://code.qt.io/qt/' + name + '.git', 'commit': commit,
            'sourceBinding': binding, 'archive': {'path': destination.name, **api.unchanged_digest(destination)},
            'notices': notices}


def collect(api, options):
    installed, prefix = api.real_directory(options.installed), api.real_directory(options.qt_prefix)
    build_directory = api.real_directory(options.build_directory)
    output = api.new_output(options.output)
    api.meta.require(not output.is_relative_to(installed) and not output.is_relative_to(prefix) and
                     not installed.is_relative_to(prefix), 'Qt material output/original prefixes overlap')
    marker_path = installed / 'installation.json'
    if not marker_path.is_file():
        marker_path = installed / 'bin/installation.json'
    marker = api.meta.json_read(marker_path, 16384)
    build, files = api.meta.audit_tree(installed, marker.get('packageKind'), development=True)
    version = api.meta.json_read(api.ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
    api.meta.require(build.get('qtVersion') == version, 'Actual installed Qt version differs from the dependency lock')
    records, failures, sources = [], [], []
    report = {'schemaVersion': 2, 'scope': 'Original Qt SPDX files and actual installed Qt bytes before packaging transforms',
              'buildIdentity': build, 'qtVersion': version, 'binaryBindingComplete': False,
              'correspondingSourceComplete': False, 'files': records, 'sourceModules': sources, 'failures': failures,
              'outstandingSourceFamilies': ['qt-sdk-icu', 'exact-sdk-configuration-and-source-closure']}
    try:
        documents, index = sdk_spdx(api, prefix, version)
        sbom = []
        for path in sorted((prefix / 'sbom').iterdir()):
            api.meta.require(path.is_file() and not path.is_symlink(), 'Unexpected linked SDK SBOM input')
            relative = Path('notices/qt/sbom') / path.name
            sbom.append({'path': relative.as_posix(), **api.copy_original(path, output / relative)})
        report['originalSbomFiles'] = sbom
        planned = []
        for file in files:
            relative = file['path']
            if relative.startswith(('plugins/', 'translations/')):
                original_relative = relative
            elif build['platform'] == 'linux' and relative.startswith('lib/libQt6'):
                original_relative = relative
            elif build['platform'] == 'windows' and re.fullmatch(r'Qt6[^/]+\.dll', relative):
                original_relative = 'bin/' + relative
            else:
                continue
            original = original_sdk_file(api, prefix, original_relative, index)
            planned.append((file, original))
        api.meta.require(planned, 'The real installation contains no attributable Qt files')
        plan = deployment_input(api, build_directory, prefix, build['platform'], options.configuration)
        def bind(replay, replay_files):
            available = {item['path']: item for item in replay_files}
            for deployed, original in planned:
                same = all(deployed[key] == original[key] for key in ('size', 'sha256'))
                replayed = not deployed['path'].startswith('translations/')
                if replayed:
                    actual = available.get(deployed['path'])
                    api.meta.require(actual is not None and all(actual[key] == deployed[key] for key in ('size', 'sha256')),
                                     'Deployed Qt bytes do not match the official replay: ' + deployed['path'])
                else:
                    api.meta.require(same, 'Compiled Qt translation differs from original SDK SPDX bytes')
                records.append({**deployed, 'binding': 'original-full-bytes' if same else 'official-deployment-full-bytes',
                                'officialDeploymentVerified': replayed, 'originalSdk': original})
        report['deploymentReplay'] = replay_deployment(api, plan, prefix, installed, build, output, bind)
        for _, original in planned:
            api.meta.require(api.unchanged_digest(prefix / original['path']) ==
                             {key: original[key] for key in ('size', 'sha256')}, 'An original Qt SDK file changed')
        source_inputs = {}
        for value in options.module_source:
            name, separator, directory = value.partition('=')
            api.meta.require(separator and name in documents and name not in source_inputs,
                             'Use unique original SPDX module=SOURCE_CHECKOUT inputs')
            source_inputs[name] = Path(directory)
        if source_inputs:
            api.meta.require(options.source_output is not None, 'Full Qt source archives need a separate --source-output')
            source_output = api.new_output(options.source_output)
            api.meta.require(not source_output.is_relative_to(installed) and not source_output.is_relative_to(output) and
                             not source_output.is_relative_to(prefix), 'Full Qt sources must be separate from runtime materials')
            for name, checkout in sorted(source_inputs.items()):
                sources.append(retain_module_source(api, name, checkout, documents[name], source_output, output, version))
            api.meta.json_write(source_output / 'qt-sources.json', {'schemaVersion': 1, 'qtVersion': version,
                                'correspondingSourceComplete': False, 'modules': sources})
        report['sourceModulesNotCollected'] = sorted(documents.keys() - source_inputs.keys())
        api.meta.verify_inventory(installed, files)
        report['binaryBindingComplete'] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        failures.append(str(error))
        raise
    finally:
        api.meta.json_write(output / 'qt-materials.json', report)
    print(f'Bound {len(records)} actual deployed Qt file paths to original SPDX and complete-byte evidence.')
    print('This Qt-family result does not certify corresponding-source closure or combined release licensing.')
