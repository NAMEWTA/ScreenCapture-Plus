#!/usr/bin/env python3
"""Retain exact dpkg runtime packages, source archives and Debian build recipes.

Run on the original Linux build runner after runtime attribution and package
preparation. This downloads exact versions through APT without installing them.
For a prepared package, the first continuous patchelf observation identifies
the original bytes of libraries added by linuxdeploy.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import importlib.util


def module(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(result)
    return result


materials = module('huhucapture_system_runtime_materials', ROOT / 'scripts/collect-runtime-materials.py')
receipts = module('huhucapture_system_execution', ROOT / 'build/package/source_receipts.py')
debian = module('huhucapture_system_debian_source', ROOT / 'build/package/debian_sources.py')
ubuntu = module('huhucapture_system_ubuntu_indexes', ROOT / 'build/package/ubuntu_archive_indexes.py')
bundle = receipts.bundle
require = bundle.require
APT_AUTH = ['-o', 'APT::Get::AllowUnauthenticated=false', '-o', 'Acquire::AllowInsecureRepositories=false',
            '-o', 'Acquire::AllowDowngradeToInsecureRepositories=false']


def record_text(path, value):
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(value)


def package_identity(package, execute, directory):
    require(re.fullmatch(r'[a-z0-9][a-z0-9+.\-]*(?::[a-z0-9][a-z0-9\-]*)?', package),
            'Invalid exact installed binary package identifier')
    stdout, _ = execute(['dpkg-query', '-W', '-f=${binary:Package}\n${Version}\n${Architecture}\n'
                         '${source:Package}\n${source:Version}\n${Status}', package], cwd=directory, capture=True)
    fields = stdout.splitlines()
    require(len(fields) == 6 and fields[0] == package and fields[2] == 'amd64' and
            fields[5] == 'install ok installed' and all(fields),
            'The original dpkg runtime package is not still installed with an exact source identity')
    return dict(zip(('binaryPackage', 'version', 'architecture', 'sourcePackage', 'sourceVersion'), fields[:5]))


def selected_binary_index(text, package):
    name = package['binaryPackage'].split(':', 1)[0]
    matches = [item for item in debian.control_stanzas(text) if item.get('package') == name and
               item.get('version') == package['version'] and item.get('architecture') == package['architecture']]
    require(matches, 'APT has no exact installed binary package/version/architecture record')
    selected = matches[0]
    require(debian.SHA256.fullmatch(selected.get('sha256', '')) and selected.get('size', '').isdigit() and
            0 < int(selected['size']) <= debian.MAX_FILE,
            'The exact binary package lacks an original APT SHA256/size pin')
    source = selected.get('source', name)
    match = re.fullmatch(r'([a-z0-9][a-z0-9+.\-]*)(?: \(([^()\s]+)\))?', source)
    require(match and match[1] == package['sourcePackage'] and
            (match[2] or package['version']) == package['sourceVersion'],
            'APT binary/source association differs from the original installed dpkg package')
    keys = ('package', 'version', 'architecture', 'sha256', 'size', 'source')
    require(all(all(item.get(key) == selected.get(key) for key in keys) for item in matches),
            'APT indexes disagree about the installed binary package bytes/source')
    return selected


def match_extracted_binary(root, origin):
    candidates = set()
    original = Path(origin['originalPath']).resolve(strict=True)
    for value in (origin['originalPath'], origin['loaderPath']):
        path = Path(value)
        require(path.is_absolute(), 'The original dpkg library path must be absolute')
        names = [path]
        relative = path.relative_to('/')
        names.append(Path('/') / Path(*relative.parts[1:]) if relative.parts[0] == 'usr' else Path('/usr') / relative)
        for name in names:
            # A merged-/usr alias is considered only when the actual runner
            # resolves it to this same original file, never from a basename.
            if name.is_file() and name.resolve(strict=True) == original:
                target = root / name.relative_to('/')
                if target.is_file() and target.resolve().is_relative_to(root):
                    if materials.unchanged_digest(target) == {key: origin[key] for key in ('size', 'sha256')}:
                        candidates.add(target.resolve(strict=True))
    require(len(candidates) == 1, 'Original deployed ELF does not match exactly one file in its real .deb')
    return next(iter(candidates)).relative_to(root).as_posix()


def added_system_libraries(stage, build):
    package = module('huhucapture_distro_package', ROOT / 'scripts/package-qt.py')
    prepared = package.verify_prepared(stage)
    require({key: value for key, value in prepared['build'].items() if key != 'packagingTools'} == build,
            'The prepared package belongs to a different original runtime')
    journal = bundle.read_json(stage / 'transformations.json')
    execution = bundle.read_json(stage / 'executions/build-execution.json')
    lineage = package.lineage_api().validate(stage, journal, execution, package.source_inventory_api())
    cache = materials.loader_cache()
    records, unmatched = [], []
    for name, chain in lineage['observedOrigins'].items():
        if Path(name).parent.as_posix() != 'appimage/usr/lib':
            continue
        path = stage / 'payloads' / bundle.relative(name)
        soname = materials.sdk.elf_soname(path)
        if not soname:
            continue
        expected = {key: chain['input'][key] for key in ('size', 'sha256')}
        matches = {}
        for candidate in cache.get(soname, []):
            if candidate.is_file() and materials.unchanged_digest(candidate) == expected:
                matches.setdefault(candidate.resolve(strict=True), candidate)
        if len(matches) != 1:
            unmatched.append(name)
            continue
        original, loader = next(iter(matches.items()))
        try:
            owner = materials.sdk.owner(loader)
        except RuntimeError:
            unmatched.append(name)
            continue
        records.append({'path': name, 'root': 'prepared', 'origin': {'kind': 'dpkg', 'package': owner,
                       'originalPath': str(original), 'loaderPath': str(loader), 'soname': soname, **expected}})
    return records, unmatched


def collect(options):
    require(platform.system() == 'Linux' and platform.machine() in ('x86_64', 'amd64'),
            'Collect Debian runtime source material on the original Linux amd64 runner')
    installed = materials.real_directory(options.installed)
    original_path = options.runtime_materials.resolve(strict=True)
    original = bundle.read_json(original_path)
    require(original.get('schemaVersion') == 1 and original.get('systemAttributionComplete') is True and
            not original.get('failures'), 'A successful original runtime/dpkg attribution report is required')
    marker = materials.meta.json_read(installed / 'bin/installation.json')
    build, installed_files = materials.meta.audit_tree(installed, marker.get('packageKind'), development=True)
    require(build == original.get('buildIdentity') and build['platform'] == 'linux' and
            build['sourceCommit'] == bundle.git(ROOT, 'rev-parse', 'HEAD').decode().strip(),
            'Runtime attribution belongs to a different actual project build')
    output = options.output.resolve()
    require(not output.is_relative_to(installed) and not installed.is_relative_to(output) and
            not original_path.is_relative_to(output), 'Source output overlaps original runtime inputs')
    output = materials.new_output(output)
    execution = receipts.ExecutionLog(output / 'executions', 'linux', Path(__file__))
    execution.retain_record(original_path)
    execution.retain_record(ROOT / 'build/package/debian_sources.py')
    execution.retain_record(ROOT / 'build/package/ubuntu_archive_indexes.py')
    observed = {original_path: materials.unchanged_digest(original_path)}
    report = {'schemaVersion': 1, 'kind': 'huhucapture-debian-source-materials', 'buildIdentity': build,
              'status': 'collecting', 'binaryPackages': [], 'sourcePackages': [],
              'gaps': [],
              'scope': 'Exact original dpkg/.deb bytes, source archives, Debian patches and recipes; no upstream build receipt'}
    plan = {'schemaVersion': 1, 'platform': 'linux', 'project': str(ROOT), 'sources': [], 'materials': []}
    def material(identifier, path):
        plan['materials'].append({'id': identifier, 'path': str(path)})
    material('debian-original-runtime-attribution', original_path)
    environment = os.environ.copy()
    environment['LC_ALL'] = 'C.UTF-8'
    environment.pop('APT_CONFIG', None)
    def execute(arguments, *, cwd, capture=False, env=None):
        return execution.run(arguments, cwd=cwd, env=env if env is not None else environment, capture=capture)
    try:
        records = [item for item in original.get('binaries', []) if item.get('origin', {}).get('kind') == 'dpkg']
        packages = dict(original.get('packages', {}))
        require(records and set(packages) == {item['origin']['package'] for item in records},
                'Original runtime report has no exact matching set of attributed dpkg libraries/packages')
        notices = [(name.replace(':', '_'), item['copyright']) for name, item in sorted(packages.items())]
        notices.extend(('common-' + str(index), item) for index, item in enumerate(original.get('commonLicenseFiles', [])))
        require(original.get('commonLicenseFiles'), 'Original dpkg common-license texts were not retained')
        for name, notice in notices:
            retained = original_path.parent / bundle.relative(notice['path'])
            expected = {key: notice[key] for key in ('size', 'sha256')}
            require(materials.unchanged_digest(retained) == expected,
                    'The retained original dpkg copyright/common-license text changed')
            observed[retained] = expected
            material('debian-notice-' + name, retained)
        for item in records:
            path = installed / bundle.relative(item['path'])
            expected = {key: item['origin'][key] for key in ('size', 'sha256')}
            for original_file in (path, Path(item['origin']['originalPath'])):
                require(materials.unchanged_digest(original_file) == expected,
                        'Original installed/host ELF changed after runtime attribution')
                observed[original_file] = expected
            require(materials.sdk.owner(Path(item['origin']['loaderPath'])) == item['origin']['package'],
                    'The current dpkg owner differs from the original runtime attribution')
        if options.prepared:
            stage = options.prepared.resolve(strict=True)
            require(not stage.is_relative_to(output) and not output.is_relative_to(stage),
                    'The source collection and prepared package must use separate directories')
            deployed, unmatched = added_system_libraries(stage, build)
            records.extend(deployed)
            report['unmatchedDeploymentLibraries'] = unmatched
            report['gaps'].extend('No unique original host ELF matches the observed input: ' + name for name in unmatched)
            for item in deployed:
                origin = item['origin']
                observed[Path(origin['originalPath'])] = {key: origin[key] for key in ('size', 'sha256')}
                name = origin['package']
                if name not in packages:
                    notice = packages[name] = materials.package_notice(name, output)
                    retained = output / bundle.relative(notice['copyright']['path'])
                    material('debian-notice-' + name.replace(':', '_'), retained)
                    observed[retained] = {key: notice['copyright'][key] for key in ('size', 'sha256')}
        families, binary_pins, source_pins = {}, {}, {}
        for package_name, previous in sorted(packages.items()):
            package = package_identity(package_name, execute, output)
            require(all(previous.get(key) == value for key, value in package.items()),
                    'Installed binary/source package versions changed after runtime attribution')
            family = package['sourcePackage']
            require(family not in families or families[family] == package['sourceVersion'],
                    'One deployment contains conflicting versions of the same source package')
            families[family] = package['sourceVersion']
            directory = output / 'binaries' / package_name.replace(':', '_'); directory.mkdir(parents=True)
            stdout, _ = execute(['apt-cache', 'show', package_name + '=' + package['version']], cwd=directory, capture=True)
            index = directory / 'Packages-records.txt'; record_text(index, stdout)
            indexed = selected_binary_index(stdout, package)
            downloads = directory / 'download'; downloads.mkdir()
            execute(['apt-get', *APT_AUTH, 'download', package_name + '=' + package['version']], cwd=downloads)
            debs = list(downloads.iterdir())
            require(len(debs) == 1 and debs[0].suffix == '.deb' and
                    debian.identity(debs[0]) == {'size': int(indexed['size']), 'sha256': indexed['sha256']},
                    'Downloaded binary package differs from its exact APT index pin')
            bindings = []
            with tempfile.TemporaryDirectory(prefix='actual-deb-', dir=output) as temporary:
                extracted = Path(temporary)
                execute(['dpkg-deb', '--extract', debs[0], extracted], cwd=directory)
                for item in records:
                    if item['origin']['package'] == package_name:
                        name = match_extracted_binary(extracted, item['origin'])
                        bindings.append({'installedPath': item['path'], 'root': item.get('root', 'installed'), 'packagePath': name,
                                         **{key: item['origin'][key] for key in ('size', 'sha256')}})
            report['binaryPackages'].append({**package, 'archive': {'path': str(debs[0]), **debian.identity(debs[0])},
                                             'originalIndex': {'path': str(index), **debian.identity(index)}, 'files': bindings,
                                             'copyright': previous['copyright']})
            binary_pins[(package_name.split(':', 1)[0], package['version'])] = debian.identity(debs[0])
            material('debian-binary-' + package_name.replace(':', '_'), debs[0])
            material('debian-binary-index-' + package_name.replace(':', '_'), index)
        for family, version in sorted(families.items()):
            identifier = 'openssl' if family == 'openssl' else 'system-' + family
            require(bundle.IDENTIFIER.fullmatch(identifier), 'Debian source component name exceeds the retained format')
            directory = output / 'sources' / identifier; directory.mkdir(parents=True)
            stdout, _ = execute(['apt-cache', '--only-source', 'showsrc', family], cwd=directory, capture=True)
            index = directory / 'Sources-records.txt'; record_text(index, stdout)
            _, indexed = debian.selected_index(stdout, family, version)
            source_pins[(family, version)] = indexed
            downloads = directory / 'download'; downloads.mkdir()
            execute(['apt-get', *APT_AUTH, '--download-only', '--only-source', '--yes',
                     'source', family + '=' + version], cwd=downloads)
            require({path.name for path in downloads.iterdir()} == set(indexed),
                    'APT source download omitted or added files outside its exact source index')
            dsc_name = next(name for name in indexed if name.endswith('.dsc'))
            specification = {'id': identifier, 'kind': 'debian-source', 'sourcePackage': family, 'sourceVersion': version,
                             'dsc': str(downloads / dsc_name), 'sha256': indexed[dsc_name]['sha256'],
                             'sourceIndex': str(index), 'sourceIndexSha256': debian.identity(index)['sha256']}
            provenance, files, inputs = debian.extract_source(specification, directory / 'extracted', bundle.inventory, execute=execute)
            source_record = {'id': identifier, 'source': provenance, 'sourceTreeSha256': bundle.tree_digest(files), 'files': files}
            record_path = directory / 'source-inputs.json'; receipts.write_new(record_path, source_record)
            material('debian-source-record-' + identifier, record_path)
            plan['sources'].append(specification)
            report['sourcePackages'].append({key: source_record[key] for key in ('id', 'source', 'sourceTreeSha256')})
            report['gaps'].append(identifier + ': original distribution buildinfo/configure/link evidence and matching build receipt remain uncollected')
            observed.update(inputs)
        if platform.freedesktop_os_release().get('ID') == 'ubuntu':
            indexes = ubuntu.collect(output / 'archive-indexes', execute, debian, binary_pins, source_pins, material)
            report['authenticatedIndexes'] = indexes
            for category, pins in (('binaries', binary_pins), ('sources', source_pins)):
                for key in pins:
                    if '='.join(key) not in indexes[category]:
                        report['gaps'].append('The signed Ubuntu archive did not bind the exact ' + category + ' input: ' + '='.join(key))
        else:
            report['gaps'].append('debian-repository: signed indexes were not collected for this distribution')
        require(all(materials.unchanged_digest(path) == expected for path, expected in observed.items()),
                'Original runtime or source package bytes changed during collection')
        materials.meta.verify_inventory(installed, installed_files)
        report['status'] = 'original-packages-and-sources-retained'
        report_path = output / 'system-sources.json'; receipts.write_new(report_path, report)
        material('debian-system-source-report', report_path)
        execution.raw_record({'runtimeReport': observed[original_path], 'buildIdentity': build},
                             {'binaryPackages': report['binaryPackages'], 'sourcePackages': report['sourcePackages'],
                              'authenticatedIndexes': report.get('authenticatedIndexes', {})})
        archive = output / 'source-collection-execution.tar.gz'
        bundle.make_archive(execution.directory, archive, 0)
        material('debian-system-source-execution', archive)
        receipts.write_new(output / 'source-plan.json', plan)
        return report
    except Exception as error:
        report.update(status='failed', failure=str(error))
        if not (output / 'system-sources.json').exists():
            receipts.write_new(output / 'system-sources.json', report)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installed', type=Path, required=True)
    parser.add_argument('--runtime-materials', type=Path, required=True,
                        help='Original successful collect-runtime-materials system report from this runner')
    parser.add_argument('--prepared', type=Path, help='Frozen package preparation from this same Linux build')
    parser.add_argument('--output', type=Path, required=True, help='New separate material directory')
    args = parser.parse_args()
    try:
        report = collect(args)
        print(json.dumps({key: report[key] for key in ('status', 'gaps')}, indent=2))
        return 0
    except (ValueError, KeyError, OSError, RuntimeError, bundle.subprocess.SubprocessError) as error:
        print('System source collection failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
