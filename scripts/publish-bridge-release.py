#!/usr/bin/env python3
"""Validate/publish the one explicitly declared P11a legacy Windows bridge.

This exception publishes only the retained five Windows assets. Its controlled
HTTP fixture proves the new download contract; it does not assert that a Qt
product, corresponding source bundle, or Linux release has passed acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import types
import zipfile

from version_source import read_version

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'NAMEWTA/huhucapture'
PURPOSE = 'legacy-windows-bridge'
RELEASE_MARKER = '<!-- huhucapture:legacy-windows-bridge:v1 -->'
SHA = re.compile(r'[0-9a-f]{40}')
VERSION = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)')
meta_path = ROOT / 'build/package/metadata.py'
meta = types.ModuleType('bridge_asset_metadata')
meta.__file__ = str(meta_path)
exec(compile(meta_path.read_bytes(), str(meta_path), 'exec'), meta.__dict__)
require = meta.require


def command(arguments, *, payload=None):
    result = subprocess.run(arguments, cwd=ROOT, input=payload, text=True, encoding='utf-8',
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=1800)
    require(result.returncode == 0, f'Command failed: {arguments[:2]}\n{result.stderr}')
    return result.stdout


def api(endpoint, *, method='GET', body=None, pages=False):
    args = ['gh', 'api', f'repos/{REPOSITORY}/{endpoint}', '--method', method]
    if pages:
        args += ['--paginate', '--slurp']
    payload = None
    if body is not None:
        args += ['--input', '-']
        payload = json.dumps(body, ensure_ascii=False)
    return json.loads(command(args, payload=payload))


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def descriptor(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), 'Expected a real, unlinked release file')
    return {'name': path.name, 'size': path.stat().st_size, 'sha256': sha256(path)}


def load_json(path, limit=8 * 1024 * 1024):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= limit,
            'Missing, linked, empty or oversized JSON evidence')
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    require(isinstance(value, dict), 'JSON evidence must have an object root')
    return value


def declaration(source_commit, *, root=ROOT, version=None):
    require(isinstance(source_commit, str) and SHA.fullmatch(source_commit), 'An exact source SHA is required')
    version = read_version(root) if version is None else version
    value = load_json(root / 'releases/bridge-publish.json', 16384)
    require(set(value) == {'schemaVersion', 'kind', 'tag', 'notes', 'previousRelease'} and
            type(value['schemaVersion']) is int and value['schemaVersion'] == 1 and value['kind'] == PURPOSE,
            'A separate explicit legacy-windows-bridge declaration is required')
    require(value['tag'] == 'v' + version and value['notes'] == f'releases/v{version}.md',
            'Bridge declaration differs from the single product version or matching notes')
    previous = value['previousRelease']
    require(isinstance(previous, str) and previous.startswith('v') and VERSION.fullmatch(previous[1:]) and
            VERSION.fullmatch(version) and tuple(map(int, version.split('.'))) > tuple(map(int, previous[1:].split('.'))),
            'Bridge must explicitly supersede an older stable release, never reuse its version')
    notes = root / value['notes']
    require(notes.is_file() and not notes.is_symlink() and 0 < notes.stat().st_size <= 1024 * 1024,
            'Matching bridge notes are missing, linked or oversized')
    body = notes.read_text(encoding='utf-8')
    require(body.strip() and RELEASE_MARKER not in body, 'Bridge notes are empty or contain a reserved publication marker')
    return value, body


def names(version):
    return {'huhucapture.exe', f'huhucapture-v{version}-windows-x64.zip',
            f'huhucapture-v{version}-windows-x64-symbols.zip', 'build-info.json', 'SHA256SUMS.txt'}


def archive_files(path, maximum):
    """Read actual ZIP members within the old package's bounded file budget."""
    with zipfile.ZipFile(path) as archive:
        members = archive.infolist()
        require(0 < len(members) <= 10000, 'Legacy ZIP has an invalid member count')
        result = {}
        folded = set()
        total = 0
        for member in members:
            raw = member.filename
            relative = PurePosixPath(raw)
            require('\\' not in raw and not relative.is_absolute() and
                    all(part not in ('..', '.') for part in relative.parts) and ':' not in raw,
                    'Legacy ZIP contains an unsafe path')
            if member.is_dir():
                continue
            require(raw not in result and raw.casefold() not in folded and
                    ((member.external_attr >> 16) & 0o170000) != 0o120000 and not (member.flag_bits & 1),
                    'Legacy ZIP has a duplicate, linked or encrypted member')
            total += member.file_size
            require(0 < member.file_size <= maximum and total <= maximum, 'Legacy ZIP exceeds its uncompressed budget')
            with archive.open(member) as stream:
                result[raw] = {'size': member.file_size, 'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}
            folded.add(raw.casefold())
        return result


def verify_assets(directory, source_commit, version, *, root=ROOT):
    directory = Path(directory)
    require(directory.is_dir() and not directory.is_symlink(), 'Expected the original old Windows package directory')
    require({path.name for path in directory.iterdir()} == names(version),
            'Bridge publication accepts exactly the existing five Windows assets; no Qt/Linux assets')
    assets = {name: descriptor(directory / name) for name in names(version)}
    require(0 < assets['huhucapture.exe']['size'] <= 64 * 1024 * 1024,
            'Bridge EXE must remain directly downloadable by v0.0.5')
    require(meta.machine(directory / 'huhucapture.exe') == 'windows', 'Bridge must be the actual x64 Windows executable')
    info = load_json(directory / 'build-info.json', 16 * 1024 * 1024)
    require(info.get('repository') == REPOSITORY and info.get('commit') == source_commit and
            info.get('tag') == 'v' + version and info.get('executableVersion') == version + '.0' and
            info.get('architecture') == 'x64' and info.get('configuration') == 'Release' and
            info.get('toolset') == 'MSVC v145' and info.get('runtime') == 'static CRT (/MT)' and
            info.get('executableSha256') == assets['huhucapture.exe']['sha256'],
            'Bridge build-info does not describe these actual legacy Release bytes and source')
    require(info.get('dependencies') == load_json(root / 'build/dependencies.lock.json'),
            'Bridge dependency lock differs from the tested checkout')
    require(assets['SHA256SUMS.txt']['size'] <= 1024 * 1024, 'Legacy checksum manifest is oversized')
    checksums = {}
    for line in (directory / 'SHA256SUMS.txt').read_text(encoding='utf-8-sig').splitlines():
        matched = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9._-]+)', line)
        require(matched is not None and matched[2] not in checksums, 'Invalid or duplicate legacy checksum entry')
        checksums[matched[2]] = matched[1]
    require(set(checksums) == names(version) - {'SHA256SUMS.txt'} and
            all(checksums[name] == assets[name]['sha256'] for name in checksums), 'Legacy asset checksum mismatch')
    portable_name = f'huhucapture-v{version}-windows-x64.zip'
    require(assets[portable_name]['size'] <= 512 * 1024 * 1024, 'Legacy portable ZIP exceeds its budget')
    portable = archive_files(directory / portable_name, 512 * 1024 * 1024)
    for name in ('huhucapture.exe', 'build-info.json'):
        require(portable.get(name) == {key: assets[name][key] for key in ('size', 'sha256')},
                'Legacy portable ZIP does not contain the identical executable/build-info')
    languages = list((root / 'Lang').glob('*.json'))
    require(len(languages) == 6, 'The checkout must retain all six actual language files')
    for language in languages:
        require(portable.get('Lang/' + language.name) == {key: descriptor(language)[key] for key in ('size', 'sha256')},
                'Legacy portable ZIP changed or omitted an actual language resource')
    required_notices = {'NOTICE.md', 'licenses/huhucapture-LICENSE.txt', 'licenses/Ling-LICENSE.txt',
                        'licenses/gifski-LICENSE.txt', 'licenses/Yoga-LICENSE.txt',
                        'licenses/quirc-LICENSE.txt', 'licenses/cgif-LICENSE.txt'}
    require(required_notices <= portable.keys(), 'Legacy portable ZIP lacks retained dependency notices')
    require(not any(name.lower().endswith(('.dll', '.appimage', '.deb')) for name in portable),
            'Bridge portable payload must remain the existing standalone legacy application')
    symbols_name = f'huhucapture-v{version}-windows-x64-symbols.zip'
    require(assets[symbols_name]['size'] <= 2 * 1024 * 1024 * 1024, 'Legacy symbols ZIP exceeds its budget')
    symbols = archive_files(directory / symbols_name, 2 * 1024 * 1024 * 1024)
    require('huhucapture.pdb' in symbols and all('/' not in name and name.endswith('.pdb') for name in symbols),
            'Legacy symbols must be the actual matching Windows PDB archive')
    return assets


def verify_report(report, configuration, source_commit, version, executable):
    require(report.get('schemaVersion') == 1 and type(report.get('schemaVersion')) is int and
            report.get('purpose') == 'legacy-windows-bridge-contract' and report.get('repository') == REPOSITORY and
            report.get('sourceCommit') == source_commit and report.get('version') == version and
            report.get('tag') == 'v' + version and report.get('configuration') == configuration and
            report.get('nativeExecutable') == executable,
            'Bridge contract evidence differs from the actual source/version/legacy executable')
    require(report.get('controlledPackageContractPassed') is True and report.get('transportFixtureIncluded') is True and
            report.get('actualCompletePackageRun') is False,
            'Independent bridge release requires its explicit controlled fixture scope, not a relabelled Qt candidate')
    fixture = report.get('controlledPackageFixture', {})
    require(isinstance(fixture, dict) and fixture.get('name') == f'huhucapture-v{version}-windows-x64.zip' and
            type(fixture.get('size')) is int and 64 * 1024 * 1024 < fixture['size'] <= 512 * 1024 * 1024 and
            isinstance(fixture.get('sha256'), str) and re.fullmatch('[0-9a-f]{64}', fixture['sha256']),
            'Controlled package fixture must exercise the real stream beyond the old whole-buffer limit')
    cases = report.get('cases')
    require(isinstance(cases, list) and all(isinstance(item, dict) and isinstance(item.get('case'), str) for item in cases),
            'Bridge evidence lacks actual native case results')
    indexed = {item['case']: item for item in cases}
    require(len(indexed) == len(cases), 'Duplicate bridge case evidence')
    expected = {
        'legacy-same-version-and-collision': 'downloaded', 'legacy-repeat-download': 'downloaded',
        'legacy-current-check': 'idle', 'legacy-newer-metadata-check': 'available',
        'legacy-mismatched-pe-version': 'failed', 'package-bounded-stream-and-collision': 'downloaded',
        'package-repeat-download': 'downloaded', 'package-current-check': 'idle',
        'cancel-before-network': 'cancelled', 'cancel-real-stream': 'cancelled',
        **{name: 'failed' for name in ('wrong-content-root', 'missing-qt-plugin', 'traversal-path', 'case-collision',
            'development-build', 'dirty-build', 'malformed-build-flag', 'missing-dependency',
            'wrong-package-architecture', 'wrong-package-repository', 'manifest-unavailable-no-fallback',
            'redirect-origin-rejection', 'short-body', 'overlong-body', 'encoded-body-rejected',
            'sha256-rejection', 'unavailable-downloads')},
    }
    require(expected.keys() <= indexed.keys(), 'One or more retained native bridge contract cases did not execute')
    for name, stage in expected.items():
        result = indexed[name].get('result')
        require(isinstance(result, dict) and result.get('stage') == stage and result.get('joined') is True and
                result.get('timedOut') is False, 'Bridge case failed or did not drain: ' + name)
    result = indexed['package-bounded-stream-and-collision']['result']
    require(result.get('packageKind') == 'portable-zip' and result.get('total') == fixture['size'] and
            result.get('received') == fixture['size'] and
            type(result.get('peakAdditionalWorkingSet')) is int and result['peakAdditionalWorkingSet'] <= 64 * 1024 * 1024,
            'Controlled complete-package download did not retain all streamed bytes within its measured memory budget')
    if configuration == 'Release':
        require(report.get('nativeAboutFixturePassed') is True and
                indexed.get('native-about-transport-fixture', {}).get('result') == 'passed',
                'Release bridge lacks the actual About close/reopen and six-language window journey')
    else:
        require(report.get('nativeAboutFixturePassed') is False, 'Debug evidence must retain its actual transport-only scope')


def verify_evidence(directory, source_commit, version, executable):
    result = {}
    for configuration in ('Debug', 'Release'):
        candidates = list((directory / 'legacy-update' / configuration).glob('run-*/result.json'))
        require(len(candidates) == 1, 'Expected one exact original bridge report per native configuration')
        report = load_json(candidates[0])
        verify_report(report, configuration, source_commit, version, executable)
        probe = directory / 'legacy-update' / configuration / 'LegacyUpdateProbe.exe'
        require(report.get('probe') == descriptor(probe), 'Retained native bridge probe differs from the actually exercised executable')
        if configuration == 'Release':
            screens = candidates[0].parent / 'native-about-transport-fixture/screens'
            for language in ('en-US', 'zh-CN', 'zh-TW', 'ja-JP', 'ru-RU', 'id-ID'):
                path = screens / ('21-package-instructions-' + language + '.png')
                require(path.is_file() and not path.is_symlink() and path.stat().st_size > 0,
                        'Release bridge lacks actual native language/window evidence')
        result[configuration] = descriptor(candidates[0])
    return result


def verify_windows_run(source_commit, run_id):
    require(isinstance(run_id, str) and run_id.isdecimal(), 'The successful old Windows workflow run ID is required')
    value = api(f'actions/runs/{run_id}')
    require(value.get('head_sha') == source_commit and value.get('head_branch') == 'main' and
            value.get('path') == '.github/workflows/windows.yml' and
            (value.get('head_repository') or {}).get('full_name') == REPOSITORY and
            value.get('status') == 'completed' and value.get('conclusion') == 'success' and
            value.get('event') in ('push', 'workflow_dispatch'),
            'Bridge requires a successful canonical main Windows run for the exact source')
    jobs = []
    for page in api(f'actions/runs/{run_id}/jobs?per_page=100', pages=True):
        jobs += page['jobs']
    builds = [job for job in jobs if job.get('name') == 'build']
    require(len(builds) == 1 and builds[0].get('conclusion') == 'success', 'The retained Windows build did not pass')
    steps = {step['name']: step.get('conclusion') for step in builds[0].get('steps', [])}
    required = ('Build the release executable and pinned dependencies', 'Run core regression tests',
        'Verify native annotation rendering and PNG export', 'Check single-executable tray startup',
        'Verify the debug configuration', 'Verify the Debug legacy updater bridge with real HTTP and files',
        'Package and verify the single executable', 'Verify real annotation editor interactions and clipboard export',
        'Verify Release bridge streaming and six-language About interactions', 'Verify latest release download into Windows Downloads')
    require(all(steps.get(name) == 'success' for name in required), 'A required native bridge production gate was skipped or failed')
    artifacts = []
    for page in api(f'actions/runs/{run_id}/artifacts?per_page=100', pages=True):
        artifacts += page['artifacts']
    selected = {}
    for name in ('windows-x64', 'windows-render-checks'):
        matches = [item for item in artifacts if item.get('name') == name and item.get('expired') is False]
        require(len(matches) == 1 and matches[0].get('size_in_bytes', 0) > 0,
                'The actual Windows package/native evidence artifact is absent or ambiguous')
        selected[name] = matches[0]['id']
    return selected


def context(source_commit):
    require(os.environ.get('GITHUB_REPOSITORY') == REPOSITORY and os.environ.get('GITHUB_REF') == 'refs/heads/main' and
            os.environ.get('GITHUB_EVENT_NAME') == 'workflow_dispatch',
            'Bridge publication is only available from the explicit canonical main dispatch')
    require(command(['git', 'rev-parse', 'HEAD']).strip() == source_commit and
            not command(['git', 'status', '--porcelain', '--untracked-files=normal']).strip(),
            'Bridge publication requires the exact clean tested checkout')
    require(api('git/ref/heads/main')['object']['sha'] == source_commit, 'Main changed since the bridge was validated')


def publish(args, value, body, assets):
    context(args.source_commit)
    verify_windows_run(args.source_commit, args.windows_run)
    releases = []
    for page in api('releases?per_page=100', pages=True):
        releases += page
    require(not any(item.get('tag_name') == value['tag'] for item in releases), 'Bridge tag already has a release/draft; no rewrite is allowed')
    require(not any(RELEASE_MARKER in (item.get('body') or '') for item in releases),
            'The one bridge transition already has a release/draft; inspect it before any further publication')
    latest = api('releases/latest')
    require(latest.get('tag_name') == value['previousRelease'] and latest.get('draft') is False and
            latest.get('prerelease') is False and any(item.get('name') == 'huhucapture.exe' for item in latest.get('assets', [])),
            'The declared old standalone release must still be latest before the bridge transition')
    refs = api('git/matching-refs/tags/' + value['tag'])
    require(not any(item.get('ref') == 'refs/tags/' + value['tag'] for item in refs), 'Existing tags are never reused or moved')
    context(args.source_commit)
    api('git/refs', method='POST', body={'ref': 'refs/tags/' + value['tag'], 'sha': args.source_commit})
    released = api('releases', method='POST', body={'tag_name': value['tag'], 'target_commitish': args.source_commit,
        'name': 'huhucapture ' + value['tag'], 'body': body + '\n\n' + RELEASE_MARKER + '\n',
        'draft': True, 'prerelease': False, 'make_latest': 'false'})
    require(released.get('draft') is True and released.get('tag_name') == value['tag'], 'Bridge draft was not created as requested')
    print(f'Created unpublished bridge draft {released["id"]}; failure retains it and the new tag for explicit recovery.', flush=True)
    command(['gh', 'release', 'upload', value['tag'], *[str(args.package / name) for name in sorted(assets)], '--repo', REPOSITORY])
    uploaded = []
    for page in api(f'releases/{released["id"]}/assets?per_page=100', pages=True):
        uploaded += page
    observed = {item['name']: item for item in uploaded}
    require(len(observed) == len(uploaded) and set(observed) == set(assets), 'Draft asset set differs from the five verified Windows assets')
    for name, expected in assets.items():
        item = observed[name]
        require(item.get('state') == 'uploaded' and item.get('size') == expected['size'] and
                item.get('digest') == 'sha256:' + expected['sha256'], 'Uploaded bridge bytes differ: ' + name)
    require(verify_assets(args.package, args.source_commit, value['tag'][1:]) == assets,
            'Bridge package changed during upload')
    context(args.source_commit)
    require(api('git/ref/tags/' + value['tag'])['object']['sha'] == args.source_commit and
            api('releases/latest').get('id') == latest['id'], 'Source/tag/latest changed; the bridge remains unpublished')
    final = api(f'releases/{released["id"]}', method='PATCH', body={'draft': False, 'prerelease': False, 'make_latest': 'true'})
    require(final.get('draft') is False and final.get('prerelease') is False and
            api('releases/latest').get('id') == released['id'], 'Published bridge was not observed as stable latest')
    print(json.dumps({'release': final['html_url'], 'sourceCommit': args.source_commit, 'windowsRun': args.windows_run,
                      'kind': PURPOSE, 'assetCount': len(assets)}, ensure_ascii=True, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('admit', 'verify', 'publish'))
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--windows-run', default='')
    parser.add_argument('--package', type=Path)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    try:
        value, body = declaration(args.source_commit)
        if args.command == 'admit':
            context(args.source_commit)
            selected = verify_windows_run(args.source_commit, args.windows_run)
            require('GITHUB_OUTPUT' in os.environ, 'Admission requires the real Actions output file')
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as stream:
                stream.write(f'source_commit={args.source_commit}\npackage_id={selected["windows-x64"]}\nevidence_id={selected["windows-render-checks"]}\n')
            print('Admitted the exact successful legacy Windows artifacts; no Qt gate or publication was performed.')
            return 0
        require(args.package is not None and args.evidence is not None, 'Original package and native evidence directories are required')
        args.package = args.package.resolve()
        assets = verify_assets(args.package, args.source_commit, value['tag'][1:])
        verify_evidence(args.evidence.resolve(), args.source_commit, value['tag'][1:], assets['huhucapture.exe'])
        if args.command == 'publish':
            require(os.name == 'nt', 'Final bridge publishing must run the native Windows version/resource gate')
            # Direct Python use must not bypass the same actual PE resource check.
            executable = args.package / 'huhucapture.exe'
            command(['pwsh', '-NoProfile', '-NonInteractive', '-File', str(ROOT / 'scripts/verify-bridge-native.ps1'),
                     '-Executable', str(executable), '-Version', value['tag'][1:]])
            publish(args, value, body, assets)
        else:
            print('Verified five exact legacy Windows assets and Debug/Release controlled bridge evidence. No publication performed.')
        return 0
    except (ValueError, RuntimeError, OSError, KeyError, TypeError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print('Bridge release refused; existing tags/assets retained:', error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
