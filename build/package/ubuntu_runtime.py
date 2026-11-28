"""Bind native Ubuntu files to signed distribution inputs and original build records."""
from __future__ import annotations

import json
from pathlib import Path, PurePosixPath
import runpy
import subprocess
import tempfile


DIRECTORY = Path(__file__).resolve().parent
launchpad = runpy.run_path(str(DIRECTORY / 'launchpad_build_records.py'))
indexes = runpy.run_path(str(DIRECTORY / 'ubuntu_archive_indexes.py'))
debian = launchpad['debian']
require = debian.require


class ArchiveProof:
    def __init__(self, collector, system):
        self.collector = collector
        self.authenticated = system['authenticatedIndexes']
        self.releases, self.indexes = {}, {}
        self.keyring = self.material('ubuntu-archive-keyring')
        require(debian.identity(self.keyring) == debian.identity(indexes['KEYRING']),
                'Retained Ubuntu signing keyring differs from the installed archive trust anchor')
        self.wanted = {'binaries': {item['binaryPackage'].split(':')[0] for item in system['binaryPackages']},
                       'sources': {item['source']['package'] for item in system['sourcePackages']}}

    def material(self, identifier):
        require(identifier in self.collector.materials, 'Missing retained Ubuntu material: ' + identifier)
        return self.collector.stage / self.collector.materials[identifier]['path']

    def prove(self, category, package, version, verify):
        for binding in self.authenticated[category].get(package + '=' + version, []):
            release_id, index_id, index_key = (binding[key] for key in ('release', 'index', 'indexKey'))
            require(indexes['INDEX_KEY'].fullmatch(index_key) and
                    (category == 'binaries') == index_key.endswith('/Packages'), 'Unexpected original Ubuntu index kind')
            if release_id not in self.releases:
                release = self.material(release_id)
                result = subprocess.run(['gpgv', '--keyring', self.keyring, '--status-fd', '1', release],
                                        capture_output=True, text=True, timeout=30, check=False)
                require(result.returncode == 0 and any(line.startswith('[GNUPG:] VALIDSIG ')
                        for line in result.stdout.splitlines()), 'Ubuntu archive index signature verification failed')
                self.releases[release_id] = indexes['release_hashes'](release, debian)
            if index_id not in self.indexes:
                index = self.material(index_id)
                require(debian.identity(index) == self.releases[release_id].get(index_key) == binding['indexIdentity'],
                        'Retained Ubuntu APT index differs from the signed original SHA256/size')
                stanzas = indexes['selected_stanzas'](index, self.wanted[category], debian)
                self.indexes[index_id] = release_id, index_key, stanzas
            anchor, key, records = self.indexes[index_id]
            require((anchor, key) == (release_id, index_key), 'Inconsistent identity for one Ubuntu index')
            if any(verify(record) for record in records.get((package, version), [])):
                return {'release': release_id, 'index': index_id}
        return None


def original_build(collector, path, upload, record, archive):
    package = launchpad['package_identity'](record)
    directory = path.parent / 'packages' / package['binaryPackage'].replace(':', '_')
    build = json.loads(collector.original_material(directory / 'api/build.json').read_text(encoding='utf-8'))
    expected_pages = set(upload['publications'])
    pages = []
    index = 0
    while collector.has_original_material(directory / 'api' / f'publications-{index}.json'):
        value = collector.original_material(directory / 'api' / f'publications-{index}.json')
        pages.extend(json.loads(value.read_text(encoding='utf-8'))['entries'])
        index += 1
    series = launchpad['UBUNTU'] + '/' + upload['series']
    require(expected_pages and {item['self_link'] for item in pages} == expected_pages and all(
            item.get('archive_link') == launchpad['ARCHIVE'] and item.get('build_link') == upload['buildLink'] and
            item.get('binary_package_name') == package['binaryPackage'].split(':')[0] and
            item.get('binary_package_version') == package['version'] and
            item.get('source_package_name') == package['sourcePackage'] and
            item.get('source_package_version') == package['sourceVersion'] and
            item.get('distro_arch_series_link') == series + '/amd64' for item in pages),
            'Original Ubuntu build publications disagree with the deployed package')
    launchpad['validate_build'](build, upload['buildLink'], package, series, pages)
    original_paths = upload['originals']
    names = ('changesfile_url', 'buildinfo_url', 'build_log_url')
    materials = [collector.original_material(path.parent / collector.safe_path(original_paths[name])) for name in names]
    require(all(material.name == launchpad['artifact_name'](build[name]) for name, material in zip(names, materials)),
            'Retained launchpad build file differs from the published record')
    binding = launchpad['bind_records'](package, archive, *materials)
    require(binding == upload['bindings'], 'Original Ubuntu build file bindings changed')
    return {'buildLink': upload['buildLink'], 'bindings': binding}


def collect(collector, system, builds, build_report_path):
    require(system['kind'] == 'huhucapture-debian-source-materials' and
            system['status'] == 'original-packages-and-sources-retained' and
            builds['kind'] == 'huhucapture-launchpad-build-materials' and
            builds['status'] == 'original-build-records-bound-to-debs' and
            system['buildIdentity'] == builds['buildIdentity'] and all(
                system['buildIdentity'][key] == collector.identity[key]
                for key in ('sourceCommit', 'product', 'repository', 'version')),
            'The original Ubuntu source and build records belong to a different application run')
    proof = ArchiveProof(collector, system)
    sources, binaries = {}, {}
    for identifier, item in collector.components.items():
        source = item['source']
        if source.get('kind') != 'debian-source':
            continue
        archive_pins = {part['name']: {key: part[key] for key in ('sha256', 'size')}
                        for part in [source['dsc'], *source['archives']]}
        anchor = proof.prove('sources', source['package'], source['version'],
                             lambda stanza: debian.checksum_files(stanza) == archive_pins)
        if anchor:
            sources[(source['package'], source['version'])] = identifier, anchor
    uploads = {item['binaryPackage']: item for item in builds['binaryPackages']}
    require(len(uploads) == len(builds['binaryPackages']), 'Duplicate published Ubuntu native package records')
    for record in system['binaryPackages']:
        package = launchpad['package_identity'](record)
        name = package['binaryPackage']
        source = sources.get((package['sourcePackage'], package['sourceVersion']))
        if name not in uploads or source is None:
            continue
        archive = collector.original_material(record['archive']['path'])
        pin = debian.identity(archive)
        require(pin == launchpad['expected_digest'](record['archive']), 'Retained Ubuntu binary archive changed')
        binary_name = name.split(':')[0]
        anchor = proof.prove('binaries', binary_name, package['version'], lambda stanza:
            stanza.get('architecture') == package['architecture'] and stanza.get('size') == str(pin['size']) and
            stanza.get('sha256') == pin['sha256'] and PurePosixPath(stanza.get('filename', '')).name == archive.name and
            launchpad['source_field'](stanza.get('source', binary_name), stanza['version']) ==
                (package['sourcePackage'], package['sourceVersion']))
        if anchor is None:
            continue
        publish = original_build(collector, build_report_path, {**uploads[name], 'series': builds['series']}, record, archive)
        files = {}
        identifier, source_anchor = source
        notice = record.get('copyright', {})
        copyright_source = 'usr/share/doc/' + binary_name + '/copyright'
        notice_pin = {key: notice[key] for key in ('size', 'sha256')} if notice else None
        retained_notice = 'debian-notice-' + name.replace(':', '_')
        notice_verified = False
        with tempfile.TemporaryDirectory(prefix='original-ubuntu-deb-', dir=collector.stage) as temporary:
            extracted = Path(temporary)
            subprocess.run(['dpkg-deb', '--extract', archive, extracted], check=True, timeout=60)
            for entry in record['files']:
                relative = collector.safe_path(entry['packagePath'])
                path = extracted / relative
                require(path.resolve(strict=True).is_relative_to(extracted) and not path.is_symlink(),
                        'Ubuntu native file escapes the exact extracted original binary package')
                byte_pin = debian.identity(path)
                require(byte_pin == {key: entry[key] for key in ('size', 'sha256')},
                        'Retained Ubuntu native file differs from its original binary package')
                with path.open('rb') as binary:
                    header = binary.read(20)
                    require(header.startswith(b'\x7fELF\x02\x01') and header[18:20] == b'\x3e\x00',
                            'Expected an actual original x64 Ubuntu ELF library')
                files[relative] = {'path': relative, **byte_pin}
            if notice_pin:
                published_notice = (extracted / copyright_source).resolve()
                if published_notice.is_relative_to(extracted) and published_notice.is_file():
                    notice_verified = debian.identity(published_notice) == notice_pin
        require(files, 'The published Ubuntu runtime package has no verified deployed file')
        result = {'package': package, 'source': identifier, 'sourceIndex': source_anchor,
                  'binaryIndex': anchor, 'build': publish, 'outputs': list(files.values())}
        if notice_pin:
            source_notice = collector.sources / identifier / 'debian/copyright'
            if notice_verified or debian.identity(source_notice) == notice_pin:
                require(debian.identity(proof.material(retained_notice)) == notice_pin,
                        'Retained Ubuntu copyright differs from its original source/package bytes')
                result['copyright'] = {'path': copyright_source, 'material': retained_notice, **notice_pin}
        binaries[name] = result
    return binaries, {identifier: anchor for identifier, anchor in sources.values()}
