"""Facts read from real Qt deployment trees and artifacts; no publishing API."""
from __future__ import annotations

import hashlib
import json
import mmap
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct

PRODUCT = 'huhucapture'
REPOSITORY = 'NAMEWTA/huhucapture'
VERSION = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z')
HEX40 = re.compile(r'[0-9a-f]{40}\Z')
HEX64 = re.compile(r'[0-9a-f]{64}\Z')
KINDS = {
    'installer': ('windows', 'x64', '-windows-x64-setup.exe', 1024**3),
    'portable-zip': ('windows', 'x64', '-windows-x64.zip', 2*1024**3),
    'deb': ('linux', 'amd64', '-linux-amd64.deb', 1024**3),
    'appimage': ('linux', 'amd64', '-linux-amd64.AppImage', 2*1024**3),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while chunk := source.read(1024*1024):
            digest.update(chunk)
    return digest.hexdigest()


def json_read(path: Path, limit=8*1024*1024):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= limit,
            f'Missing, linked or oversized JSON input: {path}')
    return json.loads(path.read_text(encoding='utf-8'))


def json_write(path: Path, value):
    # No existing output is silently replaced, including results of an earlier
    # interrupted packaging run. Caller owns the containing fresh workspace.
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False)
        stream.write('\n')


def safe_relative(value: str):
    require(isinstance(value, str) and value and len(value.encode('utf-8')) <= 4096 and
            not any(c in value for c in ('\\', ':', '\0')) and not value.startswith('/'),
            'Invalid relative package path')
    require(all(part not in ('', '.', '..') for part in value.split('/')), 'Unsafe package path component')
    return value


def asset_name(version: str, kind: str):
    require(VERSION.fullmatch(version) and kind in KINDS, 'Invalid version or package kind')
    return PRODUCT + '-v' + version + KINDS[kind][2]


def descriptor(path: Path):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size > 0,
            f'Expected an actual nonempty regular artifact: {path}')
    return {'name': path.name, 'size': path.stat().st_size, 'sha256': sha256(path)}


def identity(value: dict, *, development=False):
    require(isinstance(value, dict) and value.get('schemaVersion') == 1 and
            value.get('product') == PRODUCT and value.get('repository') == REPOSITORY,
            'Build identity does not describe the canonical product')
    version = value.get('version', '')
    require(isinstance(version, str) and VERSION.fullmatch(version) and value.get('tag') == 'v'+version,
            'Build version and tag differ or are noncanonical')
    require(HEX40.fullmatch(value.get('sourceCommit', '')) is not None, 'Source commit must be exact lowercase Git SHA')
    require((value.get('platform'), value.get('architecture')) in (('windows', 'x64'), ('linux', 'amd64')),
            'Unsupported build platform or architecture')
    require(value.get('configuration') in ('Release', 'RelWithDebInfo'), 'Only actual release-CRT builds can be packaged')
    require(type(value.get('sourceDirty')) is bool and (development or not value['sourceDirty']),
            'Dirty sources require an explicitly marked development bundle; they cannot become release metadata')
    require(HEX64.fullmatch(value.get('dependencyLockSha256', '')) is not None and
            HEX64.fullmatch(value.get('cargoLockSha256', '')) is not None, 'Missing actual dependency lock identities')
    return {key: value[key] for key in ('schemaVersion', 'product', 'repository', 'tag', 'version', 'sourceCommit', 'sourceDirty')}


def binary_identity(path: Path):
    require(path.is_file() and not path.is_symlink(), 'The product executable must be a regular file')
    start = b'HUHU_BUILD_IDENTITY_V1\0'
    end = b'\0HUHU_BUILD_IDENTITY_END'
    with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        offset = data.find(start)
        require(offset >= 0 and data.find(start, offset+len(start)) < 0,
                'Executable has no unique embedded build identity; reconfigure/rebuild the real Qt target')
        boundary = data.find(end, offset+len(start))
        require(boundary > offset and boundary - offset < 16384, 'Invalid embedded build identity bounds')
        return json.loads(data[offset+len(start):boundary].decode('utf-8'))


def machine(path: Path):
    with path.open('rb') as stream:
        prefix = stream.read(64)
        if prefix.startswith(b'\x7fELF'):
            require(len(prefix) == 64 and prefix[4:6] == b'\x02\x01', 'Expected an ELF64 little-endian runtime file')
            require(struct.unpack_from('<H', prefix, 18)[0] == 62, 'ELF runtime is not amd64')
            return 'linux'
        if prefix.startswith(b'MZ'):
            require(len(prefix) == 64, 'Truncated PE DOS header')
            offset = struct.unpack_from('<I', prefix, 60)[0]
            require(64 <= offset <= 1024*1024, 'Invalid PE header offset')
            stream.seek(offset); header = stream.read(26)
            require(len(header) == 26 and header[:4] == b'PE\0\0', 'Missing actual PE signature')
            require(struct.unpack_from('<H', header, 4)[0] == 0x8664 and
                    struct.unpack_from('<H', header, 24)[0] == 0x20b, 'Runtime PE is not x64 PE32+')
            return 'windows'
    raise ValueError(f'Runtime is neither a genuine PE nor ELF: {path}')


def verify_binary(path: Path, recorded: dict):
    require(machine(path) == recorded['platform'] and binary_identity(path) == recorded,
            'Actual executable identity differs from its sidecar')


def verify_artifact(folder: Path, value: dict, kind: str, version: str):
    require(kind in KINDS and value.get('name') == asset_name(version, kind),
            'Artifact name does not match its version/platform/package form')
    actual = descriptor(folder/value['name'])
    require(actual == {key: value.get(key) for key in ('name', 'size', 'sha256')} and
            actual['size'] <= KINDS[kind][3], 'Artifact bytes changed after packaging')
    with (folder/value['name']).open('rb') as source:
        header = source.read(72)
    signatures = {'portable-zip': b'PK\x03\x04', 'installer': b'MZ', 'deb': b'!<arch>\n', 'appimage': b'\x7fELF'}
    require(header.startswith(signatures[kind]) and (kind != 'appimage' or header[8:11] == b'AI\x02'),
            'Artifact header does not match its declared package form')
    return actual


def inventory(root: Path, prefix=''):
    root = root.resolve()
    result, seen = [], set()
    for path in sorted(root.rglob('*')):
        if path.is_dir() and not path.is_symlink():
            continue
        relative = path.relative_to(root).as_posix()
        key = safe_relative(prefix + relative)
        require(key not in seen, 'Duplicate inventory path'); seen.add(key)
        if path.is_symlink():
            target = os.readlink(path)
            require(not os.path.isabs(target) and '\0' not in target and path.resolve().is_relative_to(root) and path.resolve().is_file(),
                    f'Package link escapes or is dangling: {relative}')
            entry = {'path': key, 'size': path.stat().st_size, 'sha256': sha256(path), 'linkTarget': target}
        else:
            require(stat.S_ISREG(path.stat().st_mode), f'Package contains a special file: {relative}')
            entry = {'path': key, 'size': path.stat().st_size, 'sha256': sha256(path)}
        require(entry['size'] <= 2*1024**3, f'Oversized runtime file: {relative}')
        result.append(entry)
    require(result and len(result) <= 8192, 'Empty or oversized deployment inventory')
    return result


def verify_inventory(root: Path, expected):
    require(inventory(root) == expected, 'Deployment files changed after their inventory was recorded')


def audit_tree(root: Path, kind: str, *, development=False):
    require(kind in KINDS, 'Unknown package form')
    platform, architecture, _, _ = KINDS[kind]
    binary_dir = root if platform == 'windows' else root / 'bin'
    marker = json_read(binary_dir/'installation.json', 16384)
    require(marker == {'schemaVersion': 1, 'product': PRODUCT, 'platform': platform,
                       'architecture': architecture, 'packageKind': kind},
            'Installation marker does not describe this actual package')
    recorded = json_read(binary_dir/'build-identity.json', 16384)
    identity(recorded, development=development)
    require(recorded['platform'] == platform and recorded['architecture'] == architecture,
            'Build receipt and selected package platform differ')
    executable = binary_dir / ('huhucapture.exe' if platform == 'windows' else 'huhucapture')
    verify_binary(executable, recorded)
    all_files = inventory(root)
    leaves = {PurePosixPath(entry['path']).name: entry['path'] for entry in all_files if entry['size']}
    qt_version = recorded.get('qtVersion')
    require(qt_version and VERSION.fullmatch(qt_version), 'Build receipt omits the actual Qt SDK version')
    required = (['Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'qwindows.dll',
                 'vcruntime140.dll', 'msvcp140.dll', 'avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll']
                if platform == 'windows' else
                [f'libQt6Core.so.{qt_version}', f'libQt6Gui.so.{qt_version}', f'libQt6Widgets.so.{qt_version}',
                 f'libQt6Network.so.{qt_version}', 'libqxcb.so', 'libpipewire-0.3.so.0', 'libportal.so.1',
                 'libportal-qt6.so.1', 'libavcodec.so.63', 'libavformat.so.63', 'libavutil.so.61'])
    for name in required:
        require(name in leaves, f'Incomplete runtime dependency tree: missing {name}')
    require(any('/imageformats/' in '/'+entry['path'] for entry in all_files), 'Qt image-format plugins are missing')
    require(any('/tls/' in '/'+entry['path'] for entry in all_files), 'Qt HTTPS/TLS backend is missing')
    require(any(PurePosixPath(entry['path']).name.startswith('qtbase_') and entry['path'].endswith('.qm') and entry['size']
                for entry in all_files), 'Qt base translations are missing')
    license_root = root/('licenses' if platform == 'windows' else 'share/huhucapture/licenses')
    license_files = ['LICENSE', 'NOTICE.md', 'project/gifski/LICENSE', 'project/localsend/LICENSE',
                     'runtime/ffmpeg/LICENSE.md']
    license_files += (['runtime/ffmpeg/COPYING.LGPLv2.1'] if platform == 'windows' else
                      ['runtime/ffmpeg/COPYING.GPLv3', 'runtime/x264/COPYING', 'runtime/portal/COPYING',
                       'runtime/pipewire/LICENSE', 'runtime/pipewire/COPYING'])
    for relative in license_files:
        material = license_root/relative
        require(material.is_file() and material.stat().st_size, f'Dependency license material is missing: {relative}')
    if platform == 'windows':
        require('qschannelbackend.dll' in leaves, 'Native Windows HTTPS backend is missing')
    if platform == 'linux':
        require(any(name.startswith('libqwayland') and name.endswith('.so') for name in leaves), 'Qt Wayland platform plugin is missing')
        for name in ('libqopensslbackend.so', 'libssl.so.3', 'libcrypto.so.3'):
            require(name in leaves, f'Actual Qt HTTPS runtime is missing: {name}')
        for relative in ['share/pipewire/client.conf', 'lib/pipewire-0.3/libpipewire-module-protocol-native.so',
                         'lib/pipewire-0.3/libpipewire-module-client-node.so', 'lib/spa-0.2/support/libspa-support.so',
                         'lib/spa-0.2/audioconvert/libspa-audioconvert.so', 'lib/spa-0.2/videoconvert/libspa-videoconvert.so']:
            require((root/relative).is_file(), f'Missing explicit PipeWire client runtime: {relative}')
    for entry in all_files:
        name = PurePosixPath(entry['path']).name
        require(not re.match(r'(?i)(?:Qt6\w+d|vcruntime\d\w*d|msvcp\d\w*d|ucrtbased)\.dll$', name), 'Debug CRT/Qt DLL in release package')
        if platform == 'linux':
            require(name not in ('pipewire', 'pipewire-pulse', 'wireplumber', 'Xorg', 'weston') and
                    not re.fullmatch(r'(?:ld-linux[\w-]*|lib(?:c|m|pthread|dl|rt|GL[\w-]*|EGL[\w-]*|'
                                     r'vulkan(?:_[\w-]+)?|drm(?:_[\w-]+)?|nvidia[\w-]*|cuda[\w-]*|gbm|glapi))'
                                     r'\.so(?:\.\d+)*', name),
                    f'Host daemon/loader/graphics driver must not be shipped: {name}')
        if name.lower().endswith('.dll') or re.search(r'\.so(?:\.\d+)*$', name):
            require(machine(root/entry['path']) == platform, 'Mixed native architectures in package')
    return recorded, all_files


def locked_dependencies(repository: Path, platform: str):
    lock = json_read(repository/'build/dependencies.lock.json')['qtMigration']
    gif = json_read(repository/'third_party/gifski/upstream.json')
    local = json_read(repository/'third_party/localsend/upstream.json')
    ffmpeg = lock['softwareMedia']['ffmpeg']
    result = [
        {'name': 'qt', 'version': lock['qt']['version'], 'license': lock['qt']['qtbaseLicense'],
         'source': lock['qt']['qtbaseRepository'], 'commit': lock['qt']['qtbaseCommit']},
        {'name': 'gifski', 'version': gif['version'], 'license': gif['license'], 'source': gif['repository'], 'commit': gif['commit']},
        {'name': 'localsend', 'version': local['crate_version'], 'license': local['license'], 'source': local['repository'], 'commit': local['commit']},
        {'name': 'ffmpeg', 'version': ffmpeg['version'], 'license': ffmpeg['configuredLicense'] if platform == 'linux' else lock['videoInspectionWindows']['configuredLicense'],
         'source': ffmpeg['repository'], 'commit': ffmpeg['commit'], 'configure': ffmpeg['configure'] if platform == 'linux' else lock['videoInspectionWindows']['configure'],
         'patches': ffmpeg['patches']},
    ]
    if platform == 'linux':
        for name, value, version, license_key in [('libportal', lock['libportal'], lock['libportal']['version'], 'license'),
                ('pipewire', lock['pipewire'], lock['pipewire']['version'], 'clientLicense'),
                ('x264', lock['softwareMedia']['x264'], str(lock['softwareMedia']['x264']['apiBuild']), 'license')]:
            result.append({'name': name, 'version': version, 'license': value[license_key], 'source': value['repository'], 'commit': value['commit']})
    return result


def write_platform_metadata(output: Path, build: dict, files, packages, dependencies, minimum_system: str,
                            *, development=False, source_materials=None, extra_assets=()):
    base = identity(build, development=development)
    platform, arch = build['platform'], build['architecture']
    require(minimum_system and len(minimum_system.encode('utf-8')) <= 256, 'Minimum-system declaration is required')
    expected_kinds = {'installer', 'portable-zip'} if platform == 'windows' else {'deb', 'appimage'}
    require({item['packageKind'] for item in packages} == expected_kinds and len(packages) == 2, 'Both real package forms are required per platform')
    for item in packages:
        verify_artifact(output, item, item['packageKind'], base['version'])
    result = {**base, 'platform': platform, 'architecture': arch, 'developmentOnly': development,
              'compiler': build['compiler'], 'configuration': build['configuration'], 'qtVersion': build['qtVersion'],
              'dependencyLockSha256': build['dependencyLockSha256'], 'cargoLockSha256': build['cargoLockSha256'],
              'packagingTools': build.get('packagingTools', []),
              'dependencies': dependencies, 'files': files, 'packages': packages,
              'additionalAssets': list(extra_assets), 'sourceMaterials': source_materials}
    require(len(json.dumps(result).encode()) <= 8*1024*1024, 'Actual build inventory exceeds updater metadata budget')
    build_name = f'build-info-{platform}-{arch}.json'; json_write(output/build_name, result)
    build_info = descriptor(output/build_name)
    assets = [{**item, 'platform': platform, 'architecture': arch, 'minimumSystem': minimum_system,
               'buildInfo': build_info} for item in packages]
    partial = {**base, 'developmentOnly': development, 'assets': assets}
    json_write(output/f'package-manifest-{platform}-{arch}.json', partial)
    return result
