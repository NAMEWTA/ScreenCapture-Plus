"""Retain the actual notice and collector-report inputs copied into the packages."""
from __future__ import annotations

from pathlib import Path
import shutil


REPORTS = {
    'collected-native/native-materials.json', 'collected-qt/qt-materials.json',
    'collected-runtime/runtime-materials.json', 'collected-rust/rust-materials.json',
    'collected-rust-toolchain/rust-toolchain-materials.json',
}
NOTICE_PREFIXES = (
    'collected-runtime/notices/system/', 'runtime/system/', 'collected-rust/rust/crates/',
    'collected-rust/rust/toolchain/', 'collected-rust-toolchain/rust-toolchain/',
)
INPUTS = 'installed-notices'


def selected(relative):
    for prefix in ('licenses/', 'share/huhucapture/licenses/'):
        if relative.startswith(prefix):
            notice = relative.removeprefix(prefix)
            return notice in REPORTS or notice.startswith(NOTICE_PREFIXES)
    return False


def record(installed, execution, files, api):
    for entry in files:
        name = entry['path']
        if entry['type'] != 'file' or not entry['size'] or not selected(name):
            continue
        original = installed / name
        if not text_input(original):
            continue
        target = execution / INPUTS / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with original.open('rb') as source, target.open('xb') as destination:
            shutil.copyfileobj(source, destination)
        api.require(api.digest(target) == api.digest(original) == entry['sha256'] and target.stat().st_size == entry['size'],
                    'An actual installed notice changed during package input retention: ' + name)


def text_input(path):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 16 * 1024 * 1024:
        return False
    with path.open('rb') as stream:
        header = stream.read(4)
    return not header.startswith((b'MZ', b'\x7fELF')) and b'\0' not in header


def verified(stage, bound, installed, api):
    root = stage / 'executions' / INPUTS
    if not root.is_dir():
        return {}
    snapshots = {item['path']: item for item in api.inventory(root)}
    original = {item['path']: item for item in installed}
    api.require(all(name in original and selected(name) and text_input(root / name) and
                    all(item[key] == original[name][key] for key in ('size', 'sha256'))
                    for name, item in snapshots.items()), 'The retained installed notice inputs differ from the packaging originals')
    result = {}
    for name, chain in bound['origins'].items():
        source = chain['input']['path']
        if source not in snapshots or not name.endswith('/' + source):
            continue
        snapshot = snapshots[source]
        if all(chain['input'][field] == chain['output'][field] == snapshot[field] for field in ('size', 'sha256')):
            result[name] = {'path': (Path(INPUTS) / source).as_posix(), 'size': snapshot['size'], 'sha256': snapshot['sha256']}
    return result
