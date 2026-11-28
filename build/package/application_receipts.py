"""Verify complete product/Rust inputs before their actual build commands run."""
from __future__ import annotations

import os
from pathlib import Path
import runpy
import tempfile
import tomllib
import types

ROOT = Path(__file__).resolve().parents[2]
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
bundle = API['bundle']
rust_materials = runpy.run_path(str(ROOT / 'build/package/rust_materials.py'))
graph_api = types.SimpleNamespace(meta=types.SimpleNamespace(require=bundle.require))
REQUIRED = {'huhucapture', 'localsend', 'gifski', 'rust-vendor', 'rust-toolchain-source'}


class SourceInputs:
    def __init__(self, path, recorder):
        self.path = Path(path).resolve(strict=True)
        self.plan = bundle.read_json(self.path)
        self.plan_identity = API['identity'](self.path)
        self.recorder = recorder
        self.platform = 'windows' if os.name == 'nt' else 'linux'
        bundle.require(self.plan.get('schemaVersion') == 1 and self.plan.get('platform') == self.platform,
                       'Source plan must describe this actual native build platform')
        base = self.path.parent
        self.resolve = lambda value: (base / value).resolve() if not Path(value).is_absolute() else Path(value).resolve()
        bundle.require(self.resolve(self.plan['project']) == ROOT, 'Source plan refers to another project checkout')
        ids = [s['id'] for s in self.plan['sources']]
        bundle.require(len(ids) == len(set(ids)) and {'gifski', 'rust-toolchain-source'} <= set(ids),
                       'The native build needs distinct full gifski and Rust standard-library sources')
        plan = {key: self.plan[key] for key in ('schemaVersion', 'project', 'platform', 'rustVendor')}
        plan['sources'] = [s for s in self.plan['sources'] if s['id'] in REQUIRED]
        with tempfile.TemporaryDirectory(prefix='product-source-check-', dir=recorder.directory) as temporary:
            collector = bundle.Collector(plan, base, Path(temporary))
            collector.collect_project()
            for item in plan['sources']:
                collector.collect_source(item)
            collector.collect_rust()
            # No executable exists at this pre-build point. Every other source
            # gap remains a real error, including missing originals or replay.
            allowed = 'huhucapture: actual matching build-identity.json is missing'
            bundle.require(all(gap == allowed for gap in collector.gaps),
                           'Native source preparation is incomplete: ' + '; '.join(collector.gaps))
            bundle.require(set(collector.components) == REQUIRED, 'A complete native source component is missing')
            self.components = collector.components
            self.commit = collector.identity['sourceCommit']
            standard = collector.stage / collector.components['rust-toolchain-source']['path']
            self.rust_commit = (standard / 'git-commit-hash').read_text().strip()
        self.sources = [{'id': name, 'sourceTreeSha256': self.components[name]['sourceTreeSha256']}
                        for name in sorted(self.components)]
        self.config = self.resolve(plan['rustVendor']['config'])
        self.config_identity = API['identity'](self.config)
        declared = tomllib.loads(self.config.read_text(encoding='utf-8'))['source']['vendored-sources']['directory']
        bundle.require(Path(declared).is_absolute() and Path(declared).resolve() ==
                       self.resolve(plan['rustVendor']['directory']),
                       'Cargo must build the exact checked absolute vendor directory')
        self.standard = next(s for s in plan['sources'] if s['id'] == 'rust-toolchain-source')
        bundle.require(self.standard.get('kind') == 'tar', 'Use the verified official Rust source distribution')
        self.archive = self.resolve(self.standard['archive'])
        self.archive_identity = API['identity'](self.archive)
        reports = [m for m in self.plan.get('materials', []) if m['id'] == 'rust-toolchain-inputs']
        bundle.require(len(reports) == 1, 'The actual official toolchain verification report is required')
        self.report_path = self.resolve(reports[0]['path'])
        self.report_identity = API['identity'](self.report_path)
        self.toolchain = bundle.read_json(self.report_path)
        lock = bundle.read_json(ROOT / 'build/dependencies.lock.json')['qtMigration']
        report = self.toolchain
        bundle.require(report.get('standardLibraryInputBindingComplete') is True and
                       report.get('standardLibrarySourceMaterialsCollected') is True and
                       report.get('preparedBeforeAggregateBuild') is True and
                       report.get('aggregateBuildBound') is False and
                       report['toolchain']['commit'] == self.rust_commit and
                       report['toolchain']['rustVersion'] == lock['rustToolchain'],
                       'Prepared standard-library sources do not describe the actual locked compiler')
        self.sysroot = Path(report['toolchain']['sysroot']).resolve(strict=True)
        bundle.require(self.resolve(self.plan['rustVendor']['replay']['rustc']) ==
                       Path(report['toolchain']['compiler']).resolve(strict=True),
                       'Cargo replay and the actual standard-library report use different compilers')
        expected_host = 'x86_64-pc-windows-msvc' if self.platform == 'windows' else 'x86_64-unknown-linux-gnu'
        bundle.require(report['toolchain']['host'] == expected_host, 'Rust report belongs to another build host')
        self.runtime_inputs = []
        for item in report['components']:
            if item['family'] in ('rust-std', 'rustc'):
                bundle.require(isinstance(item['verifiedInputs'], list), 'Actual compiler/std input inventory is missing')
                self.runtime_inputs += item['verifiedInputs']
        bundle.require(self.runtime_inputs, 'Actual compiler/std inputs are not bound to their original archives')
        prepared = [m for m in self.plan.get('materials', []) if m['id'] == 'native-prepared-metadata']
        bundle.require(len(prepared) == 1, 'The original pre-vendor Cargo graph is required')
        self.prepared_metadata = self.resolve(prepared[0]['path'])
        self.prepared_metadata_identity = {key: prepared[0][key] for key in ('size', 'sha256')}
        bundle.require(API['identity'](self.prepared_metadata) == self.prepared_metadata_identity,
                       'The original source preparation graph changed')
        recorder.retain_record(Path(__file__).resolve())
        recorder.retain_record(self.path)
        recorder.retain_record(self.config)
        recorder.retain_record(self.report_path)
        recorder.retain_record(self.prepared_metadata)
        source_record = recorder.directory / 'source-inputs.json'
        API['write_new'](source_record, {'sourceCommit': self.commit, 'components': list(self.components.values())})
        recorder.retain_record(source_record)
        self.verify()

    def verify_metadata(self, actual):
        bundle.require(API['identity'](self.prepared_metadata) == self.prepared_metadata_identity and
                       rust_materials['graph_identity'](graph_api, actual) ==
                       rust_materials['graph_identity'](graph_api, bundle.read_json(self.prepared_metadata)),
                       'The actual vendored build graph differs from the original locked source preparation')

    def rust_environment(self):
        environment = os.environ.copy()
        jobs = environment.get('CARGO_BUILD_JOBS')
        for name in tuple(environment):
            if name.startswith('CARGO_') or name in ('RUSTC', 'RUSTDOC', 'RUSTC_WRAPPER',
                    'RUSTC_WORKSPACE_WRAPPER', 'RUSTFLAGS', 'RUSTUP_TOOLCHAIN'):
                del environment[name]
        environment.update(CARGO_HOME=str(self.recorder.directory / 'cargo-home'),
                           CARGO_NET_OFFLINE='true', RUSTUP_AUTO_INSTALL='0',
                           RUSTUP_TOOLCHAIN=self.toolchain['toolchain']['rustVersion'],
                           RUSTC=str(self.resolve(self.plan['rustVendor']['replay']['rustc'])))
        if jobs:
            environment['CARGO_BUILD_JOBS'] = jobs
        for name in ('cargo', 'rustc'):
            self.recorder.observe_tool(self.resolve(self.plan['rustVendor']['replay'][name]), environment)
        return environment

    def verify(self):
        bundle.require(API['identity'](self.path) == self.plan_identity and
                       API['identity'](self.config) == self.config_identity and
                       API['identity'](self.archive) == self.archive_identity and
                       API['identity'](self.report_path) == self.report_identity and
                       API['identity'](self.prepared_metadata) == self.prepared_metadata_identity,
                       'Prepared source plan, Cargo configuration or official source evidence changed')
        bundle.compare_checkout(ROOT, self.components['huhucapture']['files'], self.commit)
        gifski = next(s for s in self.plan['sources'] if s['id'] == 'gifski')
        original = self.components['gifski']
        bundle.compare_checkout(self.resolve(gifski['directory']), original['files'], original['source']['commit'])
        vendor = self.resolve(self.plan['rustVendor']['directory'])
        bundle.require(bundle.inventory(vendor) == self.components['rust-vendor']['files'],
                       'The actual directory source changed during the build')
        for item in self.runtime_inputs:
            path = self.sysroot / bundle.relative(item['path'])
            bundle.require(path.is_file() and not path.is_symlink() and path.stat().st_size == item['size'] and
                           bundle.digest(path) == item['sha256'],
                           'An actual compiler/standard-library file changed: ' + item['path'])

    def native_proof(self, library, command):
        self.verify()
        for tool in self.recorder.tools.values():
            bundle.require(API['identity'](tool['path']) == {key: tool[key] for key in ('size', 'sha256')},
                           'A recorded native build tool changed')
        library = Path(library).resolve(strict=True)
        output = {'path': str(library), **API['identity'](library)}
        execution = self.recorder.raw_record({'sources': self.sources, 'sourceCommit': self.commit,
                                              'cargoConfig': {'path': str(self.config), **self.config_identity}},
                                             {'library': output})
        self.recorder.retain_record(execution)
        value = {'schemaVersion': 1, 'kind': 'huhucapture-native-source-build', 'platform': self.platform,
                 'sourceCommit': self.commit, 'sources': self.sources, 'buildCommand': list(map(str, command)),
                 'sourcePlan': {'path': str(self.path), **self.plan_identity},
                 'cargoConfig': {'path': str(self.config), **self.config_identity}, 'library': output,
                 'executionRecord': {'path': str(execution), **API['identity'](execution)},
                 'buildRecords': [{'path': r['path'], 'sha256': r['sha256']} for r in self.recorder.records]}
        path = self.recorder.directory / 'native-source-build.json'
        API['write_new'](path, value)
        return {'path': str(path), **API['identity'](path)}

    def checked_native_proof(self, native_receipt):
        native = bundle.read_json(native_receipt)
        reference = native.get('sourceBuildProof')
        bundle.require(reference and API['identity'](reference['path']) ==
                       {key: reference[key] for key in ('size', 'sha256')},
                       'The aggregate lacks an unchanged actual pre-build source execution proof')
        value = bundle.read_json(reference['path'])
        bundle.require(value.get('schemaVersion') == 1 and value.get('kind') == 'huhucapture-native-source-build' and
                       value.get('platform') == self.platform and value.get('sourceCommit') == self.commit and
                       value.get('sources') == self.sources and
                       value.get('sourcePlan') == {'path': str(self.path), **self.plan_identity} and
                       value.get('cargoConfig') == {'path': str(self.config), **self.config_identity},
                       'Aggregate execution used other source components or another project commit')
        library = Path(native['library']).resolve(strict=True)
        bundle.require(value['library'] == {'path': str(library), **API['identity'](library)} and
                       native['librarySha256'] == value['library']['sha256'], 'Actual Rust aggregate changed after its source build')
        record = value['executionRecord']
        bundle.require(API['identity'](record['path']) == {key: record[key] for key in ('size', 'sha256')},
                       'Native execution record changed')
        execution = bundle.read_json(record['path'])
        expected_inputs = {'sources': self.sources, 'sourceCommit': self.commit, 'cargoConfig': value['cargoConfig']}
        bundle.require(execution.get('kind') == 'huhucapture-build-execution' and
                       execution.get('platform') == self.platform and execution.get('inputs') == expected_inputs and
                       execution.get('outputs') == {'library': value['library']} and
                       value['buildCommand'] in execution['commands'] and execution['executions'] and
                       all(item['exitCode'] == 0 for item in execution['executions']),
                       'Aggregate proof lacks a successful actual build command')
        bundle.require(any(item['arguments'] == value['buildCommand'] for item in execution['executions']),
                       'Aggregate build command has no matching real process execution')
        for name in ('cargo', 'rustc'):
            program = self.resolve(self.plan['rustVendor']['replay'][name])
            expected = {'path': str(program), **API['identity'](program)}
            bundle.require(expected in execution['toolchain'].values(),
                           'Native proof did not use the verified actual ' + name + ' tool')
        self.recorder.retain_record(Path(native_receipt))
        self.recorder.retain_record(Path(reference['path']))
        self.recorder.retain_record(Path(record['path']))
        for item in value['buildRecords']:
            bundle.require(bundle.digest(item['path']) == item['sha256'], 'A real native build evidence file changed')
            self.recorder.retain_record(Path(item['path']))
        return library
