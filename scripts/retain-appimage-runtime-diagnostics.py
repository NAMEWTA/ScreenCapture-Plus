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


def selected_files(workspace):
    selected = {}

    def add(path):
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
                path.name in ('libc.a', 'libm.a', 'libstdc++.a'))

    for name in ('lib', 'lib64', TARGET + '/lib', TARGET + '/lib64'):
        tree(workspace / 'toolchain' / name, static_input)
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


def retain(workspace, output):
    workspace, output = Path(os.path.abspath(workspace)), Path(os.path.abspath(output))
    original = ordinary(workspace)
    require(original is None or stat.S_ISDIR(original.st_mode), 'Original runtime workspace is not a directory')
    require(ordinary(output) is None and not output.is_relative_to(workspace) and not workspace.is_relative_to(output),
            'Use a new diagnostic output directory separate from the original workspace')
    paths = selected_files(workspace) if original is not None else []
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
    require((ordinary(workspace) is None) if original is None else selected_files(workspace) == paths,
            'Original diagnostic selection changed')
    report = {'schemaVersion': 1, 'kind': 'huhucapture-runtime-diagnostics', 'workspace': str(workspace),
              'state': 'workspace-absent' if original is None else ('materials-retained' if files else 'no-selected-materials'),
              'files': files, 'materialBytes': total,
              'limits': {'perFileBytes': MAX_FILE, 'totalBytes': MAX_TOTAL, 'fileCount': MAX_FILES},
              'scope': 'Exact original diagnostics only; no build, static closure or corresponding-source success is asserted. '
                       'Original receipts are unchanged; the separately retained complete evidence remains required.'}
    data = canonical(report)
    require(total + len(data) <= MAX_TOTAL, 'Diagnostics and index exceed the total size limit')
    verify_retained(output, report)
    for item, (path, info) in zip(files, snapshots):
        require(signature(ordinary(path)) == signature(info) and digest(path) == item['sha256'],
                'Original diagnostic changed before index publication')
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
