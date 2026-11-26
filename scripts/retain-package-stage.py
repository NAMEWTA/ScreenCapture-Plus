#!/usr/bin/env python3
"""Retain and reverify the original minimal package stage without changing intent."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import stat
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('huhucapture_retained_packager', ROOT / 'scripts/package-qt.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
api = package.source_inventory_api()
PREFIX = 'huhucapture-prepared-stage'


def archive_descriptor(path):
    path = Path(path)
    api.require(path.is_file() and not path.is_symlink() and
                0 < path.stat().st_size <= api.MAX_TREE + api.MAX_FILES * 4096,
                'Retained stage archive is missing, linked or oversized')
    return {'name': path.name, 'size': path.stat().st_size, 'sha256': api.digest(path)}


def stage_roots(platform):
    api.require(platform in ('linux', 'windows'), 'Unknown original stage platform')
    result = ['prepared.json', 'transformations.json', 'file-transformations.json',
              'installed-input-inventory.json', 'source-deployment-inventory.json',
              'executions', 'payloads', 'artifacts']
    if platform == 'linux':
        result += ['linuxdeploy-output-inventory.json', 'linux-stripped-inventory.json', 'work/debug']
    return result


def selected_inventory(stage, roots):
    """Exact selected file modes/links, including empty files and directories."""
    stage = Path(stage).resolve(strict=True)
    selected = {}
    total = 0
    def add(path):
        nonlocal total
        name = api.relative(path.relative_to(stage).as_posix())
        if name in selected:
            return
        info = path.lstat()
        api.require(not info.st_mode & 0o7000, 'Special permission bits are not package-stage material')
        item = {'path': name, 'mode': stat.S_IMODE(info.st_mode)}
        if stat.S_ISLNK(info.st_mode):
            item.update(type='symlink', target=api.checked_link(name, os.readlink(path)))
        elif stat.S_ISDIR(info.st_mode):
            item['type'] = 'directory'
        else:
            api.require(stat.S_ISREG(info.st_mode) and info.st_size <= api.MAX_FILE,
                        'A retained stage contains a special or oversized file')
            total += info.st_size
            item.update(type='file', size=info.st_size, sha256=api.digest(path))
        selected[name] = item
        api.require(len(selected) <= api.MAX_FILES and total <= api.MAX_TREE, 'Retained stage exceeds bounded limits')
    for name in roots:
        path = stage / api.relative(name)
        api.require(path.exists() and not path.is_symlink(), 'Required original stage path is missing or linked: ' + name)
        for parent in reversed(path.relative_to(stage).parents):
            if parent != Path('.'):
                add(stage / parent)
        add(path)
        if path.is_dir():
            for folder, directories, files in os.walk(path, followlinks=False):
                for entry in sorted(directories + files):
                    add(Path(folder) / entry)
    api.require(len({name.casefold() for name in selected}) == len(selected), 'Case-colliding retained stage paths')
    for item in selected.values():
        if item['type'] == 'symlink':
            target = (stage / item['path']).resolve(strict=True)
            api.require(target.is_relative_to(stage) and target.relative_to(stage).as_posix() in selected,
                        'A retained stage link escapes the selected original files')
    return sorted(selected.values(), key=lambda item: item['path'])


def write_archive(stage, roots, archive, expected):
    api.require(selected_inventory(stage, roots) == expected, 'Original selected stage changed before retention')
    with Path(archive).open('xb') as output, gzip.GzipFile(filename='', mode='wb', fileobj=output, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as stream:
            root = tarfile.TarInfo(PREFIX); root.type = tarfile.DIRTYPE; root.mode = 0o755
            stream.addfile(root)
            for item in expected:
                entry = tarfile.TarInfo(PREFIX + '/' + item['path'])
                entry.mode = item['mode']
                if item['type'] == 'directory':
                    entry.type = tarfile.DIRTYPE
                    stream.addfile(entry)
                elif item['type'] == 'symlink':
                    entry.type = tarfile.SYMTYPE; entry.linkname = item['target']
                    stream.addfile(entry)
                else:
                    entry.size = item['size']
                    with (stage / item['path']).open('rb') as source:
                        stream.addfile(entry, source)
    api.require(selected_inventory(stage, roots) == expected, 'Original selected stage changed during retention')
    return archive_descriptor(archive)


def extract_archive(archive, destination, expected):
    api.require(archive_descriptor(archive) == expected, 'Retained stage archive bytes differ from their original digest')
    destination = Path(destination)
    api.require(not destination.exists(), 'Retained stage extraction requires a new directory')
    destination.mkdir(parents=True)
    seen, modes, total = set(), [], 0
    with tarfile.open(archive, 'r:gz') as stream:
        for entry in stream:
            name = entry.name.rstrip('/')
            if name == PREFIX:
                api.require(entry.isdir() and '' not in seen, 'Duplicate or invalid retained archive root')
                seen.add('')
                continue
            api.require(name.startswith(PREFIX + '/'), 'Archive entry is outside the retained stage root')
            name = api.relative(name[len(PREFIX) + 1:])
            api.require(name.casefold() not in seen and len(seen) < api.MAX_FILES and
                        0 <= entry.size <= api.MAX_FILE and not entry.mode & ~0o777,
                        'Duplicate, oversized or privileged retained archive entry')
            seen.add(name.casefold())
            path = destination / name
            api.ensure_parents(destination, path)
            if entry.isdir():
                path.mkdir(exist_ok=True); modes.append((path, entry.mode))
            elif entry.issym():
                os.symlink(api.checked_link(name, entry.linkname), path)
            else:
                api.require(entry.isfile(), 'Retained archive contains a hardlink or special file')
                total += entry.size
                api.require(total <= api.MAX_TREE, 'Retained archive exceeds the expanded size limit')
                source = stream.extractfile(entry)
                api.require(source is not None, 'Retained archive file data is missing')
                with source, path.open('xb') as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
                api.require(path.stat().st_size == entry.size, 'Retained archive file is truncated')
                path.chmod(entry.mode)
    for path, mode in reversed(modes):
        path.chmod(mode)
    api.require(archive_descriptor(archive) == expected, 'Retained archive changed during extraction')
    # This checks every extracted link after extraction, including cycles and
    # links to a path that was not actually retained.
    api.inventory(destination)


def retain(stage, output):
    stage, output = Path(stage).resolve(strict=True), Path(output).resolve()
    api.require(not output.exists() and not output.is_relative_to(stage) and not stage.is_relative_to(output),
                'Retain into a new output directory separate from the original stage')
    prepared = package.verify_prepared(stage)
    roots = stage_roots(prepared['build']['platform'])
    original = selected_inventory(stage, roots)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='retain-package-stage-', dir=output.parent) as temporary:
        work = Path(temporary)
        build = prepared['build']
        filename = f"huhucapture-v{build['version']}-{build['platform']}-{build['architecture']}-prepared-stage.tar.gz"
        archive = work / filename
        retained = write_archive(stage, roots, archive, original)
        restored = work / 'roundtrip'
        extract_archive(archive, restored, retained)
        api.require(selected_inventory(restored, roots) == original,
                    'Restored original stage differs in bytes, permissions or links')
        api.require(package.verify_prepared(restored) == prepared and package.verify_prepared(stage) == prepared and
                    selected_inventory(stage, roots) == original,
                    'Original or restored package stage failed independent revalidation')
        report = {'schemaVersion': 1, 'kind': 'huhucapture-retained-package-stage',
                  'build': build, 'intent': prepared['intent'], 'archive': retained,
                  'prepared': api.descriptor(stage / 'prepared.json'), 'retainedRoots': roots,
                  'retainedEntries': original,
                  'inventorySha256': hashlib.sha256(api.canonical(original)).hexdigest(),
                  'verification': 'Original and safely extracted stages both passed verify_prepared with identical metadata',
                  'scope': 'Original package-stage retention only; source completeness and release gates are unchanged'}
        report_path = work / 'package-stage-retention.json'
        report_path.write_bytes(api.canonical(report))
        output.mkdir()
        archive.rename(output / filename)
        report_path.rename(output / report_path.name)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = retain(args.stage, args.output)
    print('Retained original package stage:', report['archive']['name'], report['archive']['sha256'])


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, tarfile.TarError) as error:
        print('Package-stage retention failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
