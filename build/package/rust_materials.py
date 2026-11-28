"""Retain Cargo's real locked vendor tree and its unmodified license texts."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tomllib


# Only this reviewed eight-file publication may use the generated-Maven
# binding. Updating the crate, any file classification, or either upstream
# identity requires another source review, not a plan-provided exception.
ANDROID_NOTICE_BINDING = 'rustls-android-0.1.1-authored-and-archive-match'
ANDROID_NOTICE_PROFILE_SHA256 = '626da512f0af24ae2ef41d30e6a70ddcf414a4ae4b9b54f331b7b5117b3c6217'


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


def original_notice_reviews(api):
    """Read the small audited set of notices omitted by these exact crates."""
    path = api.ROOT / 'third_party/rust-notices/upstream.json'
    identity = api.unchanged_digest(path)
    value = api.meta.json_read(path, 256 * 1024)
    api.meta.require(api.unchanged_digest(path) == identity, 'Original crate notice review changed while reading')
    api.meta.require(value.get('schemaVersion') == 1 and value.get('correspondingSourceComplete') is False and
                     isinstance(value.get('crates'), list) and len(value['crates']) <= 64,
                     'Unsupported original crate notice review')
    entries = {}
    for entry in value['crates']:
        key = (entry['name'], entry['version'])
        api.meta.require(key not in entries and entry.get('notices'), 'Duplicate or empty original crate notice review')
        entries[key] = entry
    return path, identity, entries


def reviewed_android_publication(api, crate, review):
    """Bind separately retained notices to one unchanged original publication.

    Two authored files match the source commit. Cargo generated one manifest;
    Maven generated five files, including the AAR found byte-for-byte in the
    later official archive. Neither match establishes the original build or
    upload commit. The complete reviewed record, including those limitations,
    is fixed here so this cannot become a general generated-file exemption.
    """
    encoded = json.dumps(review, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()
    api.meta.require(hashlib.sha256(encoded).hexdigest() == ANDROID_NOTICE_PROFILE_SHA256,
                     'Android notice binding differs from the fixed original publication review')
    expected = {entry['path']: entry for entry in review['publishedFiles']}
    actual = list(crate.rglob('*'))
    api.meta.require(crate.is_dir() and not crate.is_symlink() and
                     not any(path.is_symlink() for path in actual) and
                     {path.relative_to(crate).as_posix() for path in actual if not path.is_dir()} ==
                     {*expected, '.cargo-checksum.json'},
                     'Android notice binding requires the unchanged eight-file publication')
    for name, entry in expected.items():
        path = crate / api.meta.safe_relative(name)
        api.meta.require(path.is_file() and path.resolve().is_relative_to(crate.resolve()) and
                         path.stat().st_size == entry['size'],
                         'Android published file is missing, linked or has changed size: ' + name)
        data = path.read_bytes()
        api.meta.require(hashlib.sha256(data).hexdigest() == entry['sha256'] and
                         hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest() ==
                         entry['gitBlobSha1'], 'Android published file bytes differ: ' + name)
    for name in ('Cargo.toml', 'Cargo.toml.orig'):
        package = tomllib.loads((crate / name).read_text(encoding='utf-8'))['package']
        api.meta.require(package.get('name') == review['name'] and package.get('version') == review['version'] and
                         package.get('license') == review['declaredLicense'] and
                         package.get('repository') == review['declaredRepository'] and
                         package.get('authors', []) == review['authors'],
                         'Android notice binding changes the original package declaration')
    return {'publicationGeneratedFiles': review['publicationGeneratedFiles'],
            'publishedFiles': review['publishedFiles'], 'upstreamTree': review['upstreamTree'],
            'upstreamLicenseLinks': review['upstreamLicenseLinks'], 'packagingRecipe': review['packagingRecipe'],
            'buildProvenanceRecorded': False, 'externalLicenseTexts': True,
            'fixedPublicationReviewSha256': ANDROID_NOTICE_PROFILE_SHA256,
            'licenseTextScope': review['licenseTextScope']}


def reviewed_crate_notices(api, crate, locked, declaration, checksum_path, review, review_identity, output):
    """Supplement notices without changing Cargo's immutable vendor directory.

    The reviewed Cargo checksum file binds the complete original file inventory,
    whose every byte was already checked above. It is deliberately stricter than
    matching a crate name/version or an SPDX license expression.
    """
    api.meta.require(review['source'] == locked['source'] and
                     review.get('registryChecksum') == locked.get('checksum') and
                     review['declaredLicense'] == declaration and
                     review['cargoChecksumFileSha256'] == api.unchanged_digest(checksum_path)['sha256'],
                     'Original crate notice review differs from the exact Cargo source: ' + crate.name)
    commit = review['upstreamCommit']
    api.meta.require(re.fullmatch(r'[0-9a-f]{40}', commit), 'Original crate notice review lacks an exact commit')
    binding, additional_provenance = review['bindingType'], {}
    if (review['name'], review['version']) == ('rustls-platform-verifier-android', '0.1.1'):
        api.meta.require(binding == ANDROID_NOTICE_BINDING,
                         'The fixed Android publication requires its dedicated notice binding type')
    if binding == 'cargo-vcs-and-authored-files-match':
        vcs = api.meta.json_read(crate / '.cargo_vcs_info.json', 16384)
        api.meta.require(vcs.get('git', {}).get('sha1') == commit and
                         vcs.get('path_in_vcs') == review['pathInVcs'] and
                         not vcs.get('git', {}).get('dirty', False),
                         'Original crate VCS identity differs from its notice review: ' + crate.name)
    elif binding == 'cargo-lock-git-revision-and-files-match':
        api.meta.require(locked['source'].startswith('git+') and locked['source'].endswith('#' + commit),
                         'Original Git crate revision differs from its notice review: ' + crate.name)
    elif binding == ANDROID_NOTICE_BINDING:
        additional_provenance = reviewed_android_publication(api, crate, review)
    else:
        api.meta.require(binding == 'all-published-files-match-upstream-commit' and
                         locked['source'].startswith('registry+') and review['publicationCommitRecorded'] is False,
                         'Unknown original crate notice binding: ' + crate.name)
    notices = []
    for entry in review['notices']:
        kind = entry['inputKind']
        api.meta.require(kind in {'cargo-vendor', 'project-original', 'standard-license'}, 'Unknown crate notice input')
        relative = api.meta.safe_relative(entry['inputPath'])
        api.meta.require(kind == 'cargo-vendor' or relative.startswith('third_party/'),
                         'Retained upstream notice must be a reviewed third_party original')
        base = crate if kind == 'cargo-vendor' else api.ROOT
        original = base / relative
        api.meta.require(original.is_file() and not original.is_symlink() and
                         original.resolve().is_relative_to(base.resolve()), 'Original crate notice escapes its input')
        expected = {key: entry[key] for key in ('size', 'sha256')}
        api.meta.require(0 < expected['size'] <= 1024 * 1024 and api.unchanged_digest(original) == expected,
                         'Retained original crate notice bytes differ: ' + crate.name + '/' + relative)
        data = original.read_bytes()
        git_blob = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        api.meta.require(git_blob == entry['gitBlobSha1'], 'Original crate notice Git blob differs: ' + crate.name)
        upstream = api.meta.safe_relative(entry['upstreamPath'])
        origin_repository, origin_commit = review['upstreamRepository'], commit
        if kind == 'standard-license':
            # This is an explicit choice from the exact crate's dual license,
            # not a claim that upstream supplied a missing license file. Keep
            # the original manifest/author bytes and the standard text distinct.
            api.meta.require(declaration == 'MIT OR Apache-2.0' and review.get('selectedLicense') == 'Apache-2.0' and
                             entry.get('spdxLicense') == 'Apache-2.0' and
                             entry.get('standardRepository') == 'https://github.com/spdx/license-list-data' and
                             re.fullmatch(r'[0-9a-f]{40}', entry.get('standardCommit', '')) and
                             upstream == 'text/Apache-2.0.txt', 'Unsupported explicit standard-license selection')
            source_manifest = review['sourceManifest']
            manifest_name = api.meta.safe_relative(source_manifest['path'])
            api.meta.require(manifest_name == 'Cargo.toml.orig', 'Standard license choice requires the original crate manifest')
            manifest_path = crate / manifest_name
            manifest_bytes = manifest_path.read_bytes()
            api.meta.require(api.unchanged_digest(manifest_path) ==
                             {key: source_manifest[key] for key in ('size', 'sha256')} and
                             hashlib.sha1(b'blob ' + str(len(manifest_bytes)).encode('ascii') + b'\0' +
                                          manifest_bytes).hexdigest() == source_manifest['gitBlobSha1'],
                             'Original license/author declaration differs from the reviewed source manifest')
            authored = tomllib.loads(manifest_bytes.decode('utf-8'))['package']
            api.meta.require(authored.get('name') == review['name'] and authored.get('version') == review['version'] and
                             authored.get('license') == declaration and authored.get('repository') == review['declaredRepository'] and
                             authored.get('authors', []) == review['authors'],
                             'Standard license choice changes the original crate license, authors or repository')
            origin_repository, origin_commit = entry['standardRepository'], entry['standardCommit']
            target = Path('notices/rust/crates') / crate.name / 'standard-license' / 'Apache-2.0.txt'
        else:
            target = Path('notices/rust/crates') / crate.name / upstream
        retained = api.copy_original(original, output / target)
        api.meta.require(retained == expected, 'Original crate notice changed while retaining: ' + crate.name)
        notices.append({'source': upstream, 'path': target.as_posix(), 'origin': kind,
                        'upstreamRepository': origin_repository, 'upstreamCommit': origin_commit,
                        'gitBlobSha1': git_blob, **retained})
    return notices, {'repository': review['upstreamRepository'], 'commit': commit, 'bindingType': binding,
                     'pathInVcs': review['pathInVcs'], 'matchedPublishedFileCount': review['matchedPublishedFileCount'],
                     'cargoGeneratedFiles': review['cargoGeneratedFiles'],
                     'publicationCommitRecorded': review['publicationCommitRecorded'],
                     'reviewFile': review_identity, 'cargoChecksumFileSha256': review['cargoChecksumFileSha256'],
                     'selectedLicense': review.get('selectedLicense'), 'authors': review.get('authors', []),
                     'sourceManifest': review.get('sourceManifest'), **additional_provenance,
                     'scope': 'Exact Cargo notice/source binding; standard-license selections are explicitly labeled; no combined-source closure'}


def checked_vendor(api, directory, locked, metadata, output):
    """Cargo supplies/normalizes files; its checksums establish their content."""
    locked_by_name = {}
    for package in locked['package']:
        if package.get('source'):
            locked_by_name.setdefault((package['name'], package['version']), []).append(package)
    actual = {(entry['name'], entry['version'], entry.get('source')): entry
              for entry in metadata['packages'] if entry.get('source')}
    review_path, review_identity, notice_reviews = original_notice_reviews(api)
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
        # REUSE-style crates retain SPDX-named texts such as LICENSES/MIT.txt.
        # They are original Cargo-verified files, not synthesized from a license expression.
        notice_paths = {path for path in present
                        if re.match(r'(?i)^(?:licen[cs]e|copying|copyright|notice|unlicen[cs]e)(?:[._-].*)?$',
                                    Path(path).name) or
                        any(part.casefold() in {'licenses', 'licences'} for part in Path(path).parts[:-1])}
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
        review = notice_reviews.get((name, version))
        provenance = None
        if review is not None:
            additional, provenance = reviewed_crate_notices(api, crate, locked_package, declaration,
                                                           checksum_path, review, review_identity, output)
            notices.extend(additional)
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
                        'cargoChecksumFile': api.unchanged_digest(checksum_path), 'notices': notices,
                        'supplementalNoticeReview': provenance})
    api.meta.require(set(actual) <= seen, 'Some packages in the real native build graph were not vendored')
    api.meta.require(api.unchanged_digest(review_path) == review_identity, 'Original crate notice review changed')
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
        review_path, review_identity, _ = original_notice_reviews(api)
        report['packages'], missing = checked_vendor(api, source_output / 'vendor',
                                                    tomllib.loads(cargo_lock.read_text(encoding='utf-8')), metadata, output)
        report['missingCrateNoticeTexts'] = missing
        # Additional originals live beside the full vendor tree. Never insert
        # them into crates or regenerate Cargo's checksum files.
        api.meta.require(api.unchanged_digest(review_path) == review_identity and
                         all(package['supplementalNoticeReview'] is None or
                             package['supplementalNoticeReview']['reviewFile'] == review_identity
                             for package in report['packages']),
                         'Original crate notice review changed during vendor collection')
        for destination in (evidence / 'original-crate-notice-review.json',
                            source_output / 'original-crate-notice-review.json'):
            api.meta.require(api.copy_original(review_path, destination) == review_identity,
                             'Original crate notice review changed while retaining')
        for package in report['packages']:
            for notice in package['notices']:
                if notice.get('origin') in {'project-original', 'standard-license'}:
                    expected = {key: notice[key] for key in ('size', 'sha256')}
                    relative = api.meta.safe_relative(notice['path'])
                    api.meta.require(api.copy_original(output / relative, source_output / relative) == expected,
                                     'Supplemental original crate notice changed before source retention')
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
