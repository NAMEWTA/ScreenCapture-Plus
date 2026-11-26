#!/usr/bin/env python3
"""Prepare exact Cargo and official Rust inputs before a recorded product build.

This command performs the explicit locked dependency fetch. Later source
validation and builds use the generated directory replacement offline.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import runpy
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
bundle = API['bundle']
retention = runpy.run_path(str(ROOT / 'scripts/retain-development-sources.py'))


def prepare(output):
    output = output.resolve()
    bundle.require(not output.exists(), 'Use a new native source preparation directory')
    output.mkdir(parents=True)
    log = API['ExecutionLog'](output / 'execution', 'windows' if os.name == 'nt' else 'linux', __file__)
    lock_path = ROOT / 'build/dependencies.lock.json'
    lock = bundle.read_json(lock_path)
    version = lock['qtMigration']['rustToolchain']
    manifest = ROOT / lock['qtMigration']['nativeRust']['workspaceManifest']
    commit = bundle.git(ROOT, 'rev-parse', 'HEAD').decode().strip()
    with tempfile.TemporaryDirectory(prefix='native-prepare-source-', dir=output) as temporary:
        _, project_files = bundle.export_git(ROOT, commit, Path(temporary) / 'source')
    log.retain_record(lock_path)
    log.retain_record(manifest.with_name('Cargo.lock'))
    programs = {}
    for name in ('cargo', 'rustc'):
        text, _ = log.run(['rustup', 'which', '--toolchain', version, name], cwd=ROOT, capture=True)
        programs[name] = str(Path(text.strip()).resolve(strict=True))
        actual, _ = log.run([programs[name], '--version', '--verbose'], cwd=ROOT, capture=True)
        bundle.require(actual.startswith(name + ' ' + version + ' '), 'Selected toolchain differs from the lock')
    cargo = [programs['cargo'], '--color', 'never']
    log.run(cargo + ['fetch', '--locked', '--manifest-path', str(manifest)], cwd=ROOT)
    metadata_text, _ = log.run(cargo + ['metadata', '--locked', '--offline', '--format-version', '1',
                                       '--manifest-path', str(manifest)], cwd=ROOT, capture=True)
    metadata = json.loads(metadata_text)
    metadata_path = output / 'cargo-metadata.json'
    API['write_new'](metadata_path, metadata)
    log.retain_record(metadata_path)
    cargo_home, registry = retention['registry_layout'](metadata)
    pin = lock['gifski']
    expected = 'git+' + pin['repository'] + '?rev=' + pin['commit'] + '#' + pin['commit']
    choices = [p for p in metadata['packages'] if (p.get('source') or '').startswith('git+')]
    bundle.require(len(choices) == 1 and choices[0]['source'] == expected and choices[0]['name'] == 'gifski',
                   'Expected the one locked original gifski Git dependency')
    original = Path(choices[0]['manifest_path']).parent.resolve(strict=True)
    source = output / 'gifski'
    log.run(bundle.git_command(ROOT, '-c', 'protocol.file.allow=always', 'clone', '--local',
                              '--no-hardlinks', '--dissociate', '--no-checkout', str(original), str(source)),
            cwd=ROOT, env=bundle.git_environment())
    log.run(bundle.git_command(source, '-c', 'core.autocrlf=false', 'checkout', '--detach', pin['commit']),
            cwd=ROOT, env=bundle.git_environment())
    with tempfile.TemporaryDirectory(prefix='gifski-source-', dir=output) as temporary:
        bundle.export_git(source, pin['commit'], Path(temporary) / 'source')
    vendor = output / 'vendor'
    generated, _ = log.run(cargo + ['vendor', '--locked', '--offline', '--versioned-dirs',
                                    '--manifest-path', str(manifest), str(vendor)], cwd=ROOT, capture=True)
    config = output / 'vendor-config.toml'
    with config.open('x', encoding='utf-8', newline='') as stream:
        stream.write(generated)
    log.retain_record(config)
    log.run(['rustup', 'component', 'add', 'rust-src', '--toolchain', version], cwd=ROOT)
    toolchain = output / 'toolchain-notices'
    toolchain_sources = output / 'toolchain-sources'
    log.run([sys.executable, ROOT / 'scripts/collect-rust-toolchain-materials.py', '--prepare-for-build',
             '--output', toolchain, '--source-output', toolchain_sources], cwd=ROOT)
    report_path = toolchain_sources / 'rust-toolchain-sources.json'
    report = bundle.read_json(report_path)
    bundle.require(report['preparedBeforeAggregateBuild'] and not report['aggregateBuildBound'] and
                   report['standardLibraryInputBindingComplete'] and report['standardLibrarySourceMaterialsCollected'],
                   'Official installed Rust inputs were not verified before this build')
    selected = [(i, item) for i, item in enumerate(report['components']) if item['family'] == 'rust-src']
    bundle.require(len(selected) == 1, 'Expected exactly one verified official Rust source distribution')
    index, standard = selected[0]
    source_archive = toolchain_sources / standard['retainedSourceArchive']['path']
    bundle.require(bundle.digest(source_archive) == standard['archive']['sha256'],
                   'Official Rust source archive changed after verification')
    plan = {'schemaVersion': 1, 'platform': 'windows' if os.name == 'nt' else 'linux',
            'project': str(ROOT), 'sources': [{'id': 'gifski', 'directory': str(source)},
                {'id': 'rust-toolchain-source', 'kind': 'tar', 'archive': str(source_archive),
                 'prefix': standard['package'], 'sha256': standard['archive']['sha256'],
                 'pinEvidence': {'path': str(report_path), 'sha256': bundle.digest(report_path),
                                 'pointer': '/components/' + str(index) + '/archive/sha256'}}],
            'rustVendor': {'directory': str(vendor), 'registryCache': str(registry), 'config': str(config),
                           'noticeReviews': 'third_party/rust-notices/upstream.json',
                           'replay': {**programs, 'cache': str(cargo_home)}},
            'materials': [{'id': 'rust-toolchain-inputs', 'path': str(report_path)},
                          {'id': 'native-prepared-metadata', 'path': str(metadata_path),
                           **API['identity'](metadata_path)}], 'buildReceipts': []}
    bundle.compare_checkout(ROOT, project_files, commit)
    record = log.raw_record({'sourceCommit': commit, 'sourceTreeSha256': bundle.tree_digest(project_files)},
                            {'vendorTreeSha256': bundle.tree_digest(bundle.inventory(vendor)),
                             'rustSourceArchive': bundle.descriptor(source_archive)})
    plan['materials'].append({'id': 'native-source-preparation', 'path': str(record)})
    path = output / 'source-plan.json'
    API['write_new'](path, plan)
    print(path)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    prepare(options.output)


if __name__ == '__main__':
    main()
