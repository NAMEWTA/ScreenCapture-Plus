"""Retain Cargo's real locked vendor tree and its unmodified license texts."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tomllib


def file_digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def graph_identity(api, metadata):
    """Compare Cargo graphs across runner paths without rewriting evidence."""
    packages = metadata.get('packages', [])
    api.meta.require(packages and len(packages) <= 2048 and isinstance(metadata.get('resolve'), dict),
                     'Missing or oversized actual Cargo metadata graph')
    keys, result = {}, {}
    for package in packages:
        key = (package['name'], package['version'], package.get('source') or 'workspace-path')
        api.meta.require(key not in result and package['id'] not in keys, 'Ambiguous actual Cargo package identity')
        keys[package['id']] = key
        result[key] = {'license': package.get('license'), 'licenseFile': package.get('license_file'),
                       'authors': package.get('authors', []), 'repository': package.get('repository'),
                       'description': package.get('description')}
    nodes = {}
    for node in metadata['resolve'].get('nodes', []):
        key = keys[node['id']]
        api.meta.require(key not in nodes, 'Duplicate actual Cargo resolution node')
        dependencies = sorted((entry['name'], keys[entry['pkg']],
                               tuple(sorted((kind.get('kind') or '', kind.get('target') or '')
                                            for kind in entry['dep_kinds']))) for entry in node['deps'])
        nodes[key] = {'features': sorted(node['features']), 'dependencies': dependencies}
    api.meta.require(nodes.keys() == result.keys(), 'Cargo package and resolved node inventories differ')
    return {'packages': result, 'nodes': nodes,
            'members': sorted(keys[key] for key in metadata.get('workspace_members', [])),
            'defaults': sorted(keys[key] for key in metadata.get('workspace_default_members', []))}


def verify_build_inputs(api, receipt, library):
    native_build = api.module('huhucapture_actual_native_build', api.ROOT / 'scripts/build-native-rust.py')
    lock = api.meta.json_read(api.ROOT / 'build/dependencies.lock.json')['qtMigration']
    api.meta.require(receipt.get('schemaVersion') == 1 and receipt.get('crate') == lock['nativeRust']['package'] and
                     receipt.get('rustVersion') == lock['rustToolchain'] and receipt.get('bootstrapResolution') is False and
                     receipt.get('host') in ('x86_64-unknown-linux-gnu', 'x86_64-pc-windows-msvc'),
                     'Native build receipt does not describe the locked production aggregate')
    fingerprints = native_build.source_fingerprints(api.ROOT)
    api.meta.require(fingerprints == receipt.get('sourceFiles'), 'Native build receipt source fingerprints differ')
    cargo_lock = api.ROOT / lock['nativeRust']['workspaceManifest']
    cargo_lock = cargo_lock.with_name('Cargo.lock')
    api.meta.require(file_digest(cargo_lock) == receipt.get('cargoLockSha256'), 'Actual native Cargo.lock differs')
    api.meta.require(api.unchanged_digest(library)['sha256'] == receipt.get('librarySha256'),
                     'The actual linked aggregate bytes differ from their original receipt')
    return lock, fingerprints, cargo_lock


def checked_vendor(api, directory, locked, metadata, output):
    """Cargo supplies/normalizes files; its checksums establish their content."""
    locked_by_name = {}
    for package in locked['package']:
        if package.get('source'):
            locked_by_name.setdefault((package['name'], package['version']), []).append(package)
    actual = {(entry['name'], entry['version'], entry.get('source')): entry
              for entry in metadata['packages'] if entry.get('source')}
    seen, records, missing_notices = set(), [], []
    directories = sorted(directory.iterdir())
    api.meta.require(0 < len(directories) <= 2048, 'Cargo produced an empty or oversized vendor inventory')
    for crate in directories:
        api.meta.require(crate.is_dir() and not crate.is_symlink(), 'Cargo vendor contains a non-directory or linked crate')
        manifest = tomllib.loads((crate / 'Cargo.toml').read_text(encoding='utf-8'))['package']
        name, version = manifest['name'], manifest['version']
        choices = locked_by_name.get((name, version), [])
        api.meta.require(len(choices) == 1, 'Vendored crate has ambiguous or absent lock identity: ' + crate.name)
        locked_package = choices[0]
        source = locked_package['source']
        api.meta.require((source.startswith('registry+') and
                          re.fullmatch(r'[0-9a-f]{64}', locked_package.get('checksum', ''))) or
                         (source.startswith('git+') and re.search(r'#[0-9a-f]{40}$', source) and
                          'checksum' not in locked_package), 'Cargo source is not a checksum/commit-locked original')
        key = (name, version, locked_package['source'])
        api.meta.require(key not in seen, 'Duplicate locked vendored crate'); seen.add(key)
        checksum_path = crate / '.cargo-checksum.json'
        checksum = api.meta.json_read(checksum_path, 16 * 1024 * 1024)
        api.meta.require(checksum.get('package') == locked_package.get('checksum'),
                         'Cargo registry checksum differs from the original lock: ' + crate.name)
        contents = checksum.get('files')
        api.meta.require(isinstance(contents, dict) and 0 < len(contents) <= 100000,
                         'Missing or oversized Cargo per-file checksum inventory')
        present = set()
        for path in crate.rglob('*'):
            api.meta.require(not path.is_symlink(), 'A vendored source link needs explicit retention review')
            if path.is_dir():
                continue
            api.meta.require(path.is_file(), 'Cargo vendor contains a special source file')
            relative = api.meta.safe_relative(path.relative_to(crate).as_posix())
            if relative != '.cargo-checksum.json':
                present.add(relative)
        api.meta.require(present == set(contents), 'Vendored source files differ from Cargo checksum inventory: ' + crate.name)
        for relative, expected in contents.items():
            path = crate / api.meta.safe_relative(relative)
            api.meta.require(re.fullmatch(r'[0-9a-f]{64}', expected) and file_digest(path) == expected,
                             'Vendored source checksum mismatch: ' + crate.name + '/' + relative)
        package = actual.get(key)
        declaration = package.get('license') if package else manifest.get('license')
        declared_file = package.get('license_file') if package else manifest.get('license-file')
        notice_paths = {path for path in present if re.match(r'(?i)^(?:licen[cs]e|copying|copyright|notice|unlicen[cs]e)(?:[._-].*)?$',
                                                            Path(path).name)}
        if declared_file:
            relative = api.meta.safe_relative(declared_file)
            api.meta.require(relative in present, 'The declared crate license file was not vendored')
            notice_paths.add(relative)
        notices = []
        for relative in sorted(notice_paths):
            path = Path('notices/rust/crates') / crate.name / relative
            original = crate / relative
            api.meta.require(original.stat().st_size > 0, 'The original crate notice is empty: ' + str(original))
            notices.append({'source': relative, 'path': path.as_posix(), **api.copy_original(original, output / path)})
        if not notices:
            missing_notices.append({'name': name, 'version': version, 'source': locked_package['source'],
                                    'declaredLicense': declaration})
        records.append({'name': name, 'version': version, 'source': locked_package['source'],
                        'registryChecksum': locked_package.get('checksum'),
                        'presentInBuildMetadata': package is not None,
                        'declaredLicense': declaration, 'declaredLicenseFile': declared_file,
                        'authors': package.get('authors', []) if package else manifest.get('authors', []),
                        'repository': package.get('repository') if package else manifest.get('repository'),
                        'vendorDirectory': crate.name, 'sourceFileCount': len(contents),
                        'cargoChecksumFile': api.unchanged_digest(checksum_path), 'notices': notices})
    api.meta.require(set(actual) <= seen, 'Some packages in the real native build graph were not vendored')
    return records, missing_notices


def run_logged(arguments, directory, evidence, name):
    environment = os.environ.copy(); environment['LC_ALL'] = 'C.UTF-8'
    # All network/source work is Cargo's normal locked operation. Preserve its
    # diagnostics and exact stdout, including the usable generated config.
    with (evidence / (name + '.stderr.log')).open('xb') as errors:
        result = subprocess.run(list(map(str, arguments)), cwd=directory, env=environment,
                                stdout=subprocess.PIPE, stderr=errors)
    (evidence / (name + '.stdout')).write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError('Official Cargo/Rust command failed: ' + name + '; original logs retained')
    return result.stdout.decode('utf-8', errors='strict')


def collect(api, options):
    output, source_output = api.new_output(options.output), api.new_output(options.source_output)
    api.meta.require(not output.is_relative_to(source_output) and not source_output.is_relative_to(output),
                     'Runtime notices and full vendored source need separate owned outputs')
    receipt_path, metadata_path = options.native_receipt.resolve(strict=True), options.cargo_metadata.resolve(strict=True)
    receipt = api.meta.json_read(receipt_path)
    original_metadata = api.meta.json_read(metadata_path, 32 * 1024 * 1024)
    library = (options.native_library or Path(receipt['library'])).resolve(strict=True)
    report = {'schemaVersion': 1, 'scope': 'Actual native aggregate receipt and Cargo locked vendor source',
              'correspondingSourceComplete': False, 'vendorSourceComplete': False,
              'licenseTextCoverageComplete': False, 'rustStandardLibrarySourceStatus': 'not-collected',
              'packages': [], 'failures': []}
    try:
        lock, fingerprints, cargo_lock = verify_build_inputs(api, receipt, library)
        evidence = output / 'evidence'; evidence.mkdir()
        for path, name in ((receipt_path, 'original-native-build-info.json'),
                           (metadata_path, 'original-cargo-metadata.json'), (cargo_lock, 'original-Cargo.lock')):
            api.copy_original(path, evidence / name)
            api.copy_original(path, source_output / name)
        toolchain = lock['rustToolchain']
        version = run_logged(['rustc', '+' + toolchain, '--version', '--verbose'], source_output, evidence, 'rustc-version')
        api.meta.require(version.startswith('rustc ' + toolchain + ' ') and
                         ('host: ' + receipt['host']) in version.splitlines(), 'Actual Rust toolchain/host differs from build receipt')
        cargo = ['cargo', '+' + toolchain, '--color', 'never']
        manifest = api.ROOT / lock['nativeRust']['workspaceManifest']
        actual_metadata_text = run_logged(cargo + ['metadata', '--format-version', '1', '--locked',
                                                   '--manifest-path', manifest], manifest.parent, evidence, 'cargo-metadata')
        metadata = json.loads(actual_metadata_text)
        api.meta.require(graph_identity(api, metadata) == graph_identity(api, original_metadata),
                         'Cargo actual resolved graph differs from the original native build')
        vendor_text = run_logged(cargo + ['vendor', '--locked', '--versioned-dirs', '--manifest-path', manifest, 'vendor'],
                                 source_output, evidence, 'cargo-vendor')
        config = tomllib.loads(vendor_text)
        api.meta.require(config.get('source', {}).get('vendored-sources', {}).get('directory') == 'vendor',
                         'Cargo generated an unexpected source destination; do not rewrite its configuration')
        (source_output / '.cargo').mkdir()
        (source_output / '.cargo/config.toml').write_text(vendor_text, encoding='utf-8', newline='')
        report['packages'], missing = checked_vendor(api, source_output / 'vendor',
                                                    tomllib.loads(cargo_lock.read_text(encoding='utf-8')), metadata, output)
        report['missingCrateNoticeTexts'] = missing
        for relative, expected in fingerprints.items():
            retained = source_output / 'workspace' / api.meta.safe_relative(relative)
            api.meta.require(api.copy_original(api.ROOT / relative, retained)['sha256'] == expected,
                             'The native workspace changed while retaining corresponding source')
        sysroot = Path(run_logged(['rustc', '+' + toolchain, '--print', 'sysroot'], source_output, evidence, 'rustc-sysroot').strip())
        toolchain_notices = []
        for path in sorted((sysroot / 'share/doc/rust').glob('*')):
            if path.is_file() and re.match(r'(?i)^(?:license|copyright|copying|notice)', path.name):
                target = Path('notices/rust/toolchain') / path.name
                toolchain_notices.append({'path': target.as_posix(), **api.copy_original(path, output / target)})
        report['toolchainNoticeFiles'] = toolchain_notices
        report['toolchainNoticeStatus'] = 'collected-originals' if toolchain_notices else 'not-present-in-installed-toolchain'
        verify_build_inputs(api, receipt, library)
        api.meta.require(api.unchanged_digest(receipt_path) == api.unchanged_digest(evidence / 'original-native-build-info.json') and
                         api.unchanged_digest(metadata_path) == api.unchanged_digest(evidence / 'original-cargo-metadata.json'),
                         'Original native evidence changed while collecting sources')
        report['nativeBuild'] = {'receiptSha256': file_digest(receipt_path), 'cargoMetadataSha256': file_digest(metadata_path),
                                 'librarySha256': receipt['librarySha256'], 'cargoLockSha256': receipt['cargoLockSha256'],
                                 'rustVersion': toolchain, 'host': receipt['host'], 'sourceFiles': fingerprints}
        report['vendorSourceComplete'] = True
        report['licenseTextCoverageComplete'] = not missing and bool(toolchain_notices)
        report['resolvedBuildPackageCount'] = len(metadata['packages'])
        report['scopeNote'] = 'Cargo metadata/vendor include target and build dependencies; this is not a linked-symbol inventory.'
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report['failures'].append(str(error))
        raise
    finally:
        api.meta.json_write(output / 'rust-materials.json', report)
        api.meta.json_write(source_output / 'rust-sources.json', report)
    print(f'Retained {len(report["packages"])} locked vendored crates with checked source bytes and original notices.')
    print('Rust standard-library source and combined release obligations remain independent, incomplete gates.')
