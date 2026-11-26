"""Record real native build execution and exact inputs at the producer boundary.

This module does not recover receipts from intended commands or old SDKs. A
receipt is written only after its recorded commands actually return success and
the original checked source and produced files have been compared again.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import types

ROOT = Path(__file__).resolve().parents[2]
COLLECTOR = ROOT / 'scripts/create-source-bundle.py'
bundle = types.ModuleType('huhucapture_source_receipt_bundle')
bundle.__file__ = str(COLLECTOR)
exec(compile(COLLECTOR.read_bytes(), str(COLLECTOR), 'exec'), bundle.__dict__)
ENVIRONMENT = ('CC', 'CXX', 'AR', 'LD', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS',
               'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR', 'CMAKE_PREFIX_PATH', 'QT_ROOT_DIR',
               'INCLUDE', 'LIB', 'VCTOOLSVERSION', 'WINDOWSSDKVERSION', 'SOURCE_DATE_EPOCH',
               'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'RUSTUP_TOOLCHAIN', 'CARGO_TARGET_DIR',
               'APPIMAGE_EXTRACT_AND_RUN', 'QMAKE', 'NO_STRIP', 'ARCH', 'EXTRA_PLATFORM_PLUGINS',
               'NSISDIR', 'NSISCONFDIR')


def identity(path):
    path = Path(path)
    value = bundle.descriptor(path)
    return {key: value[key] for key in ('size', 'sha256')}


def write_new(path, value):
    with path.open('xb') as stream:
        stream.write(bundle.canonical(value))


class ExecutionLog:
    def __init__(self, directory, platform, producer):
        self.directory = Path(directory).resolve()
        bundle.require(platform in ('linux', 'windows'), 'Unsupported build execution platform')
        bundle.require(not self.directory.exists(), 'Build execution evidence already exists; retain the earlier result')
        self.directory.mkdir(parents=True)
        (self.directory / 'logs').mkdir()
        self.platform, self.commands, self.executions = platform, [], []
        self.tools, self.records, self.origins = {}, [], {}
        self.producer = self.retain_record(Path(producer).resolve())
        self.retain_record(Path(__file__).resolve())
        self.retain_record(COLLECTOR)

    def retain_record(self, path):
        path = Path(path)
        before = identity(path)
        target = self.directory / 'records' / (str(len(self.records)) + '-' + bundle.relative(path.name))
        target.parent.mkdir(exist_ok=True)
        with path.open('rb') as incoming, target.open('xb') as outgoing:
            shutil.copyfileobj(incoming, outgoing)
        bundle.require(identity(path) == before and identity(target) == before, 'Build input changed during retention')
        record = {'path': str(target), 'inputPath': str(path), **before}
        self.records.append(record)
        return record

    def observe_tool(self, program, environment=None):
        environment = environment or os.environ
        executable = shutil.which(str(program), path=environment.get('PATH'))
        bundle.require(executable is not None, 'The actual build tool is unavailable: ' + str(program))
        path = Path(executable).resolve(strict=True)
        value = {'path': str(path), **identity(path)}
        key = str(program)
        bundle.require(key not in self.tools or self.tools[key] == value, 'A build tool changed during execution')
        self.tools[key] = value
        return value

    def run(self, arguments, *, cwd, env=None, capture=False):
        command = list(map(str, arguments))
        bundle.require(command and all(command), 'A build command must contain actual nonempty arguments')
        environment = os.environ.copy() if env is None else dict(env)
        program = self.observe_tool(command[0], environment)
        index = len(self.commands)
        self.commands.append(command)
        stdout = self.directory / 'logs' / (str(index) + '.stdout.log')
        stderr = self.directory / 'logs' / (str(index) + '.stderr.log')
        record = {'commandIndex': index, 'arguments': command, 'cwd': str(Path(cwd).resolve()),
                  'environment': {key: environment[key] for key in ENVIRONMENT if key in environment},
                  'tool': program, 'stdout': str(stdout), 'stderr': str(stderr), 'exitCode': None}
        self.executions.append(record)
        print('Running recorded build command:', shlex.join(command), flush=True)
        with stdout.open('xb') as output, stderr.open('xb') as errors:
            try:
                result = subprocess.run(command, cwd=cwd, env=environment, stdout=output, stderr=errors, check=False)
                record['exitCode'] = result.returncode
            except OSError as error:
                record['launchError'] = str(error)
                raise
            finally:
                write_new(self.directory / ('execution-' + str(index) + '.json'), record)
        bundle.require(identity(Path(program['path'])) == {key: program[key] for key in ('size', 'sha256')},
                       'The executed build tool changed while running')
        for path in (stdout, stderr):
            if path.stat().st_size:
                self.retain_record(path)
        self.retain_record(self.directory / ('execution-' + str(index) + '.json'))
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, command)
        if capture:
            return stdout.read_text(encoding='utf-8', errors='replace'), stderr.read_text(encoding='utf-8', errors='replace')
        return index

    def run_fixed_script(self, wrapper, script, commands, *, build_directory, posix_directory, env):
        # The existing Windows builder writes exactly set -eu, cd, then these
        # shell-quoted commands. Only a successful execution of the unchanged
        # controlled script binds its inner configure/make argument arrays.
        expected = 'set -eu\ncd ' + shlex.quote(posix_directory) + '\n' + ''.join(shlex.join(c) + '\n' for c in commands)
        bundle.require(Path(script).read_bytes() == expected.encode('utf-8'), 'Controlled native build script differs')
        retained = self.retain_record(Path(script))
        wrapper_index = self.run(wrapper, cwd=build_directory, env=env)
        bundle.require(identity(Path(script)) == {key: retained[key] for key in ('size', 'sha256')},
                       'Controlled native build script changed while executing')
        indices = []
        for line, command in enumerate(commands, 3):
            index = len(self.commands)
            self.commands.append(list(command))
            self.origins[str(index)] = {'executionCommandIndex': wrapper_index, 'script': retained, 'line': line,
                                        'meaning': 'Executed by this successful unchanged set -eu script'}
            indices.append(index)
        return indices

    def raw_record(self, inputs, outputs):
        bundle.require(self.executions and all(record['exitCode'] == 0 for record in self.executions),
                       'Unsuccessful commands cannot produce a successful build execution record')
        value = {'schemaVersion': 1, 'kind': 'huhucapture-build-execution', 'platform': self.platform,
                 'producer': self.producer, 'commands': self.commands, 'executions': self.executions,
                 'commandOrigins': self.origins, 'toolchain': self.tools, 'inputs': inputs, 'outputs': outputs,
                 'records': self.records}
        path = self.directory / 'build-execution.json'
        write_new(path, value)
        return path


class NativeBuild(ExecutionLog):
    def __init__(self, directory, platform, producer, component, source, pin, prefix):
        super().__init__(directory, platform, producer)
        self.component, self.source, self.pin = component, Path(source).resolve(), pin
        self.prefix = Path(prefix).resolve()
        self.before_outputs = bundle.inventory(self.prefix) if self.prefix.exists() else []
        patches = []
        for item in pin.get('patches', []):
            path = ROOT / bundle.relative(item['path'])
            bundle.require(bundle.digest(path) == item['sha256'], 'Producer patch differs from its exact pin')
            patches.append(Path(self.retain_record(path)['path']))
        with tempfile.TemporaryDirectory(prefix='producer-source-', dir=self.directory) as temporary:
            self.provenance, self.files = bundle.export_git(self.source, pin['commit'], Path(temporary) / 'source', patches)
        self.source_hash = bundle.tree_digest(self.files)
        write_new(self.directory / 'source-inputs.json', {'component': component, 'repository': pin['repository'],
                  'source': self.provenance, 'files': self.files, 'sourceTreeSha256': self.source_hash})
        self.retain_record(self.directory / 'source-inputs.json')

    def finish(self, *, configure_index=None, configuration_files=()):
        bundle.compare_checkout(self.source, self.files, self.pin['commit'])
        for path in configuration_files:
            self.retain_record(Path(path))
        after = bundle.inventory(self.prefix)
        before = {entry['path']: entry for entry in self.before_outputs}
        changed = [entry for entry in after if entry != before.get(entry['path'])]
        outputs = [{key: item[key] for key in ('path', 'size', 'sha256')}
                   for item in changed if item['type'] == 'file' and item['size']]
        bundle.require(outputs, 'No new or changed real build outputs were produced')
        execution = self.raw_record({'sources': [{'id': self.component, 'sourceTreeSha256': self.source_hash}],
                                    'prefixBefore': self.before_outputs}, {'prefix': str(self.prefix), 'files': changed})
        self.retain_record(execution)
        receipt = {'schemaVersion': 1, 'kind': 'huhucapture-source-build-receipt', 'platform': self.platform,
                   'commands': self.commands, 'commandOrigins': self.origins, 'toolchain': self.tools,
                   'environment': {'executions': [record['environment'] for record in self.executions]},
                   'sources': [{'id': self.component, 'sourceTreeSha256': self.source_hash}],
                   'staticInputs': [], 'outputs': outputs,
                   'profileCommands': {self.component: configure_index} if configure_index is not None else {}}
        bundle.require(bundle.inventory(self.prefix) == after, 'Actual installed outputs changed while recording receipt')
        bundle.compare_checkout(self.source, self.files, self.pin['commit'])
        path = self.directory / 'source-build-receipt.json'
        write_new(path, receipt)
        plan = {'id': self.component, 'path': str(path),
                'buildRecords': [{'path': item['path'], 'sha256': item['sha256']} for item in self.records],
                'staticFiles': {}}
        write_new(self.directory / 'receipt-plan.json', plan)
        return plan
