"""Verify original Microsoft release CRT inputs selected by the VS generator."""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import subprocess


LICENSE = 'https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution'
CRT_FILES = {
    'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll',
}
SIGNATURE_COMMAND = r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$dll = Get-Item -LiteralPath $env:HUHUCAPTURE_CRT_INPUT
$signature = Get-AuthenticodeSignature -LiteralPath $dll.FullName
if ($signature.Status -ne 'Valid' -or
    $signature.SignerCertificate.Subject -notmatch '(?:^|,\s*)O=Microsoft Corporation(?:,|$)') {
    throw ('{0}: Authenticode status {1}. {2} Signer: {3}' -f
        $dll.FullName, $signature.Status, $signature.StatusMessage, $signature.SignerCertificate.Subject)
}
[pscustomobject]@{
    signer = $signature.SignerCertificate.Subject
    thumbprint = $signature.SignerCertificate.Thumbprint
    fileVersion = $dll.VersionInfo.FileVersion
} | ConvertTo-Json -Compress
"""


def selected_inputs(path, require):
    lines = path.read_text(encoding='utf-8').splitlines()
    require(os.name == 'nt' and 3 <= len(lines) <= len(CRT_FILES) + 1 and
            all(Path(line).is_absolute() for line in lines),
            'The Windows build must retain the actual v143 CMake runtime selection')
    redist = (Path(lines[0]) / 'VC/Redist/MSVC').resolve(strict=True)
    files = {}
    versions = set()
    for line in lines[1:]:
        original = Path(line)
        selected = original.resolve(strict=True)
        require(selected.is_relative_to(redist) and not original.is_symlink(),
                'Selected CRT file is outside the actual Visual Studio redistribution directory')
        parts = selected.relative_to(redist).parts
        name = selected.name
        require(len(parts) == 4 and re.fullmatch(r'14\.\d+\.\d+', parts[0]) is not None and
                parts[1:3] == ('x64', 'Microsoft.VC143.CRT') and name in CRT_FILES and name not in files,
                'Expected original x64 Microsoft.VC143.CRT release DLLs')
        versions.add(parts[0])
        files[name] = selected
    require(len(versions) == 1 and {'msvcp140.dll', 'vcruntime140.dll'} <= files.keys(),
            'The CRT DLLs must come from one complete release version')
    return files


def verify_input(path, metadata):
    metadata.require(metadata.machine(path) == 'windows', 'The Microsoft runtime DLL must be x64 PE')
    before = metadata.descriptor(path)
    shell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    command = base64.b64encode(SIGNATURE_COMMAND.encode('utf-16-le')).decode('ascii')
    checked = subprocess.run([str(shell), '-NoProfile', '-NonInteractive', '-EncodedCommand', command],
                             env={**os.environ, 'HUHUCAPTURE_CRT_INPUT': str(path)},
                             capture_output=True, encoding='utf-8', errors='replace', timeout=30)
    metadata.require(checked.returncode == 0,
                     f'Cannot verify the original Microsoft CRT DLL {path}: {checked.stderr.strip()}')
    signature = json.loads(checked.stdout)
    metadata.require(metadata.descriptor(path) == before, 'Original CRT DLL changed during signature verification')
    return {'path': str(path), **before, **signature}


def report(selection, metadata, installed=None):
    files = selected_inputs(selection, metadata.require)
    signatures = []
    for name, path in sorted(files.items()):
        entry = verify_input(path, metadata)
        if installed is not None:
            metadata.require(metadata.descriptor(installed / name) == {k: entry[k] for k in ('name', 'size', 'sha256')},
                             'Installed CRT DLL differs from the original Microsoft file: ' + name)
        signatures.append(entry)
    return {'kind': 'microsoft-vc143-release-runtime', 'redistribution': LICENSE, 'files': signatures}
