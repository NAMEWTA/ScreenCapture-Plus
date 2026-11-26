#!/usr/bin/env python3
"""Bind the actual locked Rust standard library to official rustup materials.

Install the locked rust-src component with rustup before running this command.
The collector reads the original sysroot; it does not install components,
execute archive installers, modify the toolchain, or resolve dependencies.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile
import tomllib
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
specification = importlib.util.spec_from_file_location('huhucapture_runtime_materials', ROOT / 'scripts/collect-runtime-materials.py')
materials = importlib.util.module_from_spec(specification)
specification.loader.exec_module(materials)
native = materials.module('huhucapture_native_source_materials', ROOT / 'build/package/rust_materials.py')
downloads = materials.module('huhucapture_original_distribution_download', ROOT / 'scripts/restore-packaging-tools.py')


def ordinary_snapshot(path):
    """Source trees legitimately contain empty files; still verify stability."""
    before = path.stat()
    materials.meta.require(not path.is_symlink() and stat.S_ISREG(before.st_mode),
                           'An original Rust input is not a regular file: ' + str(path))
    result = {'size': before.st_size, 'sha256': native.file_digest(path)}
    after = path.stat()
    materials.meta.require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                           (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
                           'An original Rust input changed while collecting it')
    return result


def toolchain_identity(toolchain, host, output):
    evidence = output / 'evidence'; evidence.mkdir()
    verbose = native.run_logged(['rustc', '+' + toolchain, '-Vv'], output, evidence, 'rustc-version')
    fields = {}
    for line in verbose.splitlines()[1:]:
        key, separator, value = line.partition(': ')
        materials.meta.require(separator and key not in fields, 'Unexpected actual rustc verbose identity')
        fields[key] = value
    materials.meta.require(verbose.startswith('rustc ' + toolchain + ' ') and fields.get('release') == toolchain and
                           fields.get('host') == host and re.fullmatch(r'[0-9a-f]{40}', fields.get('commit-hash', '')),
                           'Actual compiler version, full commit or host differs from the native receipt')
    sysroot = materials.real_directory(Path(native.run_logged(
        ['rustc', '+' + toolchain, '--print', 'sysroot'], output, evidence, 'rustc-sysroot').strip()))
    target = materials.real_directory(Path(native.run_logged(
        ['rustc', '+' + toolchain, '--print', 'target-libdir', '--target', host], output, evidence, 'rustc-target-libdir').strip()))
    compiler = Path(native.run_logged(['rustup', 'which', '--toolchain', toolchain, 'rustc'],
                                      output, evidence, 'rustup-which-rustc').strip()).resolve(strict=True)
    suffix = '.exe' if host.endswith('windows-msvc') else ''
    materials.meta.require(compiler == sysroot / ('bin/rustc' + suffix) and
                           target == sysroot / 'lib/rustlib' / host / 'lib',
                           'The actual Rust compiler/target directory is outside its selected official sysroot')
    native.run_logged(['rustup', 'component', 'list', '--toolchain', toolchain, '--installed'],
                      output, evidence, 'rustup-installed-components')
    return {'rustVersion': toolchain, 'host': host, 'commit': fields['commit-hash'],
            'verboseFields': fields, 'sysroot': sysroot, 'targetLibdir': target, 'compiler': compiler}


def distribution_specifications(identity, manifest):
    materials.meta.require(manifest.get('manifest-version') == '2' and
                           re.fullmatch(r'\d{4}-\d{2}-\d{2}', manifest.get('date', '')),
                           'Unsupported original rustup channel manifest')
    version, host, result = identity['rustVersion'], identity['host'], []
    for family in ('rust-src', 'rust-std', 'rustc'):
        package = manifest['pkg'][family]
        materials.meta.require(re.match(r'^(?:rustc )?' + re.escape(version) + r'(?: |$)', package['version']),
                               'Rust component version differs from the actual compiler: ' + family)
        declared_commit = package.get('git_commit_hash')
        if declared_commit is not None:
            materials.meta.require(declared_commit == identity['commit'], 'Rust component manifest commit differs')
        target = package['target']['*' if family == 'rust-src' else host]
        name = family + '-' + version + ('' if family == 'rust-src' else '-' + host)
        url, digest = target.get('xz_url', ''), target.get('xz_hash', '')
        parsed = urlsplit(url)
        materials.meta.require(target.get('available') is True and
                               re.fullmatch(r'[0-9a-f]{64}', digest) and parsed.scheme == 'https' and
                               parsed.netloc == 'static.rust-lang.org' and not parsed.query and not parsed.fragment and
                               parsed.path == '/dist/' + manifest['date'] + '/' + name + '.tar.xz',
                               'Expected an available, exact official dated Rust distribution with its original SHA256')
        result.append({'family': family, 'package': name, 'component': family + ('-' + host if family == 'rust-std' else ''),
                       'manifestVersion': package['version'], 'url': url, 'sha256': digest})
    return result


def original_notice(relative):
    name = PurePosixPath(relative).name
    return (bool(re.match(r'(?i)^(?:licen[cs]e|copying|copyright|notice|unlicen[cs]e)(?:[._-].*)?$', name)) or
            'LICENSES' in PurePosixPath(relative).parts or relative.startswith('share/doc/rust/licenses/'))


def compiler_notice(relative):
    path = PurePosixPath(relative)
    return (relative.startswith('share/doc/rust/licenses/') or
            (path.parent.as_posix() == 'share/doc/rust' and original_notice(relative)))


def notice_destination(output, family, archive_relative):
    # Runtime notices do not need the source distribution's deeply nested
    # layout. Retain the original basename and bind its full archive path in
    # the report; compact, distinct directories also fit ordinary Windows
    # installation paths without requiring a machine-wide long-path policy.
    key = hashlib.sha256(archive_relative.encode('utf-8')).hexdigest()[:16]
    return output / 'notices/rust-toolchain' / family / key / PurePosixPath(archive_relative).name


def stream_member(stream, size, destination=None):
    digest, length = hashlib.sha256(), 0
    outgoing = None
    if destination is not None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        outgoing = destination.open('xb')
    try:
        while chunk := stream.read(1024 * 1024):
            length += len(chunk)
            materials.meta.require(length <= size, 'A Rust distribution member exceeded its declared size')
            digest.update(chunk)
            if outgoing is not None:
                outgoing.write(chunk)
    finally:
        if outgoing is not None:
            outgoing.close()
    materials.meta.require(length == size, 'A Rust distribution member was truncated')
    return {'size': length, 'sha256': digest.hexdigest()}


def real_paths(folder):
    result = set()
    for path in folder.rglob('*'):
        materials.meta.require(not path.is_symlink(), 'An installed Rust component contains an unexpected source link')
        if path.is_dir():
            continue
        materials.meta.require(path.is_file(), 'An installed Rust component contains a special file')
        result.add(path.relative_to(folder).as_posix())
    return result


def verify_distribution(spec, archive, identity, output, source_output):
    """Read the official installer archive; never extract or execute it."""
    family, root, component = spec['family'], spec['package'], spec['component']
    sysroot, host = identity['sysroot'], identity['host']
    source_prefix = 'lib/rustlib/src/rust/'
    target_prefix = 'lib/rustlib/' + host + '/lib/'
    compiler_relative = identity['compiler'].relative_to(sysroot).as_posix()
    seen, source_names, target_names = set(), set(), set()
    inputs, notices, metadata = [], [], {}
    metadata_directory = source_output / 'distribution-metadata' / family
    total, compiler_seen = 0, False
    with tarfile.open(archive, mode='r|xz') as package:
        for member in package:
            materials.meta.require(len(seen) < 200000, 'The Rust distribution member count exceeded its budget')
            name = member.name.rstrip('/')
            if name == root:
                materials.meta.require(member.isdir(), 'The Rust archive root is not a directory')
                continue
            materials.meta.require(name.startswith(root + '/'), 'The Rust archive has an unexpected package root')
            relative = materials.meta.safe_relative(name[len(root) + 1:])
            materials.meta.require(relative not in seen, 'Duplicate Rust archive member: ' + relative); seen.add(relative)
            if member.isdir():
                continue
            materials.meta.require(member.isfile() and 0 <= member.size <= 512 * 1024 * 1024,
                                   'Rust component contains a link, special file or oversized member')
            total += member.size
            materials.meta.require(total <= 4 * 1024**3, 'Uncompressed Rust distribution exceeded its budget')
            payload = relative[len(component) + 1:] if relative.startswith(component + '/') else None
            is_root_metadata = '/' not in relative
            verify_path, installed_relative = None, None
            if family == 'rust-src' and payload and payload.startswith(source_prefix):
                installed_relative = payload
                source_names.add(payload[len(source_prefix):])
            elif family == 'rust-std' and payload and payload.startswith(target_prefix):
                installed_relative = payload
                target_names.add(payload[len(target_prefix):])
            elif family == 'rustc' and payload:
                if payload == compiler_relative:
                    installed_relative, compiler_seen = payload, True
                elif compiler_notice(payload):
                    installed_relative = payload
            if installed_relative:
                verify_path = sysroot / materials.meta.safe_relative(installed_relative)
                materials.meta.require(verify_path.is_file(), 'The actual rustup component input is missing: ' + installed_relative)
            notice = original_notice(payload if payload is not None else relative)
            destination = None
            if is_root_metadata:
                materials.meta.require(member.size <= 32 * 1024 * 1024, 'Oversized original distribution metadata')
                destination = metadata_directory / relative
            elif notice:
                destination = notice_destination(output, family, relative)
            incoming = package.extractfile(member)
            materials.meta.require(incoming is not None, 'The official Rust archive member cannot be read')
            with incoming:
                digest = stream_member(incoming, member.size, destination)
            if verify_path is not None:
                materials.meta.require(ordinary_snapshot(verify_path) == digest,
                                       'Installed Rust component differs from its official archive: ' + installed_relative)
                inputs.append({'path': installed_relative, **digest})
            if is_root_metadata:
                metadata[relative] = {'path': destination.relative_to(source_output).as_posix(), **digest}
                if notice:
                    target = notice_destination(output, family, relative)
                    materials.copy_original(destination, target)
                    notices.append({'path': target.relative_to(output).as_posix(), 'archivePath': relative, **digest})
            elif notice:
                notices.append({'path': destination.relative_to(output).as_posix(), 'archivePath': relative, **digest})
    materials.meta.require({'version', 'git-commit-hash', 'COPYRIGHT', 'LICENSE-APACHE', 'LICENSE-MIT'} <= metadata.keys(),
                           'The original Rust component lacks full version/commit/license overlay evidence')
    commit = (metadata_directory / 'git-commit-hash').read_text(encoding='utf-8').strip()
    version = (metadata_directory / 'version').read_text(encoding='utf-8').strip()
    materials.meta.require(commit == identity['commit'] and
                           re.match(r'^(?:rustc )?' + re.escape(identity['rustVersion']) + r'(?: |$)', version),
                           'Official source, compiler and target archive commits/versions differ')
    if family == 'rust-src':
        materials.meta.require(source_names and {'library/core/src/lib.rs', 'library/std/src/lib.rs',
                                                'library/.cargo/config.toml'} <= source_names,
                               'Official rust-src is missing the standard library or its original vendored build config')
        source_root = materials.real_directory(sysroot / source_prefix)
        materials.meta.require(real_paths(source_root) == source_names,
                               'Installed rust-src contains missing or extra files relative to the official source archive')
        materials.meta.require(any(name.startswith('src/llvm-project/libunwind/') for name in source_names),
                               'The actual standard-library source archive lacks libunwind source')
        materials.meta.json_write(source_output / 'rust-src-files.json', {'schemaVersion': 1, 'files': inputs})
    elif family == 'rust-std':
        materials.meta.require(target_names and any(name.startswith('libstd-') and name.endswith('.rlib') for name in target_names),
                               'No actual target standard library was verified')
        materials.meta.require(real_paths(identity['targetLibdir']) == target_names,
                               'The actual target library directory has missing or unattributed files')
    else:
        checked = {item['path'] for item in inputs}
        materials.meta.require(compiler_seen and {'share/doc/rust/COPYRIGHT.html', 'share/doc/rust/COPYRIGHT-library.html'} <= checked and
                               any(path.startswith('share/doc/rust/licenses/') for path in checked),
                               'The actual compiler or its complete upstream notice families are missing')
        documentation = sysroot / 'share/doc/rust'
        installed_notices = {'share/doc/rust/' + path.name for path in documentation.iterdir()
                             if path.is_file() and compiler_notice('share/doc/rust/' + path.name)}
        installed_notices.update('share/doc/rust/licenses/' + path
                                 for path in real_paths(documentation / 'licenses'))
        materials.meta.require(installed_notices <= checked, 'Some original installed Rust notices lack archive byte attribution')
    for item in inputs:
        materials.meta.require(ordinary_snapshot(sysroot / item['path']) == {key: item[key] for key in ('size', 'sha256')},
                               'An actual Rust input changed after archive verification')
    return {'family': family, 'package': root, 'component': component, 'commit': commit,
            'archive': {'path': archive.relative_to(output).as_posix(), 'location': 'verification-output',
                        'url': spec['url'], **materials.unchanged_digest(archive)},
            'metadata': metadata, 'notices': notices, 'verifiedInputCount': len(inputs),
            'verifiedInputs': inputs if family != 'rust-src' else {'inventory': 'rust-src-files.json', 'count': len(inputs)}}


def collect(options):
    # Locate original inputs before creating output directories or command logs.
    # Even an accidentally supplied sysroot subdirectory must remain read-only.
    receipt_path = options.native_receipt.resolve(strict=True) if options.native_receipt else None
    receipt = materials.meta.json_read(receipt_path) if receipt_path else None
    library = (options.native_library or Path(receipt['library'])).resolve(strict=True) if receipt else None
    if receipt:
        lock, _, _ = native.verify_build_inputs(materials, receipt, library)
        host = receipt['host']
    else:
        materials.meta.require(options.prepare_for_build and options.native_library is None,
                               'A pre-build collection has no aggregate library to certify')
        lock = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']
        verbose = materials.command('rustc', '+' + lock['rustToolchain'], '-Vv')
        matches = re.findall(r'^host: (x86_64-(?:pc-windows-msvc|unknown-linux-gnu))$', verbose, re.MULTILINE)
        materials.meta.require(len(matches) == 1, 'Expected the actual supported native Rust host')
        host = matches[0]
    sysroot_probe = materials.real_directory(Path(materials.command('rustc', '+' + lock['rustToolchain'], '--print', 'sysroot')))
    materials.meta.require(not options.output.resolve().is_relative_to(sysroot_probe) and
                           not options.source_output.resolve().is_relative_to(sysroot_probe),
                           'Material collection cannot write to the original Rust sysroot')
    output, source_output = materials.new_output(options.output), materials.new_output(options.source_output)
    materials.meta.require(not output.is_relative_to(source_output) and not source_output.is_relative_to(output),
                           'Keep full Rust sources separate from runtime notices/evidence')
    report = {'schemaVersion': 1, 'scope': 'Actual locked rustup source, target standard library and compiler notice bytes',
              'standardLibraryInputBindingComplete': False, 'standardLibrarySourceMaterialsCollected': False,
              'upstreamNoticeSetComplete': False, 'correspondingSourceComplete': False,
              'aggregateBuildBound': False, 'preparedBeforeAggregateBuild': receipt is None,
              'compilerSourceStatus': 'not-collected; rust-src is the standard-library source distribution',
              'components': [], 'failures': []}
    try:
        identity = toolchain_identity(lock['rustToolchain'], host, output)
        sysroot = identity['sysroot']
        materials.meta.require(sysroot == sysroot_probe, 'The actual Rust toolchain changed during collection')
        manifest_path = sysroot / 'lib/rustlib/multirust-channel-manifest.toml'
        materials.meta.require(manifest_path.is_file() and not manifest_path.is_symlink() and manifest_path.stat().st_size <= 8 * 1024 * 1024,
                               'The actual rustup channel manifest is missing or oversized')
        manifest_bytes = manifest_path.read_bytes()
        manifest = tomllib.loads(manifest_bytes.decode('utf-8'))
        specifications = distribution_specifications(identity, manifest)
        materials.meta.require((sysroot / 'lib/rustlib/src/rust/library/.cargo/config.toml').is_file(),
                               'Install the locked official rust-src component with rustup before collection')
        originals = {'dependency-lock.json': ROOT / 'build/dependencies.lock.json',
                     'multirust-channel-manifest.toml': manifest_path}
        if receipt_path:
            originals['native-build-info.json'] = receipt_path
        for name, original in originals.items():
            materials.copy_original(original, output / 'evidence' / name)
            materials.copy_original(original, source_output / name)
        distribution_directory = output / 'evidence/distributions'; distribution_directory.mkdir()
        for spec in specifications:
            print('Verifying official Rust distribution: ' + spec['package'], flush=True)
            archive = distribution_directory / (spec['package'] + '.tar.xz')
            # Reuse the project's existing bounded HTTPS/SHA256 downloader.
            # The digest comes from the actual locked rustup manifest, which
            # is preserved verbatim; no dependency resolution occurs here.
            downloads.download(spec['url'], archive, spec['sha256'], 512 * 1024 * 1024)
            result = verify_distribution(spec, archive, identity, output, source_output)
            report['components'].append(result)
            if spec['family'] == 'rust-src':
                result['retainedSourceArchive'] = {'path': archive.name, 'location': 'source-output',
                                                  **materials.copy_original(archive, source_output / archive.name)}
        if receipt:
            native.verify_build_inputs(materials, receipt, library)
            materials.meta.require(materials.unchanged_digest(receipt_path) ==
                                   materials.unchanged_digest(output / 'evidence/native-build-info.json'),
                                   'Original native build evidence changed during source collection')
            report['nativeBuild'] = {'receiptSha256': native.file_digest(receipt_path),
                                    'aggregateSha256': receipt['librarySha256'],
                                    'cargoLockSha256': receipt['cargoLockSha256']}
            report['aggregateBuildBound'] = True
        materials.meta.require(manifest_path.read_bytes() == manifest_bytes,
                               'Original toolchain manifest changed during source collection')
        report['toolchain'] = {key: str(value) if isinstance(value, Path) else value for key, value in identity.items()}
        report['originalManifest'] = {'sha256': hashlib.sha256(manifest_bytes).hexdigest(), 'date': manifest['date']}
        report['standardLibraryInputBindingComplete'] = True
        report['standardLibrarySourceMaterialsCollected'] = True
        report['upstreamNoticeSetComplete'] = True
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, tarfile.TarError, subprocess.SubprocessError) as error:
        report['failures'].append(str(error))
        raise
    finally:
        materials.meta.json_write(output / 'rust-toolchain-materials.json', report)
        materials.meta.json_write(source_output / 'rust-toolchain-sources.json', report)
    print('Verified original rust-src, actual target standard-library files and the compiler notice set.')
    print('This component result does not establish complete corresponding source for the combined application.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    phase = parser.add_mutually_exclusive_group(required=True)
    phase.add_argument('--native-receipt', type=Path, help='Original same-build native-rust/build-info.json')
    phase.add_argument('--prepare-for-build', action='store_true',
                       help='Verify the installed official toolchain before building; does not certify an aggregate')
    parser.add_argument('--native-library', type=Path, help='Explicit relocated aggregate with the exact original receipt hash')
    parser.add_argument('--output', type=Path, required=True, help='Empty runtime notice and verification evidence directory')
    parser.add_argument('--source-output', type=Path, required=True, help='Separate empty full-source archive directory')
    options = parser.parse_args()
    try:
        collect(options)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, tarfile.TarError, subprocess.SubprocessError) as error:
        print('Rust toolchain material collection failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
