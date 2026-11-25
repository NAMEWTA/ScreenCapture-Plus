#!/usr/bin/env python3
"""Exercise the actual WinRT bridge over real loopback HTTP, without Qt/Ling.

Default payloads are explicitly labelled transport fixtures, not installable
products. --release-fixture additionally tests unmodified, clean complete-package
release metadata and bytes when that independent release gate is available.
"""
import argparse
import copy
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import threading
import time
import zipfile


REPOSITORY = 'NAMEWTA/huhucapture'
API_PATH = '/repos/' + REPOSITORY + '/releases/latest'
ASSET_PATH = '/' + REPOSITORY + '/releases/download/'
SCOPE = b'Transport-contract fixture; not an installable product.\n'


def require(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')


def descriptor(name, content):
    return {'name': name, 'size': content.stat().st_size if isinstance(content, Path) else len(content),
            'sha256': sha(content) if isinstance(content, Path) else hashlib.sha256(content).hexdigest()}


def fixture(package, *, tag='v0.0.5', kind='portable-zip', legacy=False):
    name = ('huhucapture.exe' if legacy else 'huhucapture-' + tag + '-windows-x64' +
            ('-setup.exe' if kind == 'installer' else '.zip'))
    identity = {'schemaVersion': 1, 'product': 'huhucapture', 'repository': REPOSITORY,
                'tag': tag, 'version': tag[1:], 'sourceCommit': '1' * 40,
                'developmentOnly': False, 'sourceDirty': False,
                'testScope': 'Transport fixture only; no product or dependency distribution is claimed.'}
    package_descriptor = {**descriptor(name, package), 'packageKind': kind}
    dependencies = [{'name': dependency, 'version': 'fixture-only', 'license': 'Transport fixture; no dependency distribution',
                     'source': 'https://github.com/' + REPOSITORY} for dependency in ('qt', 'gifski', 'localsend', 'ffmpeg')]
    files = [{'path': path, 'size': len(SCOPE), 'sha256': hashlib.sha256(SCOPE).hexdigest()}
             for path in ('huhucapture.exe', 'Qt6Core.dll', 'plugins/platforms/qwindows.dll')]
    build = {**identity, 'platform': 'windows', 'architecture': 'x64', 'dependencies': dependencies,
             'files': files, 'packages': [package_descriptor]}
    manifest = {**identity, 'assets': [{**package_descriptor, 'platform': 'windows', 'architecture': 'x64',
                                      'minimumSystem': 'Windows 10 1809; transport test only'}]}
    return {'tag': tag, 'name': name, 'payload': package, 'build': build, 'manifest': manifest, 'legacy': legacy}


def publish(value, *, legacy_payload=None):
    tag = value['tag']
    resources = {value['name']: value['payload']}
    if not value['legacy']:
        build = encode(value['build'])
        value['manifest']['assets'][0]['buildInfo'] = descriptor('build-info-windows-x64.json', build)
        resources['build-info-windows-x64.json'] = build
        resources['release-manifest.json'] = encode(value['manifest'])
    if legacy_payload is not None:
        resources['huhucapture.exe'] = legacy_payload
    assets = []
    for name, payload in resources.items():
        actual = descriptor(name, payload)
        assets.append({'name': name, 'size': actual['size'], 'digest': 'sha256:' + actual['sha256'],
                       'browser_download_url': 'https://github.com' + ASSET_PATH + tag + '/' + name})
    release = {'draft': False, 'prerelease': False, 'tag_name': tag,
               'html_url': 'https://github.com/' + REPOSITORY + '/releases/tag/' + tag, 'assets': assets}
    routes = {ASSET_PATH + tag + '/' + name: {'body': body} for name, body in resources.items()}
    routes[API_PATH] = {'body': encode(release)}
    return routes


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, routes):
        self.routes = routes
        self.requests = []
        self.stop_stall = threading.Event()
        self.request_lock = threading.Lock()
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, kwargs={'poll_interval': .05}, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop_stall.set()
        self.shutdown()
        self.server_close()
        self.thread.join(timeout=5)

    @property
    def origin(self):
        return 'http://127.0.0.1:' + str(self.server_port)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        pass

    def do_GET(self):
        with self.server.request_lock:
            self.server.requests.append(self.path)
        response = self.server.routes.get(self.path, {'body': b'Missing fixture', 'status': 404})
        body = response.get('body', b'')
        size = body.stat().st_size if isinstance(body, Path) else len(body)
        self.send_response(response.get('status', 200))
        self.send_header('Connection', 'close')
        self.send_header('Content-Type', 'application/octet-stream')
        if response.get('chunked'):
            self.send_header('Transfer-Encoding', 'chunked')
        else:
            self.send_header('Content-Length', str(response.get('declaredLength', size)))
        if 'redirect' in response:
            self.send_header('Location', response['redirect'])
        self.end_headers()
        stream = body.open('rb') if isinstance(body, Path) else None
        try:
            position = 0
            while position < size:
                chunk = stream.read(64 * 1024) if stream else body[position:position + 64 * 1024]
                if 'truncateAt' in response:
                    chunk = chunk[:max(0, response['truncateAt'] - position)]
                if not chunk:
                    break
                if response.get('chunked'):
                    self.wfile.write(format(len(chunk), 'x').encode() + b'\r\n' + chunk + b'\r\n')
                else:
                    self.wfile.write(chunk)
                self.wfile.flush()
                position += len(chunk)
                if response.get('stall'):
                    self.server.stop_stall.wait(timeout=15)
                    return
            if response.get('chunked'):
                self.wfile.write(b'0\r\n\r\n')
        except (ConnectionError, OSError):
            pass  # A tested cancellation intentionally closes the real socket.
        finally:
            if stream:
                stream.close()
            self.close_connection = True


def invoke(client, root, name, routes, expected, *, mode='download', prepare=None):
    case = root / name
    case.mkdir()
    downloads = case / 'Downloads with spaces 下载'
    downloads.mkdir()
    if prepare:
        prepare(downloads)
    report = case / 'result.json'
    with FixtureServer(routes) as server:
        process = subprocess.run([str(client), server.origin, str(downloads), str(report), mode],
                                 check=True, capture_output=True, text=True, timeout=75)
        value = json.loads(report.read_text(encoding='utf-8'))
        requests = list(server.requests)
    require(value['stage'] == expected, name + ': expected ' + expected + ', got ' + repr(value))
    require(not list(downloads.glob('*.part')), name + ': native worker did not clean owned staging')
    if expected in ('failed', 'cancelled'):
        require(not list(downloads.iterdir()), name + ': failed request published or retained an unexpected file')
    (case / 'requests.json').write_text(json.dumps(requests, indent=2), encoding='utf-8')
    (case / 'stdout.txt').write_text(process.stdout + process.stderr, encoding='utf-8')
    print(name + ': ' + value['stage'], flush=True)
    return value, requests


def transport_suite(client, root):
    small = root / 'transport-only.zip'
    with zipfile.ZipFile(small, 'x', compression=zipfile.ZIP_STORED) as archive:
        for name in ('huhucapture.exe', 'Qt6Core.dll', 'plugins/platforms/qwindows.dll'):
            archive.writestr(name, SCOPE)
    baseline = fixture(small)
    package_path = ASSET_PATH + baseline['tag'] + '/' + baseline['name']
    value, _ = invoke(client, root, 'same-version-complete-zip', publish(copy.deepcopy(baseline)), 'downloaded')
    require(value['packageKind'] == 'portable-zip' and sha(Path(value['file'])) == sha(small), 'ZIP bytes changed during commit')
    sentinel = b'Existing user file; must not be replaced.'
    value, _ = invoke(client, root, 'collision-preserves-existing', publish(copy.deepcopy(baseline)), 'downloaded',
                      prepare=lambda folder: (folder / baseline['name']).write_bytes(sentinel))
    require(Path(value['file']).name != baseline['name'], 'Collision did not produce a separate name')
    require((Path(value['file']).parent / baseline['name']).read_bytes() == sentinel, 'Existing download was overwritten')
    require(sha(Path(value['file'])) == sha(small), 'Colliding destination has incorrect bytes')
    invoke(client, root, 'check-same-version', publish(copy.deepcopy(baseline)), 'idle', mode='check')

    large = root / 'large-transport-only.zip'
    with zipfile.ZipFile(large, 'x', compression=zipfile.ZIP_STORED) as archive:
        for name in ('huhucapture.exe', 'Qt6Core.dll', 'plugins/platforms/qwindows.dll'):
            archive.writestr(name, SCOPE)
        with archive.open('TRANSPORT-PADDING.bin', 'w') as stream:
            for _ in range(65):
                stream.write(b'P' * (1024 * 1024))
    large_fixture = fixture(large)
    value, _ = invoke(client, root, 'zip-over-old-64mib-limit', publish(copy.deepcopy(large_fixture)), 'downloaded')
    require(value['total'] > 64 * 1024 * 1024 and value['received'] == value['total'] and sha(Path(value['file'])) == sha(large),
            'Full streamed archive must exceed the old EXE budget and retain exact bytes')
    routes = publish(copy.deepcopy(large_fixture))
    routes[package_path]['stall'] = True
    value, _ = invoke(client, root, 'cancel-real-inflight-read', routes, 'cancelled', mode='cancel')
    require(value['cancelSent'] and value['maximumReceived'] > 0 and value['elapsedMilliseconds'] < 10000,
            'Native cancellation must interrupt a stalled input stream and join promptly')

    # The harness itself has a real x64 PE/version resource. These cases test PE
    # transport validation only; they never execute a downloaded file or claim
    # that the harness is a product installer.
    for kind, legacy in (('installer', False), ('installer', True)):
        f = fixture(client, kind=kind, legacy=legacy)
        value, _ = invoke(client, root, 'legacy-exe-fallback' if legacy else 'installer-pe-transport', publish(f), 'downloaded')
        require(value['packageKind'] == ('legacy-exe' if legacy else 'installer') and sha(Path(value['file'])) == sha(client),
                'Real PE transport selection or retained bytes differ')
        bad = fixture(client, tag='v0.0.6', kind=kind, legacy=legacy)
        value, _ = invoke(client, root, 'legacy-version-mismatch' if legacy else 'installer-version-mismatch', publish(bad), 'failed')
        require(value['error'] == 4, 'A genuine PE with the wrong embedded version must fail integrity')
    dll_shaped = root / 'test-harness-dll-characteristic.exe'
    pe = bytearray(client.read_bytes())
    pe_offset = struct.unpack_from('<I', pe, 60)[0]
    flags = struct.unpack_from('<H', pe, pe_offset + 22)[0]
    struct.pack_into('<H', pe, pe_offset + 22, flags | 0x2000)
    dll_shaped.write_bytes(pe)
    value, _ = invoke(client, root, 'versioned-dll-is-not-executable', publish(fixture(dll_shaped, legacy=True)), 'failed')
    require(value['error'] == 4, 'A versioned DLL-shaped PE must never pass as a runnable legacy EXE')
    too_large = publish(fixture(large, legacy=True))
    value, requests = invoke(client, root, 'legacy-budget-still-64mib', too_large, 'failed')
    require(value['error'] == 2 and len(requests) == 1, 'Legacy fallback budget must be checked before download')

    for target in ('manifest', 'build'):
        for flag in ('developmentOnly', 'sourceDirty'):
            for index, invalid in enumerate((True, 'false', 0, None)):
                bad = copy.deepcopy(baseline)
                bad[target][flag] = invalid
                routes = publish(bad, legacy_payload=client)
                value, requests = invoke(client, root, f'{target}-{flag}-{index}', routes, 'failed')
                require(value['error'] == 2 and package_path not in requests and
                        ASSET_PATH + bad['tag'] + '/huhucapture.exe' not in requests,
                        'Malformed/development provenance must not request any package or downgrade to legacy')
    for name, change in (
            ('wrong-repository', lambda f: f['manifest'].update(repository='someone/else')),
            ('wrong-architecture', lambda f: f['manifest']['assets'][0].update(architecture='arm64')),
            ('wrong-build-commit', lambda f: f['build'].update(sourceCommit='2' * 40)),
            ('missing-qt-runtime', lambda f: f['build'].update(files=f['build']['files'][:1])),
            ('path-traversal', lambda f: f['build']['files'][0].update(path='../huhucapture.exe')),
            ('wrong-inventory-root', lambda f: f['build']['packages'][0].update(contentRoot='other-package'))):
        bad = copy.deepcopy(baseline)
        change(bad)
        value, requests = invoke(client, root, name, publish(bad, legacy_payload=client), 'failed')
        require(value['error'] == 2 and package_path not in requests and not any(path.endswith('/huhucapture.exe') for path in requests),
                name + ': invalid manifest/build-info must never fall back')

    routes = publish(copy.deepcopy(baseline), legacy_payload=client)
    routes[ASSET_PATH + baseline['tag'] + '/release-manifest.json'] = {'status': 404, 'body': b'missing'}
    _, requests = invoke(client, root, 'advertised-manifest-missing', routes, 'failed')
    require(not any(path.endswith('/huhucapture.exe') for path in requests), 'Missing advertised manifest incorrectly downgraded')
    routes = publish(copy.deepcopy(baseline))
    routes[package_path] = {'body': small.read_bytes()[:-1] + b'X'}
    value, _ = invoke(client, root, 'wrong-sha256', routes, 'failed')
    require(value['error'] == 4, 'Package hash mismatch was not rejected')
    routes = publish(copy.deepcopy(baseline))
    routes[package_path].update(truncateAt=80)
    invoke(client, root, 'truncated-response', routes, 'failed')
    routes = publish(copy.deepcopy(baseline))
    routes[package_path] = {'body': small.read_bytes() + b'overrun', 'chunked': True}
    value, _ = invoke(client, root, 'chunked-body-overrun', routes, 'failed')
    require(value['error'] == 4, 'Chunked response body exceeded authenticated length')
    routes = publish(copy.deepcopy(baseline))
    routes[package_path].update(chunked=True)
    value, _ = invoke(client, root, 'chunked-exact-body', routes, 'downloaded')
    require(sha(Path(value['file'])) == sha(small), 'Chunked package bytes differ')
    routes = publish(copy.deepcopy(baseline))
    routes[package_path] = {'status': 302, 'redirect': '/signed-fixture', 'body': b''}
    routes['/signed-fixture'] = {'body': small}
    invoke(client, root, 'same-origin-redirect', routes, 'downloaded')
    routes = publish(copy.deepcopy(baseline))
    routes[package_path] = {'status': 302, 'redirect': 'http://127.0.0.1:1/not-allowed', 'body': b''}
    invoke(client, root, 'different-origin-redirect', routes, 'failed')
    routes = {API_PATH: {'body': b' ' * (1024 * 1024 + 1), 'chunked': True}}
    invoke(client, root, 'bounded-release-metadata', routes, 'failed')


def actual_release_fixture(client, root, directory):
    # The producer's manifest and platform build-info are used byte-for-byte.
    # Development/dirty flags are never changed to make this gate pass.
    manifest_path = directory / 'release-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    require(manifest.get('developmentOnly') is False and manifest.get('sourceDirty') is False,
            'Actual acceptance fixture must already contain strict clean release metadata; no flag override exists')
    tag = manifest['tag']
    resources = {'release-manifest.json': manifest_path}
    for entry in manifest['assets']:
        for item in (entry, entry['buildInfo']):
            path = directory / item['name']
            require(path.parent == directory and descriptor(item['name'], path) == {key: item[key] for key in ('name', 'size', 'sha256')},
                    'Actual release fixture asset does not match unmodified manifest bytes')
            resources[item['name']] = path
    assets = []
    for name, path in resources.items():
        info = descriptor(name, path)
        assets.append({'name': name, 'size': info['size'], 'digest': 'sha256:' + info['sha256'],
                       'browser_download_url': 'https://github.com' + ASSET_PATH + tag + '/' + name})
    release = {'draft': False, 'prerelease': False, 'tag_name': tag,
               'html_url': 'https://github.com/' + REPOSITORY + '/releases/tag/' + tag, 'assets': assets}
    routes = {ASSET_PATH + tag + '/' + name: {'body': path} for name, path in resources.items()}
    routes[API_PATH] = {'body': encode(release)}
    value, _ = invoke(client, root, 'actual-complete-release-package', routes, 'downloaded')
    require(value['packageKind'] in ('installer', 'portable-zip'), 'Actual release fixture did not select a complete package')
    expected = next(entry for entry in manifest['assets'] if entry['platform'] == 'windows' and
                    entry['architecture'] == 'x64' and entry['packageKind'] == value['packageKind'])
    require(sha(Path(value['file'])) == expected['sha256'], 'Actual complete package differs after native streaming/commit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--release-fixture', type=Path)
    args = parser.parse_args()
    require(os.name == 'nt', 'The real WinRT worker must be built and run on Windows')
    client = args.client.resolve()
    require(client.is_file(), 'Native WindowsUpdateDownloadTests.exe must already be built')
    args.evidence.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='bridge-', dir=args.evidence.resolve()))
    started = time.monotonic()
    transport_suite(client, root)
    if args.release_fixture:
        actual_release_fixture(client, root, args.release_fixture.resolve())
    report = {'transportFixturesPassed': True, 'actualCompletePackagePassed': bool(args.release_fixture),
              'scope': 'Real WinRT/HTTP/file contract; native installer/GUI and physical desktop acceptance are separate gates.',
              'elapsedSeconds': time.monotonic() - started}
    (root / 'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
