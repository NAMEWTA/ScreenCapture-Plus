#!/usr/bin/env python3
"""Record linuxdeploy's real patchelf calls through its documented PATCHELF hook."""
from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def identity(path):
    path = Path(path)
    if not path.is_file() or path.is_symlink():
        raise ValueError('The actual patchelf input/tool must be a regular file')
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            value.update(data)
    return {'size': path.stat().st_size, 'sha256': value.hexdigest()}


def main():
    configuration_path = Path(os.environ['HUHUCAPTURE_PATCHELF_RECORDING']).resolve(strict=True)
    configuration_bytes = configuration_path.read_bytes()
    configuration = json.loads(configuration_bytes)
    if configuration.get('schemaVersion') != 1 or configuration.get('kind') != 'huhucapture-patchelf-recording':
        raise ValueError('Unknown original patchelf recording configuration')
    arguments = sys.argv[1:]
    if not ((len(arguments) == 2 and arguments[0] == '--print-rpath') or
            (len(arguments) == 3 and arguments[0] == '--set-rpath')):
        raise ValueError('Unreviewed linuxdeploy patchelf operation; no transformation can be inferred')
    tool = configuration['tool']
    if identity(tool['path']) != {key: tool[key] for key in ('size', 'sha256')}:
        raise ValueError('The extracted original patchelf bytes changed')
    target = Path(arguments[-1]).resolve(strict=True)
    root = Path(configuration['payloadRoot']).resolve(strict=True)
    mutating = arguments[0] == '--set-rpath'
    if mutating and not target.is_relative_to(root):
        raise ValueError('Packaging patchelf cannot modify an original file outside its owned payload')
    records = Path(configuration['records']).resolve(strict=True)
    with (records / 'journal.lock').open('ab') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        index = len(list(records.glob('*.json')))
        stem = f'{index:06d}'
        stdout, stderr = records / (stem + '.stdout'), records / (stem + '.stderr')
        before = identity(target)
        command = [tool['path'], *arguments[:-1], str(target)]
        with stdout.open('xb') as output, stderr.open('xb') as errors:
            result = subprocess.run(command, stdout=output, stderr=errors, check=False)
        after = identity(target)
        record = {'schemaVersion': 1, 'kind': 'huhucapture-patchelf-execution', 'sequence': index,
                  'configurationSha256': hashlib.sha256(configuration_bytes).hexdigest(),
                  'parentCommandIndex': configuration['parentCommandIndex'], 'arguments': command,
                  'requestedArguments': arguments, 'workingDirectory': str(Path.cwd()),
                  'operation': 'patchelf-set-rpath' if mutating else 'patchelf-print-rpath',
                  'path': target.relative_to(root).as_posix() if target.is_relative_to(root) else None,
                  'absolutePath': str(target), 'before': before, 'after': after, 'tool': tool,
                  'python': {'path': str(Path(sys.executable).resolve()), **identity(Path(sys.executable).resolve())},
                  'stdout': stdout.name, 'stderr': stderr.name, 'exitCode': result.returncode}
        with (records / (stem + '.json')).open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(record, stream, sort_keys=True, indent=2)
            stream.write('\n')
        if configuration_path.read_bytes() != configuration_bytes or identity(tool['path']) != {
                key: tool[key] for key in ('size', 'sha256')}:
            raise ValueError('Original patchelf configuration/tool changed during execution')
        if not mutating and before != after:
            raise ValueError('A read-only patchelf call changed its source file')
        sys.stdout.buffer.write(stdout.read_bytes())
        sys.stderr.buffer.write(stderr.read_bytes())
        return result.returncode


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError) as error:
        print('Recorded patchelf failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
