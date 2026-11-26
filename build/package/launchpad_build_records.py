"""Retain original Ubuntu build records without inventing a build receipt.

API contract: https://api.launchpad.net/1.0.html
Protocol reference (not a code dependency): canonical/ubuntu-package-buildinfo
at a93a2e22d80909d07c1cd00fbe99d4d8c271dd02, ubuntu_package_buildinfo/cli.py.
The original .changes binds the original .buildinfo; both must name the exact
already retained .deb. Neither parsing a clear signature nor HTTPS verifies an
OpenPGP signature, an APT Release chain, or the complete compiler/link inputs.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
from urllib.error import HTTPError
from urllib.parse import parse_qs, unquote, urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


SPEC = importlib.util.spec_from_file_location('launchpad_debian_sources', Path(__file__).with_name('debian_sources.py'))
debian = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(debian)
require = debian.require
API = 'https://api.launchpad.net/1.0'
UBUNTU = API + '/ubuntu'
ARCHIVE = UBUNTU + '/+archive/primary'
OFFICIAL_HOSTS = frozenset(('api.launchpad.net', 'launchpad.net', 'launchpadlibrarian.net'))
MAX_API = 4 * 1024 * 1024
MAX_CONTROL = 16 * 1024 * 1024
MAX_LOG = 128 * 1024 * 1024
MAX_DOWNLOADS = 512 * 1024 * 1024
MAX_PAGES = 10
MAX_PACKAGES = 64


def write_json(path, value):
    with Path(path).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write('\n')


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON object field: ' + key)
            result[key] = value
        return result
    def constant(value):
        raise ValueError('Non-finite JSON value: ' + value)
    result = json.loads(data, object_pairs_hook=pairs, parse_constant=constant)
    require(isinstance(result, dict), 'An original JSON object is required')
    return result


def official_url(value, *, api=False):
    require(isinstance(value, str) and value and len(value) <= 8192 and
            not any(ord(char) <= 32 or ord(char) == 127 for char in value) and '\\' not in value,
            'Missing or unsafe original Launchpad URL')
    parsed = urlsplit(value)
    require(parsed.scheme == 'https' and parsed.hostname in OFFICIAL_HOSTS and
            parsed.username is None and parsed.password is None and parsed.port in (None, 443) and
            not parsed.fragment, 'Only HTTPS official Launchpad URLs are accepted')
    require(not any(part in ('.', '..') for part in unquote(parsed.path).split('/')),
            'Ambiguous path traversal in an original Launchpad URL')
    if api:
        require(parsed.hostname == 'api.launchpad.net' and
                (parsed.path == '/1.0' or parsed.path.startswith('/1.0/')),
                'The original API record must remain on Launchpad API 1.0')
    return value


def artifact_name(url):
    official_url(url)
    return debian.basename(unquote(urlsplit(url).path.rsplit('/', 1)[-1]))


class OfficialRedirect(HTTPRedirectHandler):
    max_redirections = 5
    max_repeats = 2

    def __init__(self, *, api):
        self.api = api

    def redirect_request(self, request, response, code, message, headers, new_url):
        official_url(new_url, api=self.api)
        return super().redirect_request(request, response, code, message, headers, new_url)


class Downloader:
    """Bound each response and validate every redirect before following it."""
    def __init__(self, root):
        self.root = Path(root)
        self.records = []
        self.downloaded = 0

    def fetch(self, url, path, *, limit, api=False):
        official_url(url, api=api)
        path = Path(path)
        require(not path.exists() and path.resolve().is_relative_to(self.root.resolve()),
                'An exclusive response path under the new collection directory is required')
        path.parent.mkdir(parents=True, exist_ok=True)
        request = Request(url, headers={'User-Agent': 'huhucapture-original-build-records/1',
                                       'Accept': 'application/json' if api else '*/*',
                                       'Accept-Encoding': 'identity'})
        opener = build_opener(OfficialRedirect(api=api))
        try:
            with opener.open(request, timeout=30) as response:
                final = official_url(response.geturl(), api=api)
                require(response.status == 200, 'Original Launchpad request did not return HTTP 200')
                require(response.headers.get('Content-Encoding', 'identity').lower() == 'identity',
                        'Encoded HTTP responses cannot replace original artifact bytes')
                length = response.headers.get('Content-Length')
                require(length is None or (length.isdigit() and 0 < int(length) <= limit),
                        'Original Launchpad response exceeds its size limit')
                size, digest = 0, hashlib.sha256()
                with path.open('xb') as stream:
                    while block := response.read(min(1024 * 1024, limit - size + 1)):
                        size += len(block)
                        self.downloaded += len(block)
                        require(size <= limit and self.downloaded <= MAX_DOWNLOADS,
                                'Original Launchpad response exceeds its size limit or collection budget')
                        stream.write(block)
                        digest.update(block)
                require(size > 0 and (length is None or size == int(length)),
                        'Original Launchpad response is empty or truncated')
                record = {'path': path.relative_to(self.root).as_posix(), 'requestedUrl': url,
                          'responseUrl': final, 'httpStatus': response.status, 'size': size,
                          'sha256': digest.hexdigest(), 'headers': {name: response.headers[name]
                          for name in ('Content-Type', 'ETag', 'Last-Modified') if name in response.headers}}
        except HTTPError as error:
            # Preserve a bounded original failure response, including an observed
            # access denial. Do not rewrite its URL or retry through another host.
            body = error.read(64 * 1024 + 1)
            failure = path.with_name(path.name + '.http-error')
            with failure.open('xb') as stream:
                stream.write(body[:64 * 1024])
            write_json(failure.with_name(failure.name + '.json'),
                       {'requestedUrl': url, 'httpStatus': error.code,
                        'bodyTruncated': len(body) > 64 * 1024, 'failure': str(error)})
            raise
        self.records.append(record)
        write_json(path.with_name(path.name + '.http.json'), record)
        require(debian.identity(path) == {key: record[key] for key in ('size', 'sha256')},
                'Original response changed while being retained')
        return record

    def get_json(self, url, path):
        self.fetch(url, path, limit=MAX_API, api=True)
        return strict_json(Path(path).read_bytes())


def expected_digest(record):
    require(isinstance(record, dict) and debian.SHA256.fullmatch(record.get('sha256', '')) and
            type(record.get('size')) is int and 0 < record['size'] <= debian.MAX_FILE,
            'An exact original size/SHA256 is required')
    return {key: record[key] for key in ('size', 'sha256')}


def source_field(value, binary_version):
    match = re.fullmatch(r'([a-z0-9][a-z0-9+.\-]*)(?: \(([^()\s]+)\))?', value or '')
    require(match and debian.VERSION.fullmatch(match[2] or binary_version),
            'Malformed exact Debian source identity')
    return match[1], match[2] or binary_version


def package_identity(record):
    name = record.get('binaryPackage', '')
    match = re.fullmatch(r'([a-z0-9][a-z0-9+.\-]*)(?::([a-z0-9][a-z0-9\-]*))?', name)
    require(match and record.get('architecture') == 'amd64' and match[2] in (None, 'amd64') and
            debian.VERSION.fullmatch(record.get('version', '')) and
            debian.PACKAGE.fullmatch(record.get('sourcePackage', '')) and
            debian.VERSION.fullmatch(record.get('sourceVersion', '')),
            'An exact original Ubuntu amd64 binary and source identity is required')
    return {key: record[key] for key in ('binaryPackage', 'version', 'architecture', 'sourcePackage', 'sourceVersion')}


def verify_binary_index(text, package, archive):
    name = package['binaryPackage'].split(':')[0]
    entries = [item for item in debian.control_stanzas(text) if item.get('package') == name and
               item.get('version') == package['version'] and item.get('architecture') == package['architecture']]
    require(entries, 'No original exact APT binary/version/architecture record')
    for entry in entries:
        require(source_field(entry.get('source', name), package['version']) ==
                (package['sourcePackage'], package['sourceVersion']),
                'Original APT index names a different source package/version')
        require(entry.get('sha256') == archive['sha256'] and entry.get('size') == str(archive['size']),
                'Retained .deb differs from the original APT SHA256/size')


def retain_original(path, record, destination):
    path = Path(path)
    expected = expected_digest(record)
    require(debian.identity(path) == expected, 'Retained original input changed: ' + str(path))
    destination.parent.mkdir(parents=True, exist_ok=True)
    with path.open('rb') as source, destination.open('xb') as target:
        shutil.copyfileobj(source, target)
    require(debian.identity(destination) == expected and debian.identity(path) == expected,
            'Original input changed during exclusive retention')
    return expected


def validate_package_input(record, output):
    """Recheck the successful original collector's .deb, index and dpkg fields."""
    package = package_identity(record)
    require(isinstance(record.get('files'), list) and record['files'],
            'Original package lacks its actual installed-byte bindings')
    archive = Path(record['archive']['path'])
    index = Path(record['originalIndex']['path'])
    name = debian.basename(archive.name)
    require(name.endswith('.deb'), 'The original binary archive must be a .deb')
    output.mkdir(parents=True)
    archive_copy, index_copy = output / name, output / 'Packages-records.txt'
    require(expected_digest(record['originalIndex'])['size'] <= MAX_CONTROL, 'Original APT binary records are oversized')
    archive_digest = retain_original(archive, record['archive'], archive_copy)
    index_digest = retain_original(index, record['originalIndex'], index_copy)
    verify_binary_index(index_copy.read_text(encoding='utf-8'), package, archive_digest)
    executable = shutil.which('dpkg-deb')
    require(executable, 'Real dpkg-deb is required to recheck the retained .deb identity')
    executable = Path(executable).resolve(strict=True)
    tool = {'path': str(executable), **debian.identity(executable)}
    arguments = [str(executable), '--field', str(archive_copy), 'Package', 'Version', 'Architecture', 'Source']
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=30)
    (output / 'dpkg-deb.stdout').write_bytes(result.stdout)
    (output / 'dpkg-deb.stderr').write_bytes(result.stderr)
    write_json(output / 'dpkg-deb.json', {'tool': tool, 'arguments': arguments, 'exitCode': result.returncode,
        'stdout': {'path': 'dpkg-deb.stdout', 'size': len(result.stdout), 'sha256': hashlib.sha256(result.stdout).hexdigest()},
        'stderr': {'path': 'dpkg-deb.stderr', 'size': len(result.stderr), 'sha256': hashlib.sha256(result.stderr).hexdigest()}})
    require(result.returncode == 0, 'Actual dpkg-deb could not read original archive metadata')
    fields = debian.control_stanzas(result.stdout.decode('utf-8'))
    require(len(fields) == 1, 'The original .deb must contain one control stanza')
    fields = fields[0]
    require(fields.get('package') == package['binaryPackage'].split(':')[0] and
            fields.get('version') == package['version'] and fields.get('architecture') == package['architecture'] and
            source_field(fields.get('source', fields['package']), fields['version']) ==
            (package['sourcePackage'], package['sourceVersion']),
            'Actual .deb metadata differs from the original installed package/source identity')
    require(debian.identity(executable) == {key: tool[key] for key in ('size', 'sha256')} and
            debian.identity(archive_copy) == archive_digest and debian.identity(archive) == archive_digest and
            debian.identity(index) == index_digest, 'Original binary inputs changed during metadata validation')
    return package, archive_copy


def resolve_series(downloader, series, directory):
    require(re.fullmatch(r'[a-z][a-z0-9-]{0,31}', series or ''), 'An exact Ubuntu series name is required')
    series_link = UBUNTU + '/' + series
    value = downloader.get_json(UBUNTU + '?' + urlencode({'ws.op': 'getSeries', 'name_or_version': series}),
                                directory / 'series.json')
    require(value.get('self_link') == series_link and value.get('name') == series and
            value.get('distribution_link') == UBUNTU and value.get('main_archive_link') == ARCHIVE,
            'Launchpad returned a different Ubuntu distribution/series/archive')
    architecture = downloader.get_json(series_link + '?' + urlencode({'ws.op': 'getDistroArchSeries', 'archtag': 'amd64'}),
                                      directory / 'architecture.json')
    architecture_link = series_link + '/amd64'
    require(architecture.get('self_link') == architecture_link and architecture.get('architecture_tag') == 'amd64' and
            architecture.get('distroseries_link') == series_link and architecture.get('main_archive_link') == ARCHIVE,
            'Launchpad returned a different Ubuntu architecture series')
    return series_link, architecture_link


def publications(downloader, package, architecture_link, directory):
    query = {'ws.op': 'getPublishedBinaries', 'binary_name': package['binaryPackage'].split(':')[0],
             'version': package['version'], 'exact_match': 'true', 'distro_arch_series': architecture_link,
             'order_by_date': 'true', 'ws.size': '75'}
    url = ARCHIVE + '?' + urlencode(query)
    entries, visited = {}, set()
    for page in range(MAX_PAGES):
        require(url not in visited, 'Loop in original Launchpad publication pagination')
        visited.add(url)
        value = downloader.get_json(url, directory / ('publications-' + str(page) + '.json'))
        require(isinstance(value.get('entries'), list) and len(value['entries']) <= 300,
                'Malformed or oversized original publication collection')
        for entry in value['entries']:
            require(isinstance(entry, dict) and entry.get('archive_link') == ARCHIVE and
                    entry.get('distro_arch_series_link') == architecture_link and
                    entry.get('binary_package_name') == query['binary_name'] and
                    entry.get('binary_package_version') == package['version'] and
                    entry.get('source_package_name') == package['sourcePackage'] and
                    entry.get('source_package_version') == package['sourceVersion'] and
                    entry.get('architecture_specific') is True and
                    entry.get('status') in ('Published', 'Superseded', 'Deleted', 'Obsolete'),
                    'Original publication differs from exact installed binary/source/architecture identity')
            identifier = official_url(entry.get('self_link'), api=True)
            official_url(entry.get('build_link'), api=True)
            require(identifier not in entries or entries[identifier] == entry,
                    'Original publication changed during pagination')
            entries[identifier] = entry
        next_url = value.get('next_collection_link')
        if not next_url:
            break
        official_url(next_url, api=True)
        parsed = urlsplit(next_url)
        parameters = parse_qs(parsed.query, keep_blank_values=True, strict_parsing=True)
        require(parsed.scheme + '://' + parsed.netloc + parsed.path == ARCHIVE and
                all(parameters.get(key) == [item] for key, item in query.items() if key != 'ws.size') and
                not (set(parameters) - set(query) - {'ws.start'}) and
                all(len(parameters.get(key, [])) == 1 and parameters[key][0].isdigit()
                    for key in ('ws.start', 'ws.size')) and 0 < int(parameters['ws.size'][0]) <= 300,
                'Original pagination changed the exact publication query')
        url = next_url
    else:
        raise ValueError('Original publication query exceeds the page limit')
    require(entries, 'Ubuntu has no exact original publication for the retained package')
    builds = {entry['build_link'] for entry in entries.values()}
    require(len(builds) == 1, 'Ambiguous original build records for the exact package; all API pages were retained')
    return list(entries.values()), next(iter(builds))


def validate_build(build, link, package, series_link, published):
    require(build.get('self_link') == link and build.get('arch_tag') == package['architecture'] and
            build.get('source_package_name') == package['sourcePackage'] and
            build.get('source_package_version') == package['sourceVersion'] and
            build.get('distribution_link') == UBUNTU and build.get('distro_series_link') == series_link and
            build.get('buildstate') == 'Successfully built' and build.get('datebuilt'),
            'Original build is not the exact successful Ubuntu source/version/architecture/series build')
    archive = official_url(build.get('archive_link'), api=True)
    # Ubuntu security/updates can copy a completed binary into the primary
    # archive. Follow the actual primary publication and retain its copy origin;
    # never query an arbitrary PPA or infer a build from a versioned filename.
    if archive != ARCHIVE:
        require(all(entry.get('copied_from_archive_link') == archive for entry in published),
                'Non-primary build archive lacks matching original publication copy provenance')
    for field in ('changesfile_url', 'buildinfo_url', 'build_log_url'):
        official_url(build.get(field))


def control_record(path, package, *, buildinfo=False):
    text = Path(path).read_text(encoding='utf-8')
    stanzas = debian.control_stanzas(text, clear_signed=True)
    require(len(stanzas) == 1, 'An original build record must have exactly one control stanza')
    value = stanzas[0]
    require(value.get('version') == package['version'] and
            source_field(value.get('source'), value.get('version', '')) ==
            (package['sourcePackage'], package['sourceVersion']) and
            package['binaryPackage'].split(':')[0] in value.get('binary', '').split() and
            package['architecture'] in value.get('architecture', '').split(),
            'Original build control record differs from the exact binary/source/version/architecture')
    if buildinfo:
        require(value.get('build-architecture') == package['architecture'],
                'Original buildinfo names a different actual build architecture')
    return value, {'present': text.startswith('-----BEGIN PGP SIGNED MESSAGE-----'), 'verified': False}


def bind_records(package, archive, changes_path, buildinfo_path, log_path):
    changes, changes_signature = control_record(changes_path, package)
    buildinfo, buildinfo_signature = control_record(buildinfo_path, package, buildinfo=True)
    changes_files, buildinfo_files = debian.checksum_files(changes), debian.checksum_files(buildinfo)
    expected_archive = debian.identity(archive)
    require(changes_files.get(Path(archive).name) == expected_archive and
            buildinfo_files.get(Path(archive).name) == expected_archive,
            'Original changes/buildinfo SHA256 does not bind the exact retained .deb')
    require(changes_files.get(Path(buildinfo_path).name) == debian.identity(buildinfo_path),
            'Original changes SHA256 does not bind the raw buildinfo bytes')
    require(all(changes_files[name] == buildinfo_files[name] for name in changes_files.keys() & buildinfo_files.keys()),
            'Original changes and buildinfo disagree about shared file SHA256/size')
    log_pin = changes_files.get(Path(log_path).name)
    require(log_pin is None or log_pin == debian.identity(log_path),
            'Original changes SHA256 does not match the retained build log')
    return {'archive': {'name': Path(archive).name, **expected_archive},
            'buildinfo': {'name': Path(buildinfo_path).name, **debian.identity(buildinfo_path)},
            'changesSignature': changes_signature, 'buildinfoSignature': buildinfo_signature,
            'sharedChecksums': sorted(changes_files.keys() & buildinfo_files.keys()),
            'buildLogListedInChanges': log_pin is not None,
            'observedFields': {key: buildinfo[key] for key in ('build-origin', 'build-date', 'build-path',
                               'installed-build-depends', 'environment') if key in buildinfo}}


def collect(system_sources, series, output, project):
    system_sources, output = Path(system_sources), Path(output).resolve()
    require(system_sources.is_file() and not system_sources.is_symlink() and
            0 < system_sources.stat().st_size <= MAX_API, 'A bounded original system-sources report is required')
    original_identity = debian.identity(system_sources)
    original = strict_json(system_sources.read_bytes())
    require(original.get('schemaVersion') == 1 and original.get('kind') == 'huhucapture-debian-source-materials' and
            original.get('status') == 'original-packages-and-sources-retained',
            'The successful original system source collection is required')
    build = original.get('buildIdentity', {})
    require(build.get('platform') == 'linux' and build.get('architecture') == 'amd64' and
            re.fullmatch(r'[0-9a-f]{40}', build.get('sourceCommit', '')),
            'The original Linux amd64 project build identity is required')
    packages = original.get('binaryPackages')
    require(isinstance(packages, list) and 0 < len(packages) <= MAX_PACKAGES,
            'Original exact binary packages are absent or oversized')
    names = [package_identity(item)['binaryPackage'] for item in packages]
    require(len(set(names)) == len(names), 'Duplicate original binary packages')
    original_gaps = original.get('gaps')
    require(isinstance(original_gaps, list) and all(isinstance(gap, str) for gap in original_gaps),
            'Original system source collection must retain its explicit gaps')
    require(not output.exists() and not system_sources.resolve().is_relative_to(output) and
            not output.is_relative_to(system_sources.resolve().parent),
            'Build record output must be a new directory separate from original system sources')
    output.mkdir(parents=True)
    downloader = Downloader(output)
    report = {'schemaVersion': 1, 'kind': 'huhucapture-launchpad-build-materials', 'status': 'collecting',
              'buildIdentity': build, 'series': series, 'binaryPackages': [], 'requests': downloader.records,
              'correspondingSourceComplete': False, 'originalCollectionGaps': original_gaps, 'gaps': [
                  'debian-repository: the original signed Release/index chain is not verified by this collection',
                  'launchpad-signatures: original .changes/.buildinfo OpenPGP signatures have not been verified',
                  'launchpad-build: raw build logs and buildinfo are not a complete verified configure/link command or static-input inventory',
                  'launchpad-receipt: no independently verified native producer receipt or reproducible rebuild is available'],
              'scope': 'Original API/build records and exact retained .deb byte binding; no NativeBuild receipt'}
    try:
        retain_original(system_sources, original_identity, output / 'inputs/system-sources.json')
        verified = []
        for original_package in packages:
            directory = output / 'packages' / original_package['binaryPackage'].replace(':', '_')
            package, archive = validate_package_input(original_package, directory / 'inputs')
            verified.append((directory, package, archive))
        # No package lookup is made before all retained .debs and APT records
        # have passed their original hashes and actual dpkg metadata check.
        series_link, architecture_link = resolve_series(downloader, series, output / 'api')
        for directory, package, archive in verified:
            published, link = publications(downloader, package, architecture_link, directory / 'api')
            build_record = downloader.get_json(link, directory / 'api/build.json')
            validate_build(build_record, link, package, series_link, published)
            artifacts = {}
            for field, limit, suffix in (('changesfile_url', MAX_CONTROL, '.changes'),
                                          ('buildinfo_url', MAX_CONTROL, '.buildinfo'),
                                          ('build_log_url', MAX_LOG, None)):
                name = artifact_name(build_record[field])
                require(suffix is None or name.endswith(suffix), 'Original build artifact has an unexpected file type')
                path = directory / 'originals' / name
                downloader.fetch(build_record[field], path, limit=limit)
                artifacts[field] = path
            binding = bind_records(package, archive, artifacts['changesfile_url'],
                                   artifacts['buildinfo_url'], artifacts['build_log_url'])
            report['binaryPackages'].append({**package, 'buildLink': link,
                'buildArchive': build_record['archive_link'], 'publications': [entry['self_link'] for entry in published],
                'bindings': binding, 'originals': {field: path.relative_to(output).as_posix() for field, path in artifacts.items()}})
            if not binding['buildLogListedInChanges']:
                report['gaps'].append(package['binaryPackage'] + ': build log is retained from the original API URL but lacks an independent original checksum/signature binding')
        require(debian.identity(system_sources) == original_identity and
                debian.identity(output / 'inputs/system-sources.json') == original_identity,
                'Original system source report changed during collection')
        for package in packages:
            for field in ('archive', 'originalIndex'):
                require(debian.identity(Path(package[field]['path'])) == expected_digest(package[field]),
                        'Original binary package/index changed during collection')
            directory = output / 'packages' / package['binaryPackage'].replace(':', '_') / 'inputs'
            require(debian.identity(directory / Path(package['archive']['path']).name) == expected_digest(package['archive']) and
                    debian.identity(directory / 'Packages-records.txt') == expected_digest(package['originalIndex']),
                    'Retained binary package/index changed during collection')
        require(all(debian.identity(output / record['path']) == expected_digest(record) for record in downloader.records),
                'Original Launchpad response changed during collection')
        report['status'] = 'original-build-records-bound-to-debs'
        # Materials only: retaining a raw .buildinfo must never mint a native
        # producer receipt or an origin mapping for final, transformed ELF bytes.
        # Empty subprocess streams are represented exactly by size/SHA256 in
        # dpkg-deb.json. The generic source material collector accepts nonempty
        # files; do not turn an empty stderr into invented text to satisfy it.
        files = sorted([path for path in output.rglob('*') if path.is_file() and path.stat().st_size] +
                       [output / 'launchpad-build-records.json'])
        plan = {'schemaVersion': 1, 'platform': 'linux', 'project': str(Path(project).resolve()),
                'buildIdentity': build, 'materials': [{'id': 'launchpad-record-' + str(index), 'path': str(path)}
                                                     for index, path in enumerate(files)]}
        write_json(output / 'materials-plan.json', plan)
        write_json(output / 'launchpad-build-records.json', report)
        return report
    except Exception as error:
        report.update(status='failed', failure=str(error))
        if not (output / 'launchpad-build-records.json').exists():
            write_json(output / 'launchpad-build-records.json', report)
        raise
