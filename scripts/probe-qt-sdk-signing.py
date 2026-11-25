#!/usr/bin/env python3
"""Collect native Windows evidence for the three observed Qt SDK/SPDX failures.

Only Microsoft's installed SDK SignTool may verify/remove signatures. Removal
targets exclusively created copies; the original SDK and SPDX are never edited.
This diagnostic does not change qt_materials' complete-file SHA1 requirement or
certify any runtime, SDK, license or corresponding-source closure.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
specification = importlib.util.spec_from_file_location('qt_signing_materials',
                                                       ROOT / 'scripts/collect-runtime-materials.py')
materials = importlib.util.module_from_spec(specification)
specification.loader.exec_module(materials)
qt_materials = materials.module('qt_signing_spdx', ROOT / 'build/package/qt_materials.py')

# These are the actual failing/related files, not an acceptance allowlist.
PLUGINS = ('plugins/platforms/qwindows.dll', 'plugins/tls/qschannelbackend.dll',
           'plugins/generic/qtuiotouchplugin.dll')
MAXIMUM_FILE_BYTES = 64 * 1024 * 1024
COMMAND_TIMEOUT_SECONDS = 45


def fingerprint(path):
    before = path.lstat()
    materials.meta.require(stat.S_ISREG(before.st_mode) and not path.is_symlink() and
                           0 < before.st_size <= MAXIMUM_FILE_BYTES,
                           'Expected a bounded original regular file: ' + str(path))
    record = materials.unchanged_digest(path)
    sha1 = qt_materials.digest_file(path, 'sha1')
    after = path.lstat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
    materials.meta.require(identity(before) == identity(after) and materials.unchanged_digest(path) == record,
                           'Input changed while recording its identity and complete hashes: ' + str(path))
    return {**record, 'sha1': sha1,
            'identity': {'device': str(before.st_dev), 'file': str(before.st_ino)},
            'revision': {'mtimeNs': str(before.st_mtime_ns), 'ctimeNs': str(before.st_ctime_ns)}}


def installed_signtool(path):
    """Require the supplied x64 tool to belong to a registered Windows SDK."""
    import winreg
    tool = path.resolve(strict=True)
    roots = set()
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\Windows Kits\Installed Roots',
                                0, winreg.KEY_READ | view) as key:
                root, kind = winreg.QueryValueEx(key, 'KitsRoot10')
                if kind == winreg.REG_SZ:
                    roots.add(materials.real_directory(Path(root)))
        except FileNotFoundError:
            continue
    matches = []
    for root in roots:
        if tool.is_relative_to(root):
            relative = tool.relative_to(root)
            if (len(relative.parts) == 4 and relative.parts[0].lower() == 'bin' and
                    re.fullmatch(r'\d+\.\d+\.\d+\.\d+', relative.parts[1]) and
                    relative.parts[2].lower() == 'x64' and relative.parts[3].lower() == 'signtool.exe'):
                matches.append((root, relative.parts[1]))
    materials.meta.require(len(matches) == 1, 'Use the actual x64 SignTool inside one registered Windows SDK')
    materials.meta.require(materials.meta.machine(tool) == 'windows', 'SignTool must be an actual x64 PE32+ binary')
    root, version = matches[0]
    return tool, {'sdkRoot': str(root), 'sdkDirectoryVersion': version}


def command(arguments, directory, log_path):
    """Keep native output bytes and distinguish failure, warning and timeout."""
    record = {'arguments': list(map(str, arguments)), 'timeoutSeconds': COMMAND_TIMEOUT_SECONDS,
              'log': log_path.relative_to(directory).as_posix(), 'exitCode': None, 'timedOut': False}
    with log_path.open('xb') as log:
        try:
            completed = subprocess.run(record['arguments'], cwd=directory, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, timeout=COMMAND_TIMEOUT_SECONDS)
            record['exitCode'] = completed.returncode
        except subprocess.TimeoutExpired:
            record['timedOut'] = True
        except OSError as error:
            record['launchError'] = str(error)
    record['logBytes'] = log_path.stat().st_size
    record['logSha256'] = qt_materials.digest_file(log_path, 'sha256')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-prefix', type=Path, required=True, help='Original actual Windows Qt SDK')
    parser.add_argument('--signtool', type=Path, required=True, help='Installed Windows SDK x64/signtool.exe')
    parser.add_argument('--output', type=Path, required=True, help='New empty separate diagnostic directory')
    options = parser.parse_args()
    report = {'schemaVersion': 1, 'status': 'running', 'scope': __doc__.strip(), 'files': [], 'failures': [],
              'productionAcceptanceChanged': False, 'correspondingSourceComplete': False}
    output = None
    originals = {}
    report_identity = None

    def save():
        # metadata.json_write intentionally claims a new file; this one live
        # report is updated repeatedly inside this probe's exclusively owned
        # output. Keep the previous complete JSON if writing its replacement
        # fails, and never replace a different file installed at the pathname.
        nonlocal report_identity
        destination = output / 'qt-sdk-signing.json'

        def check_owner():
            if report_identity is None:
                materials.meta.require(not destination.exists() and not destination.is_symlink(),
                                       'The initial diagnostic report path is already occupied')
            else:
                current = destination.lstat()
                materials.meta.require(stat.S_ISREG(current.st_mode) and not destination.is_symlink() and
                                       (current.st_dev, current.st_ino) == report_identity,
                                       'The diagnostic report pathname changed ownership')

        check_owner()
        descriptor, name = tempfile.mkstemp(prefix='qt-sdk-signing-', suffix='.json.tmp', dir=output)
        temporary = Path(name)
        try:
            with os.fdopen(descriptor, 'wb') as file:
                file.write((json.dumps(report, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
                file.flush(); os.fsync(file.fileno())
            check_owner()
            os.replace(temporary, destination)
            current = destination.lstat()
            report_identity = (current.st_dev, current.st_ino)
        finally:
            if temporary.exists():
                temporary.unlink()

    try:
        materials.meta.require(os.name == 'nt', 'This probe requires actual Windows; emulated results are not accepted')
        prefix = materials.real_directory(options.qt_prefix)
        tool, registered = installed_signtool(options.signtool)
        requested = options.output.resolve()
        materials.meta.require(not requested.is_relative_to(prefix) and not prefix.is_relative_to(requested) and
                               not requested.is_relative_to(Path(registered['sdkRoot'])) and
                               not tool.is_relative_to(requested), 'Diagnostic output must not overlap SDK/tool inputs')
        output = materials.new_output(options.output)
        (output / 'logs').mkdir()
        report['qtPrefix'] = str(prefix)
        originals[tool] = fingerprint(tool)
        report['tool'] = {'path': str(tool), **registered, **originals[tool]}
        save()

        # Reuse the retained Windows packager's .NET FileVersionInfo API. This
        # script reads version resources; it does not load/execute the DLLs.
        powershell = shutil.which('pwsh')
        materials.meta.require(powershell, 'The native runner must provide PowerShell for Windows file-version metadata')
        version_script = output / 'tool-version.ps1'
        with version_script.open('x', encoding='utf-8') as script:
            script.write("param([Parameter(Mandatory=$true)][string]$Path)\n"
                         "$ErrorActionPreference = 'Stop'\n"
                         "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
                         "$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)\n"
                         "@{ FileVersion = $info.FileVersion; ProductVersion = $info.ProductVersion; "
                         "OriginalFilename = $info.OriginalFilename } | ConvertTo-Json -Compress\n")
        version_command = command([powershell, '-NoProfile', '-NonInteractive', '-File', version_script,
                                   '-Path', tool], output, output / 'logs/tool-version.log')
        report['tool']['versionCommand'] = version_command
        materials.meta.require(version_command['exitCode'] == 0, 'Could not read the actual Windows SignTool version')
        version_info = json.loads((output / 'logs/tool-version.log').read_bytes())
        materials.meta.require(isinstance(version_info, dict) and version_info.get('FileVersion') and
                               version_info.get('ProductVersion'), 'SignTool has no actual version resource')
        report['tool']['versionInfo'] = version_info

        version = materials.meta.json_read(ROOT / 'build/dependencies.lock.json')['qtMigration']['qt']['version']
        document = prefix / 'sbom' / f'qtbase-{version}.spdx.json'
        materials.meta.require(document.resolve(strict=True) == document,
                               'The original Qtbase SPDX pathname must not be aliased')
        originals[document] = fingerprint(document)
        documents, index = qt_materials.sdk_spdx(materials, prefix, version)
        materials.meta.require(documents['qtbase']['path'] == document and
                               fingerprint(document) == originals[document],
                               'The original Qtbase SPDX changed while indexing its file checksums')
        document_copy = output / 'original-spdx' / document.name
        materials.copy_original(document, document_copy)
        report['originalSpdx'] = {'path': str(document), 'retainedPath': document_copy.relative_to(output).as_posix(),
                                  **originals[document]}
        report['qtVersion'] = version
        save()

        for number, relative in enumerate(PLUGINS, 1):
            item = {'path': relative, 'signatureVerified': False, 'removedCopyMatchesSpdx': False,
                    'unsignedSignatureRejected': False, 'originalUnchanged': False, 'failures': []}
            report['files'].append(item)
            source = None
            signed = None
            try:
                materials.meta.require(relative in index and index[relative]['module'] == 'qtbase',
                                       'The actual original Qtbase SPDX lacks the requested plugin')
                source = (prefix / relative).resolve(strict=True)
                materials.meta.require(source.is_relative_to(prefix) and source.relative_to(prefix).as_posix() == relative,
                                       'The original plugin path escapes or aliases its SPDX path')
                originals[source] = fingerprint(source)
                item['originalBefore'] = originals[source]
                item['spdxSha1'] = index[relative]['sha1']
                item['spdxIds'] = index[relative]['spdxIds']
                item['originalMatchesSpdx'] = originals[source]['sha1'] == item['spdxSha1']
                signed = output / 'original' / relative
                removed = output / 'removed' / relative
                materials.copy_original(source, signed)
                item['signedCopy'] = {'path': signed.relative_to(output).as_posix(), **fingerprint(signed)}
                materials.copy_original(signed, removed)
                item['removalInput'] = {'path': removed.relative_to(output).as_posix(), **fingerprint(removed)}
                materials.meta.require(item['signedCopy']['identity'] != originals[source]['identity'] and
                                       item['removalInput']['identity'] != originals[source]['identity'] and
                                       item['removalInput']['identity'] != item['signedCopy']['identity'],
                                       'Each diagnostic copy must have an independent file identity')
                save()

                verify = command([tool, 'verify', '/pa', '/all', '/v', '/tw', source], output,
                                 output / f'logs/{number}-verify-original.log')
                item['verifyOriginal'] = verify
                materials.meta.require(verify['exitCode'] == 0,
                                       'Original signature verification failed, warned or timed out; no removal admitted')
                item['signatureVerified'] = True
                removal = command([tool, 'remove', '/s', removed], output, output / f'logs/{number}-remove-copy.log')
                item['removeCopy'] = removal
                materials.meta.require(removal['exitCode'] == 0, 'Official SignTool removal failed or timed out')
                item['removedCopy'] = {'path': removed.relative_to(output).as_posix(), **fingerprint(removed)}
                item['removedCopyMatchesSpdx'] = item['removedCopy']['sha1'] == item['spdxSha1']
                # The same strict verifier must reject this genuinely unsigned
                # copy. It never becomes an accepted signed SDK input.
                rejected = command([tool, 'verify', '/pa', '/all', '/v', '/tw', removed], output,
                                   output / f'logs/{number}-verify-removed.log')
                item['verifyRemoved'] = rejected
                item['unsignedSignatureRejected'] = rejected['exitCode'] == 1
                materials.meta.require(item['unsignedSignatureRejected'],
                                       'The actual unsigned copy was not rejected with the documented failure status')
                materials.meta.require(item['removedCopyMatchesSpdx'],
                                       'Officially removed complete-file SHA1 still differs from original SPDX; no PE fields changed')
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                item['failures'].append(str(error))
                report['failures'].append(relative + ': ' + str(error))
            finally:
                if signed is not None and 'signedCopy' in item:
                    try:
                        item['signedCopyAfter'] = fingerprint(signed)
                        item['signedCopyUnchanged'] = item['signedCopyAfter'] == {
                            key: value for key, value in item['signedCopy'].items() if key != 'path'}
                        materials.meta.require(item['signedCopyUnchanged'], 'The retained signed evidence copy changed')
                    except (OSError, ValueError, RuntimeError) as error:
                        item['failures'].append(str(error)); report['failures'].append(relative + ': ' + str(error))
                if source in originals:
                    try:
                        item['originalAfter'] = fingerprint(source)
                        item['originalUnchanged'] = item['originalAfter'] == originals[source]
                        materials.meta.require(item['originalUnchanged'], 'The original SDK plugin changed during the probe')
                    except (OSError, ValueError, RuntimeError) as error:
                        item['failures'].append(str(error)); report['failures'].append(relative + ': ' + str(error))
                save()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        report['failures'].append(str(error))
    finally:
        if output is not None:
            checks = []
            for path, before in originals.items():
                try:
                    after = fingerprint(path)
                    checks.append({'path': str(path), 'before': before, 'after': after, 'unchanged': before == after})
                    materials.meta.require(before == after, 'An original SDK/SPDX/tool input changed: ' + str(path))
                except (OSError, ValueError, RuntimeError) as error:
                    report['failures'].append(str(error))
            report['inputChecks'] = checks
            report['allOriginalInputsUnchanged'] = len(checks) == len(originals) and all(
                check['unchanged'] for check in checks)
            report['status'] = 'failed' if report['failures'] else 'observed'
            save()
    if report['failures']:
        print('\n'.join(report['failures']), file=sys.stderr)
        return 1
    print('Recorded three actual SignTool verification/removal comparisons; production acceptance remains unchanged.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
