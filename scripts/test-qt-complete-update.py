#!/usr/bin/env python3
"""Download real computed-complete packages through production Qt About/update.

Only finalized release-intent native outputs or their complete aggregate are
accepted. Original manifest/build-info/package bytes are served unchanged by
the existing real loopback HTTP fixture. This does not install or execute a
downloaded package and is not a physical-desktop acceptance claim.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import types

from version_source import read_version

ROOT = Path(__file__).resolve().parents[1]


def module(name: str, relative: str):
    path = ROOT / relative
    value = types.ModuleType(name)
    value.__file__ = str(path)
    sys.modules[name] = value
    exec(compile(path.read_bytes(), str(path), 'exec'), value.__dict__)
    return value


package = module('huhucapture_complete_update_packages', 'scripts/package-qt.py')
http = module('huhucapture_complete_update_http', 'scripts/test-legacy-update.py')
require = package.meta.require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    folder, probe, output = args.package.resolve(), args.probe.resolve(), args.output.resolve()
    require(not output.exists(), 'Complete-package update evidence must use a new owned output directory')
    output.mkdir(parents=True)
    report = {'schemaVersion': 1, 'passed': False, 'cases': [],
              'scope': 'actual finalized complete packages; production Qt updater and About widgets; no installation or physical-input claim'}
    server = None
    thread = None
    try:
        require(probe.is_file(), 'Build the actual native qt_complete_update_probe first')
        aggregate = folder / 'release-manifest.json'
        if aggregate.is_file():
            manifest, references = package.verify_aggregate(folder, expected_version=read_version(ROOT))
            manifest_path = aggregate
        else:
            candidates = list(folder.glob('package-manifest-*.json'))
            require(len(candidates) == 1, 'One finalized formal native platform manifest is required')
            manifest_path = candidates[0]
            manifest = package.meta.json_read(manifest_path, 1024 * 1024)
            _, references = package.verify_release_files(folder, manifest, complete=False)
        current = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, check=True,
                                 stdout=subprocess.PIPE, text=True, encoding='ascii').stdout.strip()
        require(manifest['sourceCommit'] == current and manifest['version'] == read_version(ROOT),
                'The real packages must belong to this exact native probe source/version')
        platform = 'windows' if sys.platform == 'win32' else 'linux'
        require(sys.platform == 'win32' or sys.platform.startswith('linux'), 'Only supported native package platforms may run this probe')
        assets = [entry for entry in manifest['assets'] if entry['platform'] == platform]
        expected_kinds = {kind for kind, identity in package.meta.KINDS.items() if identity[0] == platform}
        require({entry['packageKind'] for entry in assets} == expected_kinds and len(assets) == 2,
                'Both real native package forms must pass the actual updater')
        served = {name: folder / name for name in references if name not in ('release-manifest.json', 'SHA256SUMS.txt')}
        served['release-manifest.json'] = manifest_path
        descriptors = {name: references[name] for name in served if name != 'release-manifest.json'}
        descriptors['release-manifest.json'] = {**package.meta.descriptor(manifest_path), 'name': 'release-manifest.json'}
        prefix = '/NAMEWTA/huhucapture/releases/download/' + manifest['tag'] + '/'
        release = {'draft': False, 'prerelease': False, 'tag_name': manifest['tag'],
                   'html_url': http.REPOSITORY + '/releases/tag/' + manifest['tag'],
                   'assets': [{'name': name, 'size': descriptor['size'], 'digest': 'sha256:' + descriptor['sha256'],
                               'browser_download_url': http.REPOSITORY + '/releases/download/' + manifest['tag'] + '/' + name}
                              for name, descriptor in sorted(descriptors.items())]}
        routes = {http.LATEST: http.Response(http.encoded(release)),
                  **{prefix + name: http.Response(path, slow=any(entry['name'] == name for entry in assets))
                     for name, path in served.items()}}
        server = http.Server()
        thread = threading.Thread(target=server.serve_forever, name='actual-complete-update-http')
        thread.start()
        origin = f'http://127.0.0.1:{server.server_address[1]}'
        report.update({'sourceCommit': current, 'version': manifest['version'], 'tag': manifest['tag'],
                       'manifestSha256': package.meta.sha256(manifest_path), 'platform': platform,
                       'probeSha256': package.meta.sha256(probe), 'aggregateInput': aggregate.is_file()})
        for asset in sorted(assets, key=lambda item: item['packageKind']):
            case = output / asset['packageKind']
            case.mkdir()
            server.install(routes)
            native_report = case / 'native.json'
            arguments = [str(probe), '--origin', origin, '--kind', asset['packageKind'],
                         '--downloads', str(case / 'downloads'), '--output', str(native_report),
                         '--asset', asset['name'], '--sha256', asset['sha256'], '--size', str(asset['size'])]
            result = subprocess.run(arguments, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=600, env=os.environ.copy())
            (case / 'probe.log').write_bytes(result.stdout)
            requests = server.seen()
            case_evidence = {'packageKind': asset['packageKind'], 'asset': asset['name'],
                             'sha256': asset['sha256'], 'size': asset['size'],
                             'returnCode': result.returncode, 'requests': requests}
            report['cases'].append(case_evidence)
            require(result.returncode == 0 and native_report.is_file(),
                    'Actual complete-package probe failed; retain its native report and stdout: ' + asset['packageKind'])
            observed = package.meta.json_read(native_report, 1024 * 1024)
            case_evidence['native'] = observed
            require(observed.get('passed') is True and observed.get('autoExecuted') is False and
                    observed.get('shutdownFinished') is True and observed.get('pageDestroyedWhileDownloading') is True and
                    observed.get('pageReopenedFromService') is True,
                    'The real About lifecycle and joined completion must all have passed')
            require(observed.get('compiledVersion') == manifest['version'] and observed.get('version') == manifest['version'] and
                    observed.get('tag') == manifest['tag'] and observed.get('packageKind') == asset['packageKind'] and
                    observed.get('asset') == asset['name'] and observed.get('sha256') == asset['sha256'] and
                    observed.get('received') == asset['size'] and observed.get('total') == asset['size'],
                    'Native updater evidence does not describe these exact complete package bytes')
            retained = Path(observed['file']).resolve()
            require(retained.parent == (case / 'downloads').resolve() and
                    package.meta.descriptor(retained) == {key: asset[key] for key in ('name', 'size', 'sha256')},
                    'The actual retained package differs from the original finalized artifact')
            expected = [http.LATEST, prefix + 'release-manifest.json', prefix + asset['buildInfo']['name'], prefix + asset['name']]
            require(Counter(requests) == Counter(expected),
                    'The real updater must fetch only its canonical metadata and selected complete package once')
        report['passed'] = True
        return 0
    except (ValueError, RuntimeError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
        print('Complete-package Qt update failed:', error, file=sys.stderr)
        return 1
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
        if thread is not None:
            thread.join()
        (output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(report, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    raise SystemExit(main())
