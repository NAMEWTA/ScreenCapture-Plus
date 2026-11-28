#!/usr/bin/env python3
"""Restore this project's four fixed Qt source modules in an empty workspace.

This follows build-portal.py's isolated Git init/fetch/checkout workflow. The
source collector separately verifies each checkout against the actual SDK and
archives it; restoring a reviewed revision is not that SDK/source proof.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = runpy.run_path(str(ROOT / 'build/package/source_checkout.py'))
GIT = CHECKOUT['COLLECTOR']


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pinned_modules(qt, platform_name):
    """One known module set, with qtbase's existing revision as its only source."""
    modules = qt.get('sourceModules', {})
    require(set(modules) == {'qtbase', 'qtsvg', 'qtwayland', 'qttranslations'},
            'The locked Qt source module set changed and needs review')
    result = []
    for name in ('qtbase', 'qtsvg', 'qtwayland', 'qttranslations'):
        module = modules[name]
        expected_platforms = ['linux'] if name == 'qtwayland' else ['windows', 'linux']
        require(module.get('platforms') == expected_platforms, 'Unexpected Qt source platform scope: ' + name)
        if name == 'qtbase':
            require(module.get('usesQtbaseRevision') is True and 'commit' not in module and 'repository' not in module,
                    'qtbase must reuse qtbaseRepository/qtbaseCommit without a second revision')
            repository, commit = qt['qtbaseRepository'], qt['qtbaseCommit']
        else:
            repository, commit = module['repository'], module['commit']
        require(repository == 'https://github.com/qt/' + name + '.git' and
                isinstance(commit, str) and re.fullmatch(r'[0-9a-f]{40}', commit),
                'Qt source must name the reviewed official repository and exact commit')
        evidence = module['evidence']
        if name in ('qtbase', 'qtsvg'):
            require(evidence == {'type': 'sdk-spdx-download-location', 'observedPlatform': 'linux'},
                    'Qt source evidence must retain its actual platform scope')
        elif name == 'qtwayland':
            require(evidence == {'type': 'sdk-source-spdx-dot-tag-sha1', 'observedPlatform': 'linux',
                                 'sha1': hashlib.sha1((commit + '\n').encode('ascii')).hexdigest()},
                    'Qt Wayland revision differs from its reviewed source SPDX .tag bytes')
        else:
            require(evidence == {'type': 'official-release-tag', 'tag': 'v' + qt['version'],
                                 'sdkSourceBinding': 'unproven'},
                    'Translations tag must match the selected Qt version and remain unproven against the SDK')
        if platform_name in module['platforms']:
            result.append({'module': name, 'repository': repository, 'commit': commit, 'reviewedEvidence': evidence})
    return result


def run(arguments, directory, log):
    environment = GIT['git_environment']()
    environment['LC_ALL'] = 'C.UTF-8'
    log.write(('Running: ' + ' '.join(map(str, arguments)) + '\n').encode('utf-8')); log.flush()
    subprocess.run(list(map(str, arguments)), cwd=directory, env=environment,
                   stdout=log, stderr=subprocess.STDOUT, check=True)


def git_output(source, *arguments):
    return GIT['git'](source, *arguments).decode('utf-8', errors='strict').strip()


def restore(workspace, modules, report):
    for module in modules:
        name = module['module']
        source = workspace / name
        print('Restoring original Qt source: ' + name, flush=True)
        with (workspace / (name + '.git.log')).open('xb') as log:
            source.mkdir()
            run(GIT['git_command'](source, 'init', '--quiet'), source, log)
            run(GIT['git_command'](source, 'config', 'core.autocrlf', 'false'), source, log)
            # core.autocrlf=false alone is overridden by upstream eol=crlf.
            # Install the explicit raw-blob profile before the first checkout.
            profile = CHECKOUT['install_policy'](source)
            log.write(('Original Git source-byte profile: ' + json.dumps(profile) + '\n').encode('utf-8')); log.flush()
            run(GIT['git_command'](source, 'remote', 'add', 'origin', module['repository']), source, log)
            run(GIT['git_command'](source, '-c', 'protocol.https.allow=always', 'fetch', '--depth=1',
                                  '--no-tags', 'origin', module['commit']), source, log)
            run(GIT['git_command'](source, '-c', 'advice.detachedHead=false', 'checkout', '--detach', 'FETCH_HEAD'), source, log)
            actual = git_output(source, 'rev-parse', 'HEAD')
            require(actual == module['commit'], 'Fetched Qt revision differs from the dependency lock: ' + name)
            tag_evidence = None
            if module['reviewedEvidence']['type'] == 'official-release-tag':
                tag = module['reviewedEvidence']['tag']
                # This explicit refspec obtains the real remote tag object.
                # Never create a local tag to make a selected commit appear to
                # have upstream release evidence.
                reference = 'refs/tags/' + tag
                run(GIT['git_command'](source, '-c', 'protocol.https.allow=always', 'fetch', '--depth=1',
                                      '--no-tags', 'origin', reference + ':' + reference), source, log)
                tag_commit = git_output(source, 'rev-parse', reference + '^{commit}')
                require(tag_commit == actual, 'The actual official Qt release tag points to a different commit')
                tag_evidence = {'reference': reference, 'object': git_output(source, 'rev-parse', reference),
                                'peeledCommit': tag_commit}
            require(git_output(source, 'status', '--porcelain', '--untracked-files=normal') == '',
                    'The newly restored Qt source is not clean: ' + name)
            require(git_output(source, 'rev-parse', 'HEAD') == module['commit'], 'Qt checkout changed during restoration')
            require(CHECKOUT['verify_policy'](source) == profile, 'The original Qt checkout profile changed')
            report['modules'].append({**module, 'sourceDirectory': str(source),
                                      'tree': git_output(source, 'rev-parse', 'HEAD^{tree}'),
                                      'sourceCheckoutProfile': profile,
                                      'officialTag': tag_evidence, 'sdkSourceBinding': 'pending-actual-sdk-collector'})


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=('windows', 'linux'), required=True)
    parser.add_argument('--workspace', type=Path, required=True, help='New empty source-input workspace; never reset/clean old files')
    options = parser.parse_args()
    workspace = options.workspace
    report = None
    try:
        lock_path = root / 'build/dependencies.lock.json'
        lock_bytes = lock_path.read_bytes()
        qt = json.loads(lock_bytes)['qtMigration']['qt']
        modules = pinned_modules(qt, options.platform)
        require(not workspace.is_symlink(), 'The Qt source workspace cannot be a directory link')
        workspace.mkdir(parents=True, exist_ok=True)
        require(not any(workspace.iterdir()), 'Use an empty Qt source workspace; previous files are retained')
        workspace = workspace.resolve(strict=True)
        report = {'schemaVersion': 1, 'qtVersion': qt['version'], 'platform': options.platform,
                  'dependencyLockSha256': hashlib.sha256(lock_bytes).hexdigest(),
                  'restorationComplete': False, 'correspondingSourceComplete': False,
                  'scope': 'Original fixed Git blob bytes, without checkout conversion; SDK binding and source archive remain separate proof',
                  'modules': [], 'failures': [], 'outstandingSourceFamilies': ['qt-sdk-icu', 'qttranslations-sdk-source-binding']}
        restore(workspace, modules, report)
        require(lock_path.read_bytes() == lock_bytes, 'The dependency lock changed during Qt source restoration')
        report['restorationComplete'] = True
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        if report is not None:
            report['failures'].append(str(error))
        print('Qt source restoration failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if report is not None:
            (workspace / 'restore-info.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Restored {len(report["modules"])} exact Qt modules; run the actual SDK source collector before packaging notices.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
