#!/usr/bin/env python3
"""Exercise the production WinRT bridge against real bounded HTTP and files.

The default ZIP is an explicitly non-installable transport fixture. It proves
the P11a controlled download contract, never Qt product installation or source completeness.
Pass --real-package-directory with actual clean package metadata to additionally
download that exact complete package; development flags are never rewritten.
"""
from __future__ import annotations

import argparse
import copy
from dataclasses import dataclass
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import threading
import time
import types
import zipfile

from version_source import read_version

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'https://github.com/NAMEWTA/huhucapture'
LATEST = '/repos/NAMEWTA/huhucapture/releases/latest'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')


def sha256(value: Path | bytes):
    digest = hashlib.sha256()
    if isinstance(value, bytes):
        digest.update(value)
    else:
        with value.open('rb') as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    return digest.hexdigest()


def length(value: Path | bytes):
    return len(value) if isinstance(value, bytes) else value.stat().st_size


def descriptor(name, value):
    return {'name': name, 'size': length(value), 'sha256': sha256(value)}


def release(version, assets):
    tag = 'v' + version
    return {'draft': False, 'prerelease': False, 'tag_name': tag,
            'html_url': REPOSITORY + '/releases/tag/' + tag,
            'assets': [{'name': name, 'size': length(body), 'digest': 'sha256:' + sha256(body),
                        'browser_download_url': REPOSITORY + '/releases/download/' + tag + '/' + name}
                       for name, body in assets.items()]}


@dataclass
class Response:
    body: Path | bytes = b''
    status: int = 200
    redirect: str = ''
    slow: bool = False
    chunked: bool = False
    body_delta: int = 0
    encoding: str = ''


class Server(ThreadingHTTPServer):
    # server_close joins every request handler, including a real client that
    # cancelled a partial body; no fixture writer is left behind at completion.
    daemon_threads = False

    def __init__(self):
        self.routes = {}
        self.requests = []
        self.lock = threading.Lock()
        super().__init__(('127.0.0.1', 0), Handler)

    def install(self, routes):
        with self.lock:
            self.routes = dict(routes)
            self.requests = []

    def seen(self):
        with self.lock:
            return list(self.requests)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_args):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.requests.append(self.path)
            response = copy.copy(self.server.routes.get(self.path, Response(status=404)))
        try:
            self.send_response(response.status)
            self.send_header('Connection', 'close')
            if response.redirect:
                self.send_header('Location', response.redirect)
            if response.encoding:
                self.send_header('Content-Encoding', response.encoding)
            if response.chunked:
                self.send_header('Transfer-Encoding', 'chunked')
            else:
                self.send_header('Content-Length', str(length(response.body)))
            self.end_headers()
            remaining = max(0, length(response.body) + min(0, response.body_delta))

            def send(chunk):
                if response.chunked:
                    self.wfile.write(f'{len(chunk):x}\r\n'.encode('ascii'))
                self.wfile.write(chunk)
                if response.chunked:
                    self.wfile.write(b'\r\n')
                self.wfile.flush()
                if response.slow:
                    time.sleep(0.004)

            if isinstance(response.body, Path):
                with response.body.open('rb') as stream:
                    while remaining:
                        chunk = stream.read(min(64 * 1024, remaining))
                        if not chunk:
                            break
                        send(chunk)
                        remaining -= len(chunk)
            else:
                offset = 0
                while remaining:
                    chunk = response.body[offset:offset + min(64 * 1024, remaining)]
                    send(chunk)
                    offset += len(chunk)
                    remaining -= len(chunk)
            if response.body_delta > 0:
                send(b'X' * response.body_delta)
            if response.chunked:
                self.wfile.write(b'0\r\n\r\n')
                self.wfile.flush()
        except (OSError, ConnectionError):
            # Cancellation closes the real client stream. The client result and
            # filesystem assertions, rather than this expected server EOF, decide.
            pass
        finally:
            self.close_connection = True


def package_fixture(directory: Path, version: str, executable: Path, commit: str):
    name = f'huhucapture-v{version}-windows-x64.zip'
    path = directory / name
    contents = {'huhucapture.exe': executable.read_bytes(),
                'Qt6Core.dll': b'Non-installable transport fixture; not a Qt binary.\n',
                'platforms/qwindows.dll': b'Non-installable transport fixture; not a Qt plugin.\n',
                'fixture-not-a-product.txt': b'This exercises transport only. Do not distribute or install.\n'}
    inventory = []
    with zipfile.ZipFile(path, 'x', compression=zipfile.ZIP_STORED) as archive:
        for leaf, body in contents.items():
            archive.writestr(leaf, body)
            inventory.append({'path': 'portable-zip/' + leaf, 'size': len(body), 'sha256': sha256(body)})
        padding = bytes(range(256)) * 4096
        digest = hashlib.sha256()
        with archive.open('transport-padding.bin', 'w', force_zip64=True) as stream:
            for _ in range(70):
                stream.write(padding)
                digest.update(padding)
        inventory.append({'path': 'portable-zip/transport-padding.bin', 'size': 70 * len(padding), 'sha256': digest.hexdigest()})
    require(path.stat().st_size > 64 * 1024 * 1024, 'The real streaming input must exceed the old whole-buffer limit')
    base = {'schemaVersion': 1, 'product': 'huhucapture', 'repository': 'NAMEWTA/huhucapture',
            'tag': 'v' + version, 'version': version, 'sourceCommit': commit}
    # These are fixture labels, not a claimed dependency build or release receipt.
    sources = {'qt': 'https://github.com/qt/qtbase', 'gifski': 'https://github.com/ImageOptim/gifski',
               'localsend': 'https://github.com/localsend/localsend', 'ffmpeg': 'https://github.com/FFmpeg/FFmpeg'}
    info = {**base, 'platform': 'windows', 'architecture': 'x64',
            'dependencies': [{'name': key, 'version': 'transport-fixture', 'license': 'fixture-not-distributable', 'source': source}
                             for key, source in sources.items()], 'files': inventory,
            'packages': [{**descriptor(name, path), 'packageKind': 'portable-zip', 'contentRoot': 'portable-zip'}]}
    entry = {**descriptor(name, path), 'platform': 'windows', 'architecture': 'x64',
             'packageKind': 'portable-zip', 'minimumSystem': 'Transport fixture; no supported runtime claimed'}
    return path, info, entry, base


def package_routes(version, package, info, entry, base, *, include_legacy=None):
    info_body = encoded(info)
    manifest = {**base, 'assets': [{**entry, 'buildInfo': descriptor('build-info-windows-x64.json', info_body)}]}
    assets = {package.name: package, 'build-info-windows-x64.json': info_body,
              'release-manifest.json': encoded(manifest)}
    if include_legacy is not None:
        assets['huhucapture.exe'] = include_legacy
    return routes_for(version, assets), manifest


def routes_for(version, assets):
    prefix = '/NAMEWTA/huhucapture/releases/download/v' + version + '/'
    return {LATEST: Response(encoded(release(version, assets))),
            **{prefix + name: Response(body) for name, body in assets.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--legacy-executable', type=Path, required=True)
    parser.add_argument('--bridge-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--real-package-directory', type=Path)
    parser.add_argument('--require-real-package', action='store_true')
    parser.add_argument('--gui-probe', type=Path)
    parser.add_argument('--configuration', choices=('Debug', 'Release'), required=True)
    args = parser.parse_args()
    require(args.probe.is_file() and args.legacy_executable.is_file(), 'Use actual built native probe and old product executable')
    require(not args.require_real_package or args.real_package_directory is not None,
            'Actual Qt product acceptance requires actual clean package artifacts')
    bridge_source = args.bridge_source.resolve(strict=True)
    version = read_version(bridge_source)
    bridge_commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=bridge_source, check=True,
                                   stdout=subprocess.PIPE, text=True, encoding='ascii').stdout.strip()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / 'owned-fixture'
    fixture.mkdir()  # Never silently reuse another run's owned fixture namespace.
    downloads = fixture / '下载目录 with spaces'
    downloads.mkdir()
    protected = fixture / 'existing-profile-and-media'
    protected.mkdir()
    (protected / 'config.json').write_bytes(b'\xff\xfe' + '{"unknown":{"keep":true}}'.encode('utf-16le'))
    (protected / '20261125-120000.png').write_bytes(b'Existing media sentinel; no task may modify this path.\n')
    protected_hashes = {path.name: sha256(path) for path in protected.iterdir()}
    package, info, entry, base = package_fixture(fixture, version, args.legacy_executable, bridge_commit)
    prefix = '/NAMEWTA/huhucapture/releases/download/v' + version + '/'
    package_path = prefix + package.name
    server = Server()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = f'http://127.0.0.1:{server.server_address[1]}'
    reports = []

    def run(name, routes, expected, *, error=None, action='download', cancel_after=None,
            offered_version=version, no_body=None, same_bytes=None, directory=downloads):
        server.install(routes)
        before = {p.name: sha256(p) for p in directory.iterdir()} if directory.is_dir() else {}
        command = [str(args.probe.resolve()), version, origin, str(directory), action]
        if cancel_after is not None:
            command.append(str(cancel_after))
        result = subprocess.run(command, text=True, encoding='utf-8', errors='strict',
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=135)
        (output / (name + '.stdout.jsonl')).write_text(result.stdout, encoding='utf-8')
        (output / (name + '.stderr.log')).write_text(result.stderr, encoding='utf-8')
        require(result.returncode == 0, f'{name}: native probe failed; see actual stdout/stderr')
        messages = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        require(messages and messages[-1].get('joined') is True and messages[-1].get('timedOut') is False,
                f'{name}: task did not actually join')
        final = messages[-1]
        require(final['stage'] == expected, f'{name}: actual result {final}')
        if error is not None:
            require(final['error'] == error, f'{name}: incorrect typed failure {final}')
        for leaf, digest in before.items():
            require((directory / leaf).is_file() and sha256(directory / leaf) == digest, f'{name}: modified existing download')
        require(not directory.exists() or not list(directory.glob('*.part')), f'{name}: left owned unfinished staging')
        require({p.name: sha256(p) for p in protected.iterdir()} == protected_hashes, f'{name}: modified user profile/media')
        seen = server.seen()
        if no_body:
            require(no_body not in seen, f'{name}: requested body after metadata rejection/check-only')
        if expected == 'downloaded':
            saved = Path(final['file'])
            require(saved.parent == directory.resolve() and saved.name not in before and saved.is_file(), f'{name}: missing exclusive final file')
            require(saved.stat().st_size == final['total'] == final['received'], f'{name}: wrong retained file length')
            require(final['version'] == offered_version, f'{name}: wrong selected version')
            if same_bytes is not None:
                require(sha256(saved) == sha256(same_bytes), f'{name}: retained bytes differ')
            saved.unlink()  # Only this run's proven new complete download.
        else:
            require(not final['file'], f'{name}: exposed an uncommitted filename')
        reports.append({'case': name, 'result': final, 'requests': seen})
        print(f'PASS {name}: {final["stage"]}; real native task joined', flush=True)
        return final, messages

    def run_gui(name, routes):
        require(args.gui_probe is not None and args.gui_probe.is_file(), 'Use the actual old application GUI integration executable')
        executable = args.gui_probe.resolve()
        require((executable.parent / 'config.json').is_file(), 'GUI harness must use its own explicit portable configuration')
        for path in (bridge_source / 'Lang').glob('*.json'):
            deployed = executable.parent / 'Lang' / path.name
            require(deployed.is_file() and sha256(deployed) == sha256(path),
                    'Deploy all six actual UTF-16 language files before GUI bridge verification')
        server.install(routes)
        screens = output / name / 'screens'
        screens.mkdir(parents=True)
        result = subprocess.run([str(executable), str(screens), '--enter=tray', '--verify-update-bridge=' + origin],
                                cwd=executable.parent, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding='utf-8', errors='replace', timeout=100)
        (output / (name + '.stdout.log')).write_text(result.stdout, encoding='utf-8')
        (output / (name + '.stderr.log')).write_text(result.stderr, encoding='utf-8')
        require(result.returncode != 77, f'{name}: native graphics/input desktop unavailable; this check did not pass')
        require(result.returncode == 0, f'{name}: actual native About integration failed; inspect retained logs')
        for language in ('en-US', 'zh-CN', 'zh-TW', 'ja-JP', 'ru-RU', 'id-ID'):
            image = screens / ('21-package-instructions-' + language + '.png')
            require(image.is_file() and image.stat().st_size > 0, f'{name}: missing actual six-language About evidence')
        reports.append({'case': name, 'result': 'passed', 'requests': server.seen(),
                        'scope': 'Actual About + Downloads + close/reopen + six translated minimum-size windows'})
        print(f'PASS {name}: actual shared application service and old About controls', flush=True)

    try:
        legacy = routes_for(version, {'huhucapture.exe': args.legacy_executable})
        legacy_name = f'huhucapture-v{version}-windows-x64.exe'
        (downloads / legacy_name).write_bytes(b'Existing same-name download must survive.\n')
        run('legacy-same-version-and-collision', legacy, 'downloaded', same_bytes=args.legacy_executable)
        run('legacy-repeat-download', legacy, 'downloaded', same_bytes=args.legacy_executable)
        run('legacy-current-check', legacy, 'idle', action='check', no_body=prefix + 'huhucapture.exe')
        parts = [int(x) for x in version.split('.')]
        newer = f'{parts[0]}.{parts[1]}.{parts[2]+1}'
        newer_routes = routes_for(newer, {'huhucapture.exe': args.legacy_executable})
        run('legacy-newer-metadata-check', newer_routes, 'available', action='check',
            no_body='/NAMEWTA/huhucapture/releases/download/v' + newer + '/huhucapture.exe')
        run('legacy-mismatched-pe-version', newer_routes, 'failed', error=4)

        routes, _ = package_routes(version, package, info, entry, base)
        (downloads / package.name).write_bytes(b'Existing complete-package filename must survive.\n')
        final, progress = run('package-bounded-stream-and-collision', routes, 'downloaded', same_bytes=package)
        require(final['packageKind'] == 'portable-zip', 'Bridge did not preserve portable installation form')
        require(final['peakAdditionalWorkingSet'] <= 64 * 1024 * 1024,
                f'Native streaming exceeded 64 MiB additional working-set budget: {final}')
        require(any(0 < x['received'] < x['total'] for x in progress), 'No real intermediate progress was exposed')
        run('package-repeat-download', routes, 'downloaded', same_bytes=package)
        run('package-current-check', routes, 'idle', action='check', no_body=package_path)
        run('cancel-before-network', routes, 'cancelled', action='cancel-first')
        slow = copy.deepcopy(routes)
        slow[package_path].slow = True
        final, _ = run('cancel-real-stream', slow, 'cancelled', cancel_after=128 * 1024)
        require(final['milliseconds'] < 15000, 'Actual cancellation did not release the request promptly')

        for label, change in (
            ('wrong-content-root', lambda value: value['packages'][0].update(contentRoot='installer')),
            ('missing-qt-plugin', lambda value: value.update(files=[x for x in value['files'] if 'qwindows.dll' not in x['path']])),
            ('traversal-path', lambda value: value['files'][0].update(path='portable-zip/../huhucapture.exe')),
            ('case-collision', lambda value: value['files'].append({**value['files'][0], 'path': value['files'][0]['path'].upper()})),
            ('development-build', lambda value: value.update(developmentOnly=True)),
            ('dirty-build', lambda value: value.update(sourceDirty=True)),
            ('malformed-build-flag', lambda value: value.update(sourceDirty='false')),
            ('missing-dependency', lambda value: value.update(dependencies=[x for x in value['dependencies'] if x['name'] != 'qt'])),
        ):
            bad = copy.deepcopy(info)
            change(bad)
            altered, _ = package_routes(version, package, bad, entry, base, include_legacy=args.legacy_executable)
            run(label, altered, 'failed', error=2, no_body=package_path)
            require(prefix + 'huhucapture.exe' not in server.seen(), f'{label}: invalid manifest downgraded to legacy EXE')
        bad_entry = {**entry, 'architecture': 'arm64'}
        altered, _ = package_routes(version, package, info, bad_entry, base)
        run('wrong-package-architecture', altered, 'failed', error=2, no_body=package_path)
        altered, _ = package_routes(version, package, info, entry, {**base, 'repository': 'other/product'})
        run('wrong-package-repository', altered, 'failed', error=2, no_body=package_path)

        missing = copy.deepcopy(routes)
        missing[prefix + 'release-manifest.json'] = Response(status=404)
        run('manifest-unavailable-no-fallback', missing, 'failed', no_body=package_path)
        rejected = copy.deepcopy(routes)
        rejected[package_path] = Response(status=302, redirect='http://127.0.0.1:1/untrusted')
        run('redirect-origin-rejection', rejected, 'failed', error=2)
        for label, delta in [('short-body', -1), ('overlong-body', 1)]:
            changed = copy.deepcopy(routes)
            changed[package_path].chunked = True
            changed[package_path].body_delta = delta
            run(label, changed, 'failed', error=4)
        changed = copy.deepcopy(routes)
        changed[package_path].encoding = 'gzip'
        run('encoded-body-rejected', changed, 'failed', error=4)
        changed = copy.deepcopy(routes)
        corrupt = fixture / 'corrupt.zip'
        with package.open('rb') as source, corrupt.open('xb') as target:
            first = bytearray(source.read(64 * 1024)); first[-1] ^= 1; target.write(first)
            while chunk := source.read(1024 * 1024):
                target.write(chunk)
        changed[package_path].body = corrupt
        run('sha256-rejection', changed, 'failed', error=4)
        run('unavailable-downloads', routes, 'failed', error=3, directory=fixture / 'missing-directory')
        if args.gui_probe is not None:
            slow = copy.deepcopy(routes)
            slow[package_path].slow = True
            run_gui('native-about-transport-fixture', slow)

        actual_result = 'not-run: no complete production package supplied'
        if args.real_package_directory is not None:
            directory = args.real_package_directory.resolve()
            manifest_path = directory / 'release-manifest.json'
            path = ROOT / 'scripts/package-qt.py'
            packager = types.ModuleType('huhucapture_actual_release')
            packager.__file__ = str(path)
            exec(compile(path.read_bytes(), str(path), 'exec'), packager.__dict__)
            source_commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, check=True,
                                           stdout=subprocess.PIPE, text=True, encoding='ascii').stdout.strip()
            value, verified_assets = packager.verify_aggregate(directory, expected_commit=source_commit,
                                                                expected_version=read_version(ROOT))
            require(value.get('developmentOnly') is False and value.get('sourceDirty') is False,
                    'Only the unchanged complete formal aggregate is eligible for the stable bridge')
            selected = [x for x in value['assets'] if x['platform'] == 'windows' and x['architecture'] == 'x64' and x['packageKind'] == 'portable-zip']
            require(len(selected) == 1, 'Expected one actual complete Windows portable package')
            asset = selected[0]
            body = directory / asset['name']
            build = directory / asset['buildInfo']['name']
            require(descriptor(body.name, body) == {k: asset[k] for k in ('name', 'size', 'sha256')}, 'Actual package bytes changed')
            require(descriptor(build.name, build) == asset['buildInfo'], 'Actual build-info bytes changed')
            assets = {name: directory / name for name in verified_assets}
            actual_routes = routes_for(value['version'], assets)
            actual_routes['/NAMEWTA/huhucapture/releases/download/' + value['tag'] + '/' + asset['name']].slow = True
            run('actual-complete-package', actual_routes, 'downloaded', same_bytes=body, offered_version=value['version'])
            if args.gui_probe is not None:
                run_gui('native-about-actual-complete-package', actual_routes)
            actual_result = {'package': str(body), 'sha256': sha256(body), 'manifestSource': str(manifest_path),
                             'manifestSha256': sha256(manifest_path), 'sourceCommit': value['sourceCommit'],
                             'version': value['version'], 'tag': value['tag'], 'actualAboutPassed': args.gui_probe is not None,
                             'scope': 'Exact full-package transport; install/upgrade/uninstall remain independent checks'}
        report = {'schemaVersion': 1, 'purpose': 'legacy-windows-bridge-contract',
                  'repository': 'NAMEWTA/huhucapture', 'sourceCommit': base['sourceCommit'],
                  'version': version, 'tag': 'v' + version, 'configuration': args.configuration,
                  'nativeExecutable': descriptor('huhucapture.exe', args.legacy_executable),
                  'probe': descriptor(args.probe.name, args.probe),
                  'controlledPackageFixture': descriptor(package.name, package),
                  'controlledPackageContractPassed': True,
                  'nativeAboutFixturePassed': args.gui_probe is not None,
                  'transportFixtureIncluded': True,
                  'actualCompletePackageRun': args.real_package_directory is not None,
                  'actualCompletePackage': actual_result, 'cases': reports}
        (output / 'result.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        print('Production WinRT controlled bridge contract passed. Qt product installation/source acceptance remains independently recorded.', flush=True)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        require(not thread.is_alive(), 'HTTP fixture server did not finish shutdown')


if __name__ == '__main__':
    main()
