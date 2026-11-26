"""Record and verify file ancestry at actual package transformation boundaries.

Installed inputs keep their original ancestry. The first real patchelf
observation can also identify a new input, but only a separate verified build
receipt can bind it to source. An interrupted chain cannot start again.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
PRODUCERS = ('scripts/package-qt.py', 'build/package/source_transforms.py', 'scripts/record-patchelf.py',
             'build/package/source_receipts.py', 'scripts/create-source-bundle.py')


def bytes_identity(value):
    return {'size': len(value), 'sha256': hashlib.sha256(value).hexdigest()}


def file_identity(api, path):
    value = api.descriptor(path)
    return {key: value[key] for key in ('size', 'sha256')}


def marker_bytes(identity, kind):
    return (json.dumps({'schemaVersion': 1, 'product': 'huhucapture', 'version': identity['version'],
                       'platform': identity['platform'], 'architecture': identity['architecture'],
                       'packageKind': kind}, indent=2) + '\n').encode('utf-8')


def desktop_bytes(original):
    action = b'Exec=huhucapture --action=finish-capture\n'
    if original.count(action) != 1:
        raise ValueError('The installed desktop finish action is missing or ambiguous')
    return original.replace(action, b'Exec=/opt/huhucapture/bin/huhucapture --action=finish-capture\n')


class Recorder:
    def __init__(self, stage, installed, execution, api):
        self.stage, self.installed = Path(stage).resolve(), Path(installed).resolve()
        self.payloads, self.execution, self.api = self.stage / 'payloads', execution, api
        self.original = api.inventory(self.installed)
        self.events, self.hooks, self.producers = [], [], []
        for name in PRODUCERS:
            source = ROOT / name
            existing = next((record for record in execution.records if record['inputPath'] == str(source)), None)
            record = existing or execution.retain_record(source)
            self.producers.append({'path': name, 'record': Path(record['path']).relative_to(execution.directory).as_posix(),
                                   **{key: record[key] for key in ('size', 'sha256')}})

    def point(self, path):
        path = Path(path).resolve(strict=True)
        domain, root = ('installed', self.installed) if path.is_relative_to(self.installed) else ('payload', self.payloads)
        self.api.require(path.is_relative_to(root), 'A packaging transformation input is outside its owned roots')
        return {'domain': domain, 'path': path.relative_to(root).as_posix(), **file_identity(self.api, path)}

    def add(self, operation, before, after, **extra):
        self.events.append({'operation': operation, 'before': before, 'after': after,
                            'position': len(self.execution.commands), **extra})

    def copy_tree(self, source, target):
        before = self.api.inventory(source)
        shutil.copytree(source, target, symlinks=True)
        self.api.require(self.api.inventory(source) == before and self.api.inventory(target) == before,
                         'The actual copied package tree differs from its input')
        for item in before:
            if item['type'] == 'file' and item['size']:
                self.add('copy', self.point(Path(source) / item['path']), self.point(Path(target) / item['path']))

    def mutation(self, path, arguments, runner, *, operation, **extra):
        before, index = self.point(path), len(self.execution.commands)
        runner(arguments)
        self.add(operation, before, self.point(path), position=index, commandIndex=index, **extra)

    def write_marker(self, path, identity, kind):
        before = self.point(path)
        Path(path).write_bytes(marker_bytes(identity, kind))
        self.add('package-marker', before, self.point(path), identity=identity, packageKind=kind,
                 identityInput=self.point(Path(path).with_name('build-identity.json')))

    def write_desktop(self, path):
        before, original = self.point(path), Path(path).read_bytes()
        Path(path).write_bytes(desktop_bytes(original))
        self.add('deb-desktop', before, self.point(path), inputText=original.decode('utf-8'))

    def patchelf_environment(self, linuxdeploy, environment, runner):
        self.api.require(not self.hooks, 'One package preparation must have one recorded linuxdeploy invocation')
        extraction = self.stage / 'work/patchelf-bundle'; extraction.mkdir()
        index = len(self.execution.commands)
        extract_environment = dict(environment); extract_environment.pop('APPIMAGE_EXTRACT_AND_RUN', None)
        runner([linuxdeploy, '--appimage-extract', 'usr/bin/patchelf'], cwd=extraction, env=extract_environment)
        actual = extraction / 'squashfs-root/usr/bin/patchelf'
        self.api.require(actual.is_file() and not actual.is_symlink() and os.access(actual, os.X_OK),
                         'The locked linuxdeploy image has no executable original patchelf')
        tool = self.execution.observe_tool(actual, environment)
        python = Path(sys.executable).resolve(strict=True)
        wrapper = next(item for item in self.producers if item['path'] == 'scripts/record-patchelf.py')
        wrapper_path = self.execution.directory / wrapper['record']
        launcher = self.execution.directory / 'recorded-patchelf'
        launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(python)) + ' ' + shlex.quote(str(wrapper_path)) + ' "$@"\n',
                            encoding='utf-8', newline='\n')
        launcher.chmod(0o755)
        records = self.execution.directory / 'patchelf'; records.mkdir()
        configuration = {'schemaVersion': 1, 'kind': 'huhucapture-patchelf-recording', 'tool': tool,
                         'python': {'path': str(python), **file_identity(self.api, python)},
                         'payloadRoot': str(self.payloads), 'records': str(records),
                         'parentCommandIndex': len(self.execution.commands),
                         'container': {'path': str(linuxdeploy), **file_identity(self.api, linuxdeploy)},
                         'wrapper': wrapper, 'launcher': {'path': str(launcher), **file_identity(self.api, launcher)}}
        path = self.execution.directory / 'patchelf-configuration.json'
        path.write_bytes(self.api.canonical(configuration))
        self.hooks.append({'configuration': {'path': path.relative_to(self.execution.directory).as_posix(),
                                            **file_identity(self.api, path)}, 'extractionCommandIndex': index, 'records': []})
        return {**environment, 'PATCHELF': str(launcher), 'HUHUCAPTURE_PATCHELF_RECORDING': str(path)}

    def collect_patchelf(self):
        hook = self.hooks[0]
        for path in sorted((self.execution.directory / 'patchelf').glob('*.json')):
            record = self.api.read_json(path)
            hook['records'].append({'path': path.relative_to(self.execution.directory).as_posix(),
                                    **file_identity(self.api, path)})
            self.api.require(record['exitCode'] == 0, 'A failed patchelf call cannot create a transformation chain')
            if record['path'] is not None:
                before = {'domain': 'payload', 'path': record['path'], **record['before']}
                after = {'domain': 'payload', 'path': record['path'], **record['after']}
                self.add(record['operation'], before, after, position=record['parentCommandIndex'],
                         record=path.relative_to(self.execution.directory).as_posix())

    def finish(self, identity, files):
        self.api.require(self.api.inventory(self.installed) == self.original, 'The original package input changed')
        value = {'schemaVersion': 2, 'kind': 'huhucapture-packaging-file-transformations',
                 'buildIdentity': identity, 'installedRoot': str(self.installed), 'payloadRoot': str(self.payloads),
                 'installedInput': self.original, 'files': files, 'producerSources': self.producers,
                 'patchelfHooks': self.hooks, 'events': self.events}
        path = self.stage / 'file-transformations.json'
        with path.open('xb') as stream:
            stream.write(self.api.canonical(value))
        return {'path': 'file-transformations.json', **self.api.descriptor(path)}


def validate(stage, transformations, execution, api):
    """Return only final paths with an unbroken observed original-input chain."""
    require = api.require
    specification = transformations.get('fileTransformations', {})
    require(specification.get('path') == 'file-transformations.json' and
            {key: value for key, value in specification.items() if key != 'path'} ==
            api.descriptor(stage / 'file-transformations.json') and
            execution.get('outputs', {}).get('fileTransformations') == specification,
            'Original per-file packaging transformation evidence is missing or changed')
    value = api.read_json(stage / 'file-transformations.json')
    require(type(value.get('schemaVersion')) is int and value['schemaVersion'] in (1, 2) and
            value.get('kind') == 'huhucapture-packaging-file-transformations' and
            value.get('buildIdentity') == transformations['buildIdentity'] and
            value.get('installedInput') == execution['inputs']['installedInput'] and
            value.get('files') == execution['outputs']['files'] and api.inventory(stage / 'payloads') == value['files'],
            'Per-file transformations refer to different original inputs or final output bytes')
    evidence = stage / 'executions'
    def retained(record):
        path = evidence / api.relative(record['path'])
        require(file_identity(api, path) == {key: record[key] for key in ('size', 'sha256')},
                'Original file-transform evidence bytes changed')
        return path
    require({item['path'] for item in value['producerSources']} == set(PRODUCERS),
            'Actual package transformation producer source records are incomplete')
    for item in value['producerSources']:
        retained({**item, 'path': item['record']})
    commands, executed = execution['commands'], execution['executions']
    require(commands and len(commands) == len(executed) and all(item['exitCode'] == 0 and
            item['commandIndex'] == index and item['arguments'] == commands[index] for index, item in enumerate(executed)),
            'File transformations require the original successful execution of every actual command')
    hooks = value['patchelfHooks']
    native_stages = [item for item in transformations.get('deploymentStages', []) if item['operation'] == 'native-deployment']
    native_deployment = bool(native_stages)
    require(len(hooks) == (1 if native_deployment else 0), 'Original patchelf hook evidence is missing')
    all_observations = value['schemaVersion'] == 2
    nested = {}
    for hook in hooks:
        configuration = api.read_json(retained(hook['configuration']))
        parent, extract = configuration['parentCommandIndex'], hook['extractionCommandIndex']
        require(configuration.get('schemaVersion') == 1 and configuration.get('kind') == 'huhucapture-patchelf-recording' and
                type(parent) is int and type(extract) is int and 0 <= extract < parent < len(commands) and
                extract in native_stages[0]['commands'] and parent in native_stages[0]['commands'] and
                commands[extract] == [configuration['container']['path'], '--appimage-extract', 'usr/bin/patchelf'] and
                executed[extract]['tool'] == configuration['container'] and
                commands[parent][0] == configuration['container']['path'] and
                executed[parent]['tool'] == configuration['container'] and
                executed[parent]['environment'].get('PATCHELF') == configuration['launcher']['path'] and
                executed[parent]['environment'].get('HUHUCAPTURE_PATCHELF_RECORDING') ==
                    str(Path(configuration['records']).parent / hook['configuration']['path']) and
                configuration['payloadRoot'] == value['payloadRoot'] and
                execution['toolchain'].get(configuration['tool']['path']) == configuration['tool'],
                'Recorded patchelf is not bound to the actual successful locked-image deployment command')
        launcher = evidence / Path(configuration['launcher']['path']).name
        require(file_identity(api, launcher) == {key: configuration['launcher'][key] for key in ('size', 'sha256')},
                'Actual patchelf recording launcher changed')
        wrapper = next(item for item in value['producerSources'] if item['path'] == 'scripts/record-patchelf.py')
        original_evidence = Path(configuration['records']).parent
        expected_launcher = '#!/bin/sh\nexec ' + shlex.quote(configuration['python']['path']) + ' ' + \
            shlex.quote(str(original_evidence / wrapper['record'])) + ' "$@"\n'
        require(configuration['wrapper'] == wrapper and launcher.read_bytes() == expected_launcher.encode('utf-8'),
                'Actual patchelf launcher is not bound to the retained original recording script')
        require(len(hook['records']) == len(list((evidence / 'patchelf').glob('*.json'))),
                'A real patchelf invocation is absent from the retained transformation journal')
        for index, item in enumerate(hook['records']):
            record = api.read_json(retained(item))
            require(item['path'] == 'patchelf/' + f'{index:06d}' + '.json' and record.get('sequence') == index and
                    record.get('kind') == 'huhucapture-patchelf-execution' and record.get('schemaVersion') == 1 and
                    record.get('exitCode') == 0 and record.get('parentCommandIndex') == parent and
                    record.get('configurationSha256') == hook['configuration']['sha256'] and
                    record.get('tool') == configuration['tool'] and record.get('python') == configuration['python'],
                    'Original per-file patchelf invocation is missing, unsuccessful or refers to another producer')
            for name in ('stdout', 'stderr'):
                require(record[name] == f'{index:06d}' + ('.stdout' if name == 'stdout' else '.stderr') and
                        (evidence / 'patchelf' / record[name]).is_file(), 'Original patchelf output log is missing')
            arguments = record['arguments']
            requested = record['requestedArguments']
            require(arguments[0] == configuration['tool']['path'] and arguments[-1] == record['absolutePath'],
                    'Patchelf arguments name a different actual tool or input file')
            require(isinstance(requested, list) and requested[:-1] == arguments[1:-1] and
                    Path(record['workingDirectory']).is_absolute() and
                    isinstance(requested[-1], str), 'The original requested patchelf arguments are missing')
            inside = Path(record['absolutePath']).is_relative_to(Path(value['payloadRoot']))
            require((record['path'] is None and not inside) or (record['path'] is not None and
                    record['absolutePath'] == str(Path(value['payloadRoot']) / api.relative(record['path']))),
                    'Actual patchelf observation has an inconsistent canonical payload path')
            if record['operation'] == 'patchelf-print-rpath':
                require(len(arguments) == 3 and arguments[1] == '--print-rpath' and record['before'] == record['after'],
                        'A read-only patchelf call changed bytes or has different arguments')
            else:
                require(record['operation'] == 'patchelf-set-rpath' and len(arguments) == 4 and
                        arguments[1] == '--set-rpath' and isinstance(arguments[2], str) and
                        record['absolutePath'] == str(Path(value['payloadRoot']) / api.relative(record['path'])),
                        'Unreviewed or out-of-payload native ELF transformation')
            if record['path'] is not None and (all_observations or record['operation'] == 'patchelf-set-rpath'):
                nested[item['path']] = record
    original = {item['path']: item for item in value['installedInput'] if item['type'] == 'file'}
    states, uncertain, used, previous = {}, {}, set(), -1
    seen_paths, observed_paths, poisoned = set(), set(), set()
    nested_order = list(nested)
    def byte_identity(item):
        require(isinstance(item, dict) and type(item.get('size')) is int and item['size'] > 0 and
                api.HEX64.fullmatch(item.get('sha256', '')), 'A file-transform boundary lacks its exact actual bytes')
        return {key: item[key] for key in ('size', 'sha256')}
    def current(point, observation=None):
        name = api.relative(point['path']); identity = byte_identity(point)
        if point['domain'] == 'installed':
            candidate = original.get(name)
            require(candidate is not None and byte_identity(candidate) == identity,
                    'A transformation root differs from the actual original installed input')
            return {'input': {'path': name, **identity}, 'current': identity}
        require(point['domain'] == 'payload', 'Unknown file-transform input domain')
        candidate = states.get(name)
        first = name not in seen_paths
        seen_paths.add(name)
        if name in poisoned:
            return None
        if candidate is not None and candidate['current'] == identity:
            return candidate
        if first and candidate is None and observation is not None and all_observations:
            observed_paths.add(name)
            return {'input': {'kind': 'observed-input', 'path': name, **identity,
                             'record': observation['record'], 'sequence': observation['sequence'],
                             'commandIndex': observation['commandIndex']}, 'current': identity}
        if name in observed_paths:
            poisoned.add(name)
        uncertain[name] = 'A newly deployed file or an unrecorded change has no original-input ancestry'
        return None
    for event in value['events']:
        operation, before, after, position = event['operation'], event['before'], event['after'], event['position']
        require(type(position) is int and previous <= position <= len(commands), 'File-transform execution order changed')
        previous = position
        require(after['domain'] == 'payload', 'A packaging transformation cannot rewrite original inputs')
        destination = api.relative(after['path']); output_identity = byte_identity(after)
        observation = None
        if operation in ('patchelf-set-rpath', 'patchelf-print-rpath'):
            record = nested.get(event['record'])
            require(record is not None and event['record'] not in used and
                    event['record'] == nested_order[len(used)] and operation == record['operation'] and
                    position == record['parentCommandIndex'] and record['path'] == destination and
                    record['before'] == byte_identity(before) and record['after'] == output_identity,
                    'File ancestry differs from the complete ordered original patchelf observations')
            used.add(event['record'])
            observation = {'record': event['record'], 'sequence': record['sequence'], 'commandIndex': position}
        ancestor = current(before, observation)
        if operation == 'copy':
            require(byte_identity(before) == output_identity, 'A recorded copy changed file bytes')
        else:
            require(before['domain'] == 'payload' and before['path'] == after['path'],
                    'A recorded in-place transformation moved an unrecorded input')
            absolute = str(Path(value['payloadRoot']) / destination)
            if operation in ('patchelf-set-rpath', 'patchelf-print-rpath'):
                pass  # Both observations were bound to their original records above.
            elif operation in ('objcopy-strip-debug', 'objcopy-add-debuglink'):
                index = event['commandIndex']
                require(type(index) is int and index == position and index < len(commands) and
                        Path(commands[index][0]).name == 'objcopy', 'An ELF transformation has no original objcopy invocation')
                if operation == 'objcopy-strip-debug':
                    require(commands[index][1:] == ['--strip-debug', absolute], 'The actual objcopy strip arguments differ')
                else:
                    debug = event['debugInput']; byte_identity(debug)
                    debug_path = stage / Path(debug['path']).relative_to(Path(value['payloadRoot']).parent)
                    require(commands[index][1:] == ['--add-gnu-debuglink=' + debug['path'], absolute] and
                            type(event['debugCommandIndex']) is int and 0 <= event['debugCommandIndex'] < index and
                            commands[event['debugCommandIndex']] == [commands[index][0], '--only-keep-debug', absolute, debug['path']] and
                            file_identity(api, debug_path) == byte_identity(debug),
                            'The actual objcopy debug link lacks its original matching debug production')
            elif operation == 'package-marker':
                identity, kind = event['identity'], event['packageKind']
                require(identity == value['buildIdentity'] and kind in ('appimage', 'deb', 'installer', 'portable-zip') and
                        destination.endswith('/installation.json') and output_identity == bytes_identity(marker_bytes(identity, kind)) and
                        event['identityInput']['domain'] == 'payload' and
                        event['identityInput']['path'] == str(Path(destination).with_name('build-identity.json')),
                        'Generated package marker differs from its actual identity/script inputs')
                identity_origin = current(event['identityInput'])
                if identity_origin is None:
                    ancestor = None
            elif operation == 'deb-desktop':
                text = event['inputText'].encode('utf-8')
                require(len(text) <= 1024 * 1024 and bytes_identity(text) == byte_identity(before) and
                        output_identity == bytes_identity(desktop_bytes(text)) and
                        destination.endswith('/applications/io.github.NAMEWTA.huhucapture.desktop'),
                        'Generated desktop action differs from the actual recorded source/recipe')
            else:
                raise ValueError('Unknown file-transform operation; no source ancestry can be inferred')
        if before['domain'] == 'payload' and before['path'] in observed_paths:
            observed_paths.add(destination)
            if before['path'] in poisoned:
                poisoned.add(destination)
        seen_paths.add(destination)
        if ancestor is None or destination in poisoned:
            states.pop(destination, None)
        else:
            states[destination] = {'input': ancestor['input'], 'current': output_identity}
    require(used == set(nested), 'A real patchelf observation was omitted from source ancestry validation')
    final = {item['path']: item for item in value['files'] if item['type'] == 'file' and item['size']}
    origins, observed = {}, {}
    for name, state in states.items():
        if name in final and state['current'] == byte_identity(final[name]):
            target = observed if state['input'].get('kind') == 'observed-input' else origins
            target[name] = {'input': state['input'], 'output': {'path': name, **state['current']}}
        elif name in final:
            uncertain[name] = 'Final bytes changed after the last recorded transformation'
    return {'origins': origins, 'observedOrigins': observed,
            'observedPaths': sorted(observed_paths & set(final)),
            'unresolved': uncertain, 'producerSources': value['producerSources']}


def bind_observed_inputs(observations, receipts):
    """Bind first observed bytes only to one independently verified producer.

    The caller must validate receipts first. No source blob, filename heuristic,
    or declaration of a copied path can replace a matching actual build output.
    """
    result = copy.deepcopy(observations)
    outputs = {}
    for identifier, receipt in receipts.items():
        for output in receipt['outputs']:
            outputs.setdefault((output['size'], output['sha256']), []).append(
                {'receipt': identifier, 'output': output['path']})
    for name in observations['observedPaths']:
        result['origins'].pop(name, None)
        candidate = observations['observedOrigins'].get(name)
        if candidate is None:
            result['unresolved'][name] = 'The first observed input lost its continuous recorded transformation chain'
            continue
        matches = outputs.get((candidate['input']['size'], candidate['input']['sha256']), [])
        if len(matches) == 1:
            result['origins'][name] = {**candidate, 'producer': matches[0]}
        else:
            result['unresolved'][name] = ('No independently verified original build output matches the first observation'
                                         if not matches else 'The first observation matches multiple distinct original build outputs')
    return result
