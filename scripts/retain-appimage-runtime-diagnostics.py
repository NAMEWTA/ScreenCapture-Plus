#!/usr/bin/env python3
"""Retain bounded original runtime diagnostics without asserting build success.

The complete source/build evidence remains independently required. This snapshot
never edits receipts, follows source links, or replaces the complete archive.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys

MAX_FILE = 64 * 1024**2
MAX_TOTAL = 256 * 1024**2
MAX_FILES = 20000
TARGET = 'x86_64-linux-musl'
INDEX = 'diagnostics.json'
MUSL_MAKEFILE_LINK = 'toolchain-build/obj_musl/Makefile'
MUSL_SOURCE_LINK = 'toolchain-build/src_musl'
MUSL_SOURCE = 'sources/appimage-musl'
# Original musl 1.2.6 files in the fixed source manifest. Its unchanged configure
# creates ../src_musl/Makefile; canonical litecross links src_musl to MUSL_SRCDIR.
MUSL_FILES = {
    'Makefile': {'size': 7515, 'sha256': '55962ce997212ed323f1fc19e26a8a220d65f2d9025d82fadafc2e170db8d6d2'},
    'configure': {'size': 26407, 'sha256': 'aa6574f8049f80f3b0a464bc20ab377a57bc0d3464478ac7ccb500f10002cd78'},
}
PROBE_ICON_LINKS = ('probe/AppDir/.DirIcon', 'probe/extract/squashfs-root/.DirIcon')
PROBE_ICON_TARGET = 'runtime-probe.svg'
# Exact ordinary SVG written by build-appimage-runtime.py's fixed probe.
PROBE_ICON_FILE = {'size': 112, 'sha256': '3ab2ca88fa397435f675faf227a1f509363b4bc6d0fc5800a98fa9a72414016c'}
CONFIG_FILES = ('config.log', 'config.status', 'config.mak', 'configure.log', 'Makefile',
                'CMakeCache.txt', 'build.ninja', 'compile_commands.json', 'install_manifest.txt',
                'CMakeFiles/CMakeConfigureLog.yaml', 'CMakeFiles/CMakeError.log',
                'CMakeFiles/CMakeOutput.log', 'CMakeFiles/rules.ninja')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')


def ordinary(path):
    """Reject links/reparse points in the selected path and every existing parent."""
    path = Path(path)
    for part in reversed((path, *path.parents)):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and
                not getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0),
                'Linked diagnostic path is forbidden: ' + str(part))
    try:
        return path.lstat()
    except FileNotFoundError:
        return None


def signature(info):
    require(info is not None, 'Diagnostic file disappeared')
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def digest(path):
    before = ordinary(path)
    require(before is not None and stat.S_ISREG(before.st_mode), 'Missing regular diagnostic file: ' + str(path))
    value = hashlib.sha256()
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    with os.fdopen(os.open(path, flags), 'rb') as stream:
        require(signature(os.fstat(stream.fileno())) == signature(before), 'Diagnostic file changed before reading')
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
        require(signature(os.fstat(stream.fileno())) == signature(before), 'Diagnostic file changed during reading')
    require(signature(ordinary(path)) == signature(before), 'Diagnostic path changed during reading')
    return value.hexdigest()


def original_link(path):
    ordinary(path.parent)
    before = path.lstat()
    require(stat.S_ISLNK(before.st_mode), 'Known diagnostic relation is not a symlink')
    target = os.readlink(path)
    require(signature(path.lstat()) == signature(before), 'Original configuration link changed')
    return {'originalPath': str(path), 'target': target, 'identity': list(signature(before)),
            'targetSha256': hashlib.sha256(os.fsencode(target)).hexdigest()}


def musl_makefile_relation(workspace):
    link = original_link(workspace / MUSL_MAKEFILE_LINK)
    source_link = original_link(workspace / MUSL_SOURCE_LINK)
    source = workspace / MUSL_SOURCE
    require(link['target'] == '../src_musl/Makefile' and source_link['target'] in
            (str(source), os.path.relpath(source, (workspace / MUSL_SOURCE_LINK).parent)),
            'Unknown or escaped musl configuration source link')
    for name, expected in MUSL_FILES.items():
        path = source / name
        info = ordinary(path)
        require(info is not None and stat.S_ISREG(info.st_mode) and info.st_size == expected['size'] and
                digest(path) == expected['sha256'], 'Known musl configuration source bytes changed')
    return {'profile': 'musl-1.2.6-configure-makefile', 'link': link, 'sourceDirectoryLink': source_link,
            'targetOriginalPath': str(source / 'Makefile'), 'targetPath': 'materials/' + MUSL_SOURCE + '/Makefile',
            'configurePath': 'materials/' + MUSL_SOURCE + '/configure',
            'meaning': 'Observed original two-link relation; only the fixed regular Makefile/configure bytes are copied.'}


def probe_icon_relation(workspace, relative):
    require(relative in PROBE_ICON_LINKS, 'Unknown runtime probe icon location')
    path = workspace / relative
    link = original_link(path)
    require(link['target'] == PROBE_ICON_TARGET, 'Unknown or escaped runtime probe icon link')
    target = path.parent / PROBE_ICON_TARGET
    info = ordinary(target)
    require(info is not None and stat.S_ISREG(info.st_mode) and info.st_size == PROBE_ICON_FILE['size'] and
            digest(target) == PROBE_ICON_FILE['sha256'], 'Original runtime probe icon bytes changed')
    return {'profile': 'runtime-source-probe-icon-v1', 'link': link,
            'targetOriginalPath': str(target),
            'targetPath': 'materials/' + target.relative_to(workspace).as_posix(),
            'targetFile': dict(PROBE_ICON_FILE),
            'meaning': 'Original fixed payload icon link metadata; only its ordinary target bytes are copied.'}


def selected_files(workspace, linked_records=None, probe_links=None):
    selected = {}
    linked_records = [] if linked_records is None else linked_records
    probe_links = [] if probe_links is None else probe_links

    def add(path):
        relative = path.relative_to(workspace).as_posix()
        if relative in PROBE_ICON_LINKS and path.is_symlink():
            relation = probe_icon_relation(workspace, relative)
            require(not any(item['link']['originalPath'] == str(path) for item in probe_links),
                    'Duplicate original runtime probe icon link')
            probe_links.append(relation)
            add(path.parent / PROBE_ICON_TARGET)
            return
        if path.relative_to(workspace).as_posix() == MUSL_MAKEFILE_LINK and path.is_symlink():
            relation = musl_makefile_relation(workspace)
            require(not linked_records, 'Duplicate musl configuration link observation')
            linked_records.append(relation)
            for name in MUSL_FILES:
                add(workspace / MUSL_SOURCE / name)
            return
        info = ordinary(path)
        if info is None:
            return
        require(stat.S_ISREG(info.st_mode), 'Selected diagnostic is not a regular file: ' + str(path))
        name = path.relative_to(workspace).as_posix()
        require('\\' not in name and not any(ord(character) < 32 for character in name),
                'Invalid diagnostic relative path')
        key = name.casefold()
        require(key not in selected or selected[key] == path, 'Duplicate diagnostic destination: ' + name)
        selected[key] = path
        require(len(selected) <= MAX_FILES, 'Too many selected diagnostic files')

    def tree(directory, predicate=None):
        info = ordinary(directory)
        if info is None:
            return
        require(stat.S_ISDIR(info.st_mode), 'Selected diagnostic root is not a directory: ' + str(directory))
        for path in sorted(directory.iterdir()):
            # Inspect the entry itself without following links. The complete
            # receipt trees admit every file and therefore reject every link.
            # Static-input selection ignores unrelated aliases and never walks
            # a directory link; selected archive/CRT links still fail in add().
            info = path.lstat()
            linked = stat.S_ISLNK(info.st_mode) or bool(
                getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0))
            if stat.S_ISDIR(info.st_mode) and not linked:
                tree(path, predicate)
            elif predicate is None or predicate(path):
                add(path)

    for name in ('build-info.json', 'source-plan-fragment.json', 'runtime-link.map',
                 'runtime-link-closure.json', 'toolchain-probe.json'):
        add(workspace / name)
    # Original final runtime outputs are small enough to retain directly. Do not
    # infer them from the embedded probe image or substitute descriptors for bytes.
    for name in ('runtime.o', 'runtime.unstripped', 'runtime-x86_64',
                 'runtime-x86_64.debug', 'LICENSE.runtime-sources'):
        add(workspace / 'install' / name)
    for name in ('control', 'source-inputs', 'probe'):
        tree(workspace / name)
    for path in sorted(workspace.iterdir()):
        if path.name.endswith('-source-receipt'):
            tree(path)
        if path.name.endswith('-build'):
            info = ordinary(path)
            require(info is not None and stat.S_ISDIR(info.st_mode), 'Invalid original build diagnostic directory')
            for name in CONFIG_FILES:
                add(path / name)
            for name in ('meson-logs', 'meson-info'):
                tree(path / name)
    build = workspace / 'toolchain-build'
    if ordinary(build) is not None:
        for path in sorted(build.iterdir()):
            if path.name.startswith('obj_'):
                require(stat.S_ISDIR(ordinary(path).st_mode), 'Invalid original toolchain object directory')
                for name in ('config.log', 'config.status', 'config.mak', 'Makefile'):
                    add(path / name)
        for component in ('libgcc', 'libstdc++-v3'):
            for name in ('Makefile', 'config.log', 'config.status'):
                add(build / 'obj_gcc' / TARGET / component / name)

    def static_input(path):
        return (fnmatch.fnmatchcase(path.name, 'libgcc*.a') or
                re.fullmatch(r'(?:S|r)?crt[^/]*\.o', path.name) is not None or
                path.name in ('libc.a', 'libm.a', 'libpthread.a', 'librt.a', 'libstdc++.a', 'libatomic.a'))

    for name in ('lib', 'lib64', TARGET + '/lib', TARGET + '/lib64'):
        tree(workspace / 'toolchain' / name, static_input)
    tree(workspace / 'libraries/lib', lambda path: path.suffix == '.a')
    return sorted(selected.values())


def copy_original(source, target, expected):
    before = ordinary(source)
    require(before is not None and signature(before) == expected, 'Original diagnostic changed before copying')
    target.parent.mkdir(parents=True, exist_ok=True)
    ordinary(target.parent)
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    value = hashlib.sha256()
    with os.fdopen(os.open(source, flags), 'rb') as original, target.open('xb') as retained:
        require(signature(os.fstat(original.fileno())) == expected, 'Original diagnostic identity changed')
        for block in iter(lambda: original.read(1024 * 1024), b''):
            retained.write(block)
            value.update(block)
        require(signature(os.fstat(original.fileno())) == expected, 'Original diagnostic changed while copying')
    target.chmod(stat.S_IMODE(before.st_mode) & 0o777)
    require(signature(ordinary(source)) == expected and source.stat().st_size == target.stat().st_size and
            digest(source) == digest(target) == value.hexdigest(), 'Original or retained diagnostic bytes changed')
    return {'size': before.st_size, 'sha256': value.hexdigest(), 'mode': stat.S_IMODE(before.st_mode) & 0o777}


def verify_retained(output, report):
    require(report['kind'] == 'huhucapture-runtime-diagnostics' and report['schemaVersion'] == 1,
            'Unknown diagnostic index')
    expected = set()
    for item in report['files']:
        name = item['path']
        require(isinstance(name, str) and name.startswith('materials/') and '\\' not in name and
                all(part not in ('', '.', '..') for part in name.split('/')) and name.casefold() not in expected,
                'Escaped or duplicate retained diagnostic path')
        expected.add(name.casefold())
        path = output / name
        info = ordinary(path)
        require(info is not None and stat.S_ISREG(info.st_mode) and info.st_size == item['size'] and
                stat.S_IMODE(info.st_mode) & 0o777 == item['mode'] and digest(path) == item['sha256'],
                'Retained diagnostic does not match its original indexed bytes')
    actual = set()
    pending = [output / 'materials'] if (output / 'materials').exists() else []
    while pending:
        directory = pending.pop()
        require(stat.S_ISDIR(ordinary(directory).st_mode), 'Invalid retained materials directory')
        for path in directory.iterdir():
            info = ordinary(path)
            require(info is not None, 'Retained diagnostic disappeared')
            if stat.S_ISDIR(info.st_mode):
                pending.append(path)
            else:
                require(stat.S_ISREG(info.st_mode), 'Retained diagnostic is not a regular file')
                name = path.relative_to(output).as_posix().casefold()
                require(name not in actual, 'Case-colliding retained diagnostic')
                actual.add(name)
    require(actual == expected, 'Retained diagnostic inventory differs from the original index')
    links = report.get('knownSourceLinks', [])
    require(len(links) <= 1, 'Duplicate known diagnostic source link')
    for relation in links:
        workspace = Path(report['workspace'])
        source = workspace / MUSL_SOURCE
        link, alias = relation['link'], relation['sourceDirectoryLink']
        require(relation['profile'] == 'musl-1.2.6-configure-makefile' and
                link['originalPath'] == str(workspace / MUSL_MAKEFILE_LINK) and link['target'] == '../src_musl/Makefile' and
                alias['originalPath'] == str(workspace / MUSL_SOURCE_LINK) and alias['target'] in
                (str(source), os.path.relpath(source, (workspace / MUSL_SOURCE_LINK).parent)) and
                relation['targetOriginalPath'] == str(source / 'Makefile') and
                relation['targetPath'] == 'materials/' + MUSL_SOURCE + '/Makefile' and
                relation['configurePath'] == 'materials/' + MUSL_SOURCE + '/configure',
                'Retained musl configuration link relationship changed')
        require(all(item['targetSha256'] == hashlib.sha256(os.fsencode(item['target'])).hexdigest()
                    for item in (link, alias)), 'Retained original link text changed')
        indexed = {item['path']: item for item in report['files']}
        for name, original in MUSL_FILES.items():
            item = indexed.get('materials/' + MUSL_SOURCE + '/' + name)
            require(item is not None and item['originalPath'] == str(source / name) and
                    all(item[key] == original[key] for key in ('size', 'sha256')),
                    'Retained musl link target or configure differs from its fixed original source')
    probe_links = report.get('knownProbeLinks', [])
    require(len(probe_links) <= len(PROBE_ICON_LINKS), 'Too many known runtime probe links')
    indexed = {item['path']: item for item in report['files']}
    observed = set()
    for relation in probe_links:
        workspace = Path(report['workspace'])
        link = relation['link']
        locations = {str(workspace / name): name for name in PROBE_ICON_LINKS}
        original = link['originalPath']
        require(original in locations and original not in observed and
                relation['profile'] == 'runtime-source-probe-icon-v1' and link['target'] == PROBE_ICON_TARGET and
                link['targetSha256'] == hashlib.sha256(os.fsencode(PROBE_ICON_TARGET)).hexdigest(),
                'Retained runtime probe icon relationship changed')
        observed.add(original)
        identity = link['identity']
        require(len(identity) == 6 and all(type(value) is int for value in identity) and
                stat.S_ISLNK(identity[2]) and identity[3] == len(os.fsencode(PROBE_ICON_TARGET)),
                'Retained original probe link identity changed')
        target = Path(locations[original]).parent / PROBE_ICON_TARGET
        expected_path = 'materials/' + target.as_posix()
        item = indexed.get(expected_path)
        require(relation['targetOriginalPath'] == str(workspace / target) and
                relation['targetPath'] == expected_path and relation['targetFile'] == PROBE_ICON_FILE and
                item is not None and item['originalPath'] == str(workspace / target) and
                all(item[key] == PROBE_ICON_FILE[key] for key in ('size', 'sha256')),
                'Retained runtime probe icon target differs from its fixed original bytes')


def retain(workspace, output):
    workspace, output = Path(os.path.abspath(workspace)), Path(os.path.abspath(output))
    original = ordinary(workspace)
    require(original is None or stat.S_ISDIR(original.st_mode), 'Original runtime workspace is not a directory')
    require(ordinary(output) is None and not output.is_relative_to(workspace) and not workspace.is_relative_to(output),
            'Use a new diagnostic output directory separate from the original workspace')
    linked_records, probe_links = [], []
    paths = selected_files(workspace, linked_records, probe_links) if original is not None else []
    snapshots = [(path, ordinary(path)) for path in paths]
    require(all(info is not None and 0 <= info.st_size <= MAX_FILE for _, info in snapshots),
            'An original diagnostic exceeds the per-file size limit')
    total = sum(info.st_size for _, info in snapshots)
    require(total <= MAX_TOTAL, 'Original diagnostics exceed the total size limit')
    output.mkdir(parents=True, exist_ok=False)
    files = []
    for path, info in snapshots:
        name = 'materials/' + path.relative_to(workspace).as_posix()
        actual = copy_original(path, output / name, signature(info))
        files.append({'originalPath': str(path), 'path': name, **actual})
    current_links, current_probe_links = [], []
    require((ordinary(workspace) is None) if original is None else
            (selected_files(workspace, current_links, current_probe_links) == paths and
             current_links == linked_records and current_probe_links == probe_links),
            'Original diagnostic selection changed')
    report = {'schemaVersion': 1, 'kind': 'huhucapture-runtime-diagnostics', 'workspace': str(workspace),
              'state': 'workspace-absent' if original is None else ('materials-retained' if files else 'no-selected-materials'),
              'files': files, 'knownSourceLinks': linked_records, 'knownProbeLinks': probe_links, 'materialBytes': total,
              'limits': {'perFileBytes': MAX_FILE, 'totalBytes': MAX_TOTAL, 'fileCount': MAX_FILES},
              'scope': 'Exact original diagnostics only; no build, static closure or corresponding-source success is asserted. '
                       'Original receipts are unchanged; the separately retained complete evidence remains required.'}
    data = canonical(report)
    require(total + len(data) <= MAX_TOTAL, 'Diagnostics and index exceed the total size limit')
    verify_retained(output, report)
    for item, (path, info) in zip(files, snapshots):
        require(signature(ordinary(path)) == signature(info) and digest(path) == item['sha256'],
                'Original diagnostic changed before index publication')
    require(not linked_records or musl_makefile_relation(workspace) == linked_records[0],
            'Original musl configuration link changed before index publication')
    for relation in probe_links:
        relative = Path(relation['link']['originalPath']).relative_to(workspace).as_posix()
        require(probe_icon_relation(workspace, relative) == relation,
                'Original runtime probe icon link changed before index publication')
    with (output / INDEX).open('xb') as stream:
        stream.write(data)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = retain(args.workspace, args.output)
    except (OSError, ValueError) as error:
        print('Runtime diagnostic retention failed: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps({'index': str(args.output / INDEX), 'state': report['state'],
                      'files': len(report['files']), 'materialBytes': report['materialBytes']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
