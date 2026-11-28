#!/usr/bin/env python3
"""Retain real project, Qt, Cargo and Linux runtime source/license evidence.

This collector does not certify licenses or produce a complete source bundle.
Run the system command on the original build runner, immediately after CMake
installation and before the packager strips or rewrites the installed ELF files.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def module(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(result)
    return result


meta = module('huhucapture_package_metadata', ROOT / 'build/package/metadata.py')
sdk = module('huhucapture_development_dependencies', ROOT / 'scripts/bundle-development-dependencies.py')


def command(*arguments):
    environment = os.environ.copy()
    environment['LC_ALL'] = 'C.UTF-8'
    return subprocess.check_output(list(map(str, arguments)), env=environment,
                                   text=True, encoding='utf-8', errors='strict').strip()


def real_directory(path):
    meta.require(path.is_dir() and not path.is_symlink(), 'Expected a real directory: ' + str(path))
    return path.resolve(strict=True)


def new_output(path):
    meta.require(not path.is_symlink(), 'Material output cannot be a directory link')
    path.mkdir(parents=True, exist_ok=True)
    meta.require(not any(path.iterdir()), 'Use an empty material output; earlier evidence is retained')
    return path.resolve(strict=True)


def unchanged_digest(path):
    before = path.stat()
    meta.require(stat.S_ISREG(before.st_mode) and before.st_size > 0,
                 'Expected a nonempty original file: ' + str(path))
    digest = sdk.digest(path)
    after = path.stat()
    meta.require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                 (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
                 'Original bytes changed while collecting: ' + str(path))
    return {'size': before.st_size, 'sha256': digest}


def copy_original(source, destination):
    source = source.resolve(strict=True)
    before = unchanged_digest(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as incoming, destination.open('xb') as outgoing:
        shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
    meta.require(unchanged_digest(source) == before and unchanged_digest(destination) == before,
                 'Original notice changed during retention: ' + str(source))
    return before


def project_notices(output):
    inventory = meta.json_read(ROOT / 'build/package/runtime-notices.json', 256 * 1024)
    meta.require(inventory.get('schemaVersion') == 1 and isinstance(inventory.get('files'), list) and
                 0 < len(inventory['files']) <= 512, 'Invalid project notice inventory')
    seen, records = set(), []
    for name in inventory['files']:
        relative = meta.safe_relative(name)
        meta.require(relative.startswith('third_party/') and relative not in seen,
                     'Duplicate or non-project notice path')
        seen.add(relative)
        source = ROOT / relative
        meta.require(source.is_file() and not source.is_symlink() and source.resolve().is_relative_to(ROOT),
                     'Missing or linked original project notice: ' + relative)
        target = Path('project') / Path(relative).relative_to('third_party')
        record = copy_original(source, output / target)
        records.append({'source': relative, 'path': target.as_posix(), **record})
    copy_original(ROOT / 'build/package/runtime-notices.json', output / 'runtime-notices.json')
    meta.json_write(output / 'project-notices.json', {
        'schemaVersion': 1, 'scope': inventory['scope'], 'correspondingSourceComplete': False,
        'files': records,
    })
    return records


def loader_cache():
    executable = shutil.which('ldconfig')
    if not executable:
        executable = next((str(path) for path in (Path('/usr/sbin/ldconfig'), Path('/sbin/ldconfig'))
                           if path.is_file()), None)
    meta.require(executable, 'The original Linux runner has no ldconfig cache inspector')
    result = {}
    for line in command(executable, '-p').splitlines():
        match = re.fullmatch(r'\s*(\S+)\s+\([^)]*\)\s+=>\s+(.+)', line)
        if match:
            candidate = Path(match[2])
            meta.require(candidate.is_absolute(), 'The host loader returned a relative library')
            result.setdefault(match[1], []).append(candidate)
    meta.require(result, 'The real host loader cache is empty')
    return result


def original_system_library(installed, cache):
    """A name only locates candidates; exact original bytes establish a match."""
    meta.require(meta.machine(installed) == 'linux', 'System source inspection requires a real amd64 ELF')
    expected = unchanged_digest(installed)
    soname = sdk.elf_soname(installed)
    meta.require(soname, 'Unattributed runtime library has no real ELF SONAME: ' + str(installed))
    matches = {}
    for candidate in cache.get(soname, []):
        if not candidate.is_file():
            continue
        original = candidate.resolve(strict=True)
        if original in matches:
            continue
        if unchanged_digest(original) == expected:
            matches[original] = candidate
    meta.require(len(matches) == 1,
                 'Expected one exact original host ELF, not a guessed library name: ' + str(installed))
    original, named = next(iter(matches.items()))
    meta.require(unchanged_digest(installed) == expected and unchanged_digest(original) == expected,
                 'The deployment or host library changed during attribution')
    return {'originalPath': str(original), 'loaderPath': str(named), 'soname': soname, **expected}


def system_origin(installed, cache):
    result = original_system_library(installed, cache)
    package = sdk.owner(Path(result['loaderPath']))
    meta.require(re.fullmatch(r'[a-z0-9][a-z0-9+.:\-]*', package), 'Invalid dpkg owner identifier')
    expected = {key: result[key] for key in ('size', 'sha256')}
    # The current installed file is checked again, not assumed stable after
    # running readelf/dpkg in the original runner's environment.
    meta.require(unchanged_digest(installed) == expected and
                 unchanged_digest(Path(result['originalPath'])) == expected,
                 'The deployment or host library changed during dpkg attribution')
    return {'package': package, **result}


def package_notice(package, output):
    fields = command('dpkg-query', '-W',
        '-f=${binary:Package}\n${Version}\n${Architecture}\n${source:Package}\n${source:Version}', package).splitlines()
    meta.require(len(fields) == 5 and all(fields) and fields[0] == package,
                 'The actual dpkg package lacks exact binary/source identity: ' + package)
    binary, version, architecture, source_package, source_version = fields
    meta.require(architecture == 'amd64', 'A runtime ELF was attributed to a different package architecture')
    copyright_path = Path('/usr/share/doc') / binary.split(':', 1)[0] / 'copyright'
    meta.require(copyright_path.is_file() and copyright_path.resolve().is_relative_to(Path('/usr/share/doc')),
                 'The actual dpkg copyright notice is missing or outside the host documentation tree: ' + package)
    relative = Path('notices/system') / (package.replace(':', '_') + '.copyright')
    retained = copy_original(copyright_path, output / relative)
    return {'binaryPackage': binary, 'version': version, 'architecture': architecture,
            'sourcePackage': source_package, 'sourceVersion': source_version,
            'sourceMaterialStatus': 'not-collected',
            'copyright': {'originalPath': str(copyright_path), 'path': relative.as_posix(), **retained}}


def common_notices(output):
    folder = real_directory(Path('/usr/share/common-licenses'))
    result = []
    for source in sorted(folder.iterdir()):
        meta.require(source.is_file() and source.resolve().is_relative_to(folder),
                     'A real common-license reference is missing or escapes its original directory')
        relative = Path('notices/system/common-licenses') / source.name
        result.append({'originalPath': str(source), 'path': relative.as_posix(),
                       **copy_original(source, output / relative)})
    meta.require(result, 'The actual runner has no common license texts')
    return result


def prefix_arguments(values, installed, output):
    result = {}
    for value in values:
        name, separator, directory = value.partition('=')
        meta.require(separator and name in {'qt', 'portal', 'pipewire', 'media'} and name not in result,
                     'Use unique qt/portal/pipewire/media=ORIGINAL_PREFIX inputs')
        root = real_directory(Path(directory))
        meta.require(not root.is_relative_to(installed) and not installed.is_relative_to(root) and
                     not output.is_relative_to(root), 'Original dependency prefix overlaps the deployment or material output')
        result[name] = root
    meta.require(set(result) == {'qt', 'portal', 'pipewire', 'media'},
                 'All four actual non-system prefixes are required to avoid assigning their bytes to dpkg')
    return result


def prefix_origin(installed, relative, prefixes):
    expected = unchanged_digest(installed)
    matches = []
    for name, prefix in prefixes.items():
        candidate = prefix / relative
        if not candidate.is_file():
            continue
        original = candidate.resolve(strict=True)
        meta.require(original.is_relative_to(prefix), 'Original dependency library escapes its declared prefix')
        if unchanged_digest(original) == expected:
            matches.append({'kind': 'provided-prefix', 'component': name, 'originalPath': str(original),
                            'sourceMaterialStatus': 'not-collected-by-system-command', **expected})
    meta.require(len(matches) <= 1, 'The same installed library has ambiguous non-system input prefixes')
    return matches[0] if matches else None


def collect_system(options):
    meta.require(platform.system() == 'Linux' and platform.machine() in ('x86_64', 'amd64'),
                 'Run Linux runtime attribution on the actual original Linux amd64 build runner')
    installed = real_directory(options.installed)
    output = new_output(options.output)
    meta.require(not output.is_relative_to(installed), 'Keep collected evidence separate until collection succeeds')
    marker = meta.json_read(installed / 'bin/installation.json', 16384)
    build, files = meta.audit_tree(installed, marker.get('packageKind'), development=True)
    meta.require(build['platform'] == 'linux', 'The installation is not the actual Linux product')
    prefixes = prefix_arguments(options.original_prefix, installed, output)
    cache = loader_cache()
    records, packages, deferred, failures = [], {}, [], []
    report = {
        'schemaVersion': 1, 'scope': 'Actual installed Linux ELF inputs and dpkg notices before packaging transforms',
        'buildIdentity': build, 'correspondingSourceComplete': False,
        'systemAttributionComplete': False, 'binaries': records, 'packages': packages,
        'deferredPlugins': deferred, 'failures': failures,
        'outstandingSourceFamilies': ['project', 'qt', 'qt-sdk-icu', 'rust-vendor', 'rust-standard-library',
                                      'portal', 'pipewire', 'media', 'system-source-packages', 'appimage-runtime'],
    }
    try:
        for file in files:
            relative = file['path']
            if not relative.startswith(('lib/', 'plugins/')):
                continue
            path = installed / relative
            with path.open('rb') as incoming:
                if incoming.read(4) != b'\x7fELF':
                    continue
            if relative.startswith('plugins/'):
                # The real Qt deployment adjusts plugin RPATH. Their full SDK
                # SBOM/input binding belongs to the next collection step; a
                # filename or a changed final hash cannot establish it here.
                deferred.append({**file, 'sourceMaterialStatus': 'not-collected-or-bound-to-original'})
                continue
            try:
                meta.require(meta.machine(path) == 'linux', 'Unexpected runtime architecture')
                origin = prefix_origin(path, relative, prefixes)
                if origin is None:
                    origin = {'kind': 'dpkg', **system_origin(path, cache)}
                    package = origin['package']
                    if package not in packages:
                        packages[package] = package_notice(package, output)
                records.append({**file, 'origin': origin})
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                failures.append({'path': relative, 'reason': str(error)})
        meta.require(records, 'No actual ELF runtime libraries were inspected')
        if packages:
            report['commonLicenseFiles'] = common_notices(output)
        meta.require(not failures, 'Some deployed ELF inputs have no exact, attributable original bytes')
        meta.verify_inventory(installed, files)
        report['systemAttributionComplete'] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report['failure'] = str(error)
        raise
    finally:
        meta.json_write(output / 'runtime-materials.json', report)
    print(f'Collected {len(packages)} actual dpkg package notices for {sum(r["origin"]["kind"] == "dpkg" for r in records)} ELF paths.')
    print('Corresponding-source archives, Qt plugin/SBOM binding and Rust materials remain uncollected; no release-complete claim.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    notices = commands.add_parser('notices', help='Retain the exact project notice inventory without altering source')
    notices.add_argument('--output', type=Path, required=True, help='Empty owned output directory')
    system = commands.add_parser('system', help='Attribute original Linux ELF bytes and retain their real dpkg copyright files')
    system.add_argument('--installed', type=Path, required=True, help='Actual unmodified CMake installation prefix')
    system.add_argument('--output', type=Path, required=True, help='Empty separate evidence directory')
    system.add_argument('--original-prefix', action='append', required=True,
                        help='Repeat for qt=PATH, portal=PATH, pipewire=PATH and media=PATH from this build')
    qt = commands.add_parser('qt', help='Bind installed Qt bytes to original SDK SPDX and official deployment replay')
    qt.add_argument('--installed', type=Path, required=True, help='Actual unmodified CMake installation prefix')
    qt.add_argument('--qt-prefix', type=Path, required=True, help='Original Qt SDK used by this build')
    qt.add_argument('--build-directory', type=Path, required=True, help='Actual product CMake build directory')
    qt.add_argument('--configuration', choices=('Release', 'RelWithDebInfo'), required=True)
    qt.add_argument('--output', type=Path, required=True, help='Empty separate runtime-material/evidence directory')
    qt.add_argument('--module-source', action='append', default=[], help='Optional original clean MODULE=SOURCE_CHECKOUT, repeated')
    qt.add_argument('--source-output', type=Path, help='Separate empty directory for full original Qt source archives')
    rust = commands.add_parser('rust', help='Retain the native build receipt and actual locked Cargo vendor tree')
    rust.add_argument('--native-receipt', type=Path, required=True, help='Original same-build native-rust/build-info.json')
    rust.add_argument('--cargo-metadata', type=Path, required=True, help='Original same-build cargo-metadata.json')
    rust.add_argument('--native-library', type=Path, help='Explicit relocated original aggregate; receipt bytes remain unchanged')
    rust.add_argument('--output', type=Path, required=True, help='Empty separate runtime-material/evidence directory')
    rust.add_argument('--source-output', type=Path, required=True, help='Separate empty directory for full Cargo/workspace source')
    options = parser.parse_args()
    try:
        if options.command == 'notices':
            records = project_notices(new_output(options.output))
            print(f'Retained {len(records)} original project notices; no complete dependency-source claim.')
        elif options.command == 'system':
            collect_system(options)
        else:
            helper = module('huhucapture_' + options.command + '_materials',
                            ROOT / 'build/package' / (options.command + '_materials.py'))
            helper.collect(sys.modules[__name__], options)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print('Runtime material collection failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
