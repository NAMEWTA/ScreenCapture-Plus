"""Separate original license evidence for one exact Sabotage header archive.

The source tree is never augmented with these external texts. Git object
identities connect the original archive, distribution recipe and Linux license
files. File observations preserve declarations and unlabelled files; they are
not a legal classification or evidence that a native build succeeded.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re

COMPONENT = 'appimage-linux-headers'
PROFILE = 'linux-headers-4.19.88-2-licenses-v1'
ARCHIVE_SHA256 = 'dc7abf734487553644258a3822cfd429d74656749e309f2b25f09f4282e05588'
SOURCE_TREE = 'c0f1c4e0a7d96e99bc8dd3202229a74f47137b2e147afd68205065c520ab1004'
ARCHIVE_GIT_TREE = 'a614e98307d77de8b1ac75d432b84f1b74a664b7'
HEADERS_COMMIT = '22bba0124dfe2728ef5b4555baa1ae0960492665'
KERNEL_COMMIT = 'fb683b5e3f53a73e761952735736180939a313df'
PROVENANCE = {'path': 'third_party/appimage/linux-headers-licenses/provenance.json',
              'size': 17780, 'sha256': 'd8006cfe996a3e27b816d10457c6919c6be63fad8341835828e64404839926f5'}
PROFILE_RECORD = {'profile': PROFILE, 'provenance': PROVENANCE}
LICENSE_PATHS = {'COPYING', 'LICENSES/exceptions/Linux-syscall-note',
                 *('LICENSES/preferred/' + name for name in
                   ('BSD-2-Clause', 'BSD-3-Clause', 'GPL-2.0', 'LGPL-2.0', 'LGPL-2.1', 'MIT')),
                 *('LICENSES/other/' + name for name in ('CDDL-1.0', 'GPL-1.0', 'Linux-OpenIB'))}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def object_digest(kind, data):
    return hashlib.sha1(kind.encode('ascii') + b' ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()


def tree_bytes(entries):
    require(len({entry['path'] for entry in entries}) == len(entries), 'Duplicate original Git tree entry')
    result = []
    for entry in sorted(entries, key=lambda item: (item['path'] + ('/' if item['type'] == 'tree' else '')).encode()):
        name, mode, kind = entry['path'], entry['mode'], entry['type']
        require(name not in ('', '.', '..') and not any(character in name for character in '/\\\0') and
                re.fullmatch('[0-9a-f]{40}', entry['sha']) is not None and
                ((kind == 'tree' and mode == '040000') or
                 (kind == 'blob' and mode in ('100644', '100755', '120000'))),
                'Invalid original Git tree entry')
        result.append(mode.lstrip('0').encode() + b' ' + name.encode() + b'\0' + bytes.fromhex(entry['sha']))
    return b''.join(result)


def files_tree(files):
    """Compute Git trees from complete path/blob/mode identities, including links."""
    root = {}
    for entry in files:
        parts = entry['path'].split('/')
        require(all(part not in ('', '.', '..') for part in parts), 'Invalid original source path')
        folder = root
        for part in parts[:-1]:
            folder = folder.setdefault(part, {})
            require(isinstance(folder, dict), 'Original source file/directory conflict')
        require(parts[-1] not in folder, 'Duplicate original source path')
        folder[parts[-1]] = (entry['mode'], entry['sha'])
    def visit(folder):
        entries = []
        for name, value in folder.items():
            mode, digest = ('040000', visit(value)) if isinstance(value, dict) else value
            entries.append({'path': name, 'mode': mode, 'type': 'tree' if mode == '040000' else 'blob', 'sha': digest})
        return object_digest('tree', tree_bytes(entries))
    return visit(root)


def checked_material(base, item):
    name = item['path']
    require(isinstance(name, str) and not name.startswith('/') and '\\' not in name and
            all(part not in ('', '.', '..') for part in name.split('/')), 'Invalid external license material path')
    path = base / name
    require(path.is_file() and not path.is_symlink() and
            all(not parent.is_symlink() for parent in path.parents if parent.is_relative_to(base)),
            'External license material is missing or linked')
    data = path.read_bytes()
    require(len(data) == item['size'] and hashlib.sha256(data).hexdigest() == item['sha256'] and
            object_digest('blob', data) == item['gitBlob'], 'External license material differs from its original Git blob')
    return path, data


def validate_profile(identifier, pin, platform, project):
    specification = pin.get('licenseSupplement')
    if specification is None:
        return None
    require(identifier == COMPONENT and platform == 'linux' and specification == PROFILE_RECORD and
            pin.get('sourceArchive', {}).get('sha256') == ARCHIVE_SHA256 and
            pin.get('sourcePathProfile', {}).get('sourceTreeSha256') == SOURCE_TREE and not pin.get('patches'),
            'License supplement requires its exact original Linux header profile and archive')
    path = Path(project) / PROVENANCE['path']
    require(path.is_file() and not path.is_symlink(), 'Original external license provenance is missing or linked')
    data = path.read_bytes()
    require(len(data) == PROVENANCE['size'] and hashlib.sha256(data).hexdigest() == PROVENANCE['sha256'],
            'Original external license provenance differs from its locked bytes')
    value = json.loads(data)
    require(value.get('schemaVersion') == 1 and value.get('profile') == PROFILE and
            value.get('kind') == 'huhucapture-external-linux-header-license-provenance' and
            value.get('component') == COMPONENT and value['archive']['sha256'] == ARCHIVE_SHA256 and
            value['archive']['sourceTreeSha256'] == SOURCE_TREE and value['archive']['gitTree'] == ARCHIVE_GIT_TREE,
            'Unknown original external license provenance')
    headers, kernel = value['headers'], value['kernel']
    for original, expected in ((headers, HEADERS_COMMIT), (kernel, KERNEL_COMMIT)):
        require(original['commit'] == expected and
                object_digest('commit', original['commitObject'].encode()) == expected and
                original['commitObject'].startswith('tree ' + original['tree'] + '\n'),
                'Original upstream commit does not bind the recorded source tree')
    require(headers['tag'] == 'v4.19.88-2' and kernel['tag'] == 'v4.19.88' and
            object_digest('tag', kernel['tagObjectBytes'].encode()) == kernel['tagObject'] and
            kernel['tagObjectBytes'].startswith('object ' + KERNEL_COMMIT + '\ntype commit\ntag v4.19.88\n'),
            'Original Linux release tag does not bind its exact commit')
    trees = {tree['sha']: tree for tree in kernel['trees']}
    require(len(trees) == len(kernel['trees']) and all(object_digest('tree', tree_bytes(tree['entries'])) == tree['sha']
            for tree in trees.values()), 'Original Linux license tree object changed')
    def lookup(name):
        tree = kernel['tree']
        parts = name.split('/')
        for index, part in enumerate(parts):
            require(tree in trees, 'Original Linux license Git tree chain is incomplete')
            found = [entry for entry in trees[tree]['entries'] if entry['path'] == part]
            require(len(found) == 1, 'Original Linux license path is absent or ambiguous')
            entry = found[0]
            if index == len(parts) - 1:
                require(entry['type'] == 'blob', 'Original Linux license is not a Git blob')
                return entry
            require(entry['type'] == 'tree', 'Original Linux license path crosses a non-directory')
            tree = entry['sha']
    texts = kernel['licenseTexts']
    require(len(texts) == len(LICENSE_PATHS) and {item['upstreamPath'] for item in texts} == LICENSE_PATHS,
            'Original Linux license text set is incomplete or substituted')
    base = path.parent
    for item in texts:
        entry = lookup(item['upstreamPath'])
        require(item['path'] == 'linux/' + item['upstreamPath'] and
                (item['gitBlob'], item['gitMode']) == (entry['sha'], entry['mode']),
                'External license text is not in the exact original Linux commit')
        checked_material(base, item)
    excluded = headers['omittedFromArchive']
    require(len(excluded) == 3 and {item['upstreamPath'] for item in excluded} == {'UPDATE.sh', 'create-dist.sh', 'test.sh'},
            'Unknown original header distribution omission')
    for item in excluded:
        require(item['path'] == 'sabotage/' + item['upstreamPath'], 'Changed original header distribution material path')
        checked_material(base, item)
    recipe = next(item for item in excluded if item['upstreamPath'] == 'create-dist.sh')
    require(b'rm $projver/*.sh\n' in checked_material(base, recipe)[1],
            'Original distribution recipe does not explain the omitted top-level scripts')
    return value


def review_source(identifier, pin, platform, project, source, files, api):
    provenance = validate_profile(identifier, pin, platform, project)
    if provenance is None:
        return None
    require(len(files) == 1443 and api['tree_digest'](files) == SOURCE_TREE,
            'External license evidence requires the unchanged complete header archive tree')
    source, base = Path(source), Path(project) / Path(PROVENANCE['path']).parent
    materials = provenance['kernel']['licenseTexts'] + provenance['headers']['omittedFromArchive']
    identifiers, exceptions = set(), {}
    for item in provenance['kernel']['licenseTexts']:
        text = checked_material(base, item)[1].decode('utf-8')
        identifiers.update(re.findall(r'(?m)^Valid-License-Identifier: (\S+)$', text))
        exception = re.findall(r'(?m)^SPDX-Exception-Identifier: (\S+)$', text)
        if exception:
            allowed = re.findall(r'(?m)^SPDX-Licenses: (.+)$', text)
            require(len(exception) == len(allowed) == 1, 'Ambiguous original SPDX exception declaration')
            exceptions[exception[0]] = set(allowed[0].split(', '))
    entries, observations, expressions, unlabelled = [], [], {}, []
    comment = re.compile(rb'/\*.*?\*/|//[^\n]*(?:\n[ \t]*//[^\n]*)*', re.S)
    notice = re.compile(rb'copyright|redistribut|permission\s+is\s+hereby|public\s+domain|\b(?:license|licence|GPL|LGPL)\b', re.I)
    for item in files:
        path = source / item['path']
        data = os.readlink(path).encode() if item['type'] == 'symlink' else path.read_bytes()
        require(len(data) == item['size'] and hashlib.sha256(data).hexdigest() == item['sha256'],
                'An original header file changed while its license declarations were observed')
        mode = '120000' if item['type'] == 'symlink' else ('100755' if item['mode'] == 0o755 else '100644')
        blob = object_digest('blob', data)
        entries.append({'path': item['path'], 'mode': mode, 'sha': blob})
        if item['type'] == 'symlink':
            continue
        spdx = [match.group(1).decode('utf-8').strip(' */\t') for match in
                re.finditer(rb'SPDX-License-Identifier:[ \t]*([^\r\n]+)', data)]
        for expression in spdx:
            tokens = set(re.findall(r'[A-Za-z0-9][A-Za-z0-9.+-]*', expression)) - {'AND', 'OR', 'WITH'}
            require(tokens <= identifiers | exceptions.keys(), 'An actual header SPDX identifier lacks its original license text')
            for license_id, exception in re.findall(r'([A-Za-z0-9.+-]+)\s+WITH\s+([A-Za-z0-9.+-]+)', expression):
                require(license_id in exceptions.get(exception, set()), 'An actual SPDX exception lacks its original permitted license')
            expressions.setdefault(expression, []).append(item['path'])
        blocks = [{'offset': match.start(), 'size': len(match.group()), 'sha256': hashlib.sha256(match.group()).hexdigest()}
                  for match in comment.finditer(data) if notice.search(match.group())]
        if not spdx:
            unlabelled.append(item['path'])
        observations.append({'path': item['path'], 'size': item['size'], 'sha256': item['sha256'], 'gitBlob': blob,
                             'spdxExpressions': spdx, 'noticeBlocks': blocks})
    require(files_tree(entries) == ARCHIVE_GIT_TREE, 'Actual original headers differ from their complete archive Git tree')
    full = entries + [{'path': item['upstreamPath'], 'mode': item['gitMode'], 'sha': item['gitBlob']}
                      for item in provenance['headers']['omittedFromArchive']]
    require(files_tree(full) == provenance['headers']['tree'],
            'Original archive and omitted scripts do not reconstruct the exact Sabotage tag tree')
    path_profile = api['header_path_api']()['source_profile'](identifier, pin, platform)
    require(api['inventory'](source, path_profile=path_profile) == files,
            'Original header source bytes, links or modes changed during license review')
    report = {'schemaVersion': 1, 'kind': 'huhucapture-original-header-license-observations', 'profile': PROFILE,
              'sourceComponent': COMPONENT, 'sourceTreeSha256': SOURCE_TREE, 'sourceGitTree': ARCHIVE_GIT_TREE,
              'upstreamHeadersCommit': HEADERS_COMMIT, 'upstreamKernelCommit': KERNEL_COMMIT,
              'files': observations, 'spdxExpressions': expressions, 'withoutSpdx': unlabelled,
              'withoutSpdxOrDetectedNotice': [item['path'] for item in observations
                                             if not item['spdxExpressions'] and not item['noticeBlocks']],
              'meaning': 'Every original regular file remains in the unchanged source component. Notice offsets refer '
                         'to its complete original bytes. Unlabelled files remain unclassified. Separate Linux license '
                         'texts do not establish blanket licensing or successful source/native-build closure.'}
    return {'provenance': provenance, 'observations': report,
            'materials': [(item, base / item['path']) for item in materials]}
