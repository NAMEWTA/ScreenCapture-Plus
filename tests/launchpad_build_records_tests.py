#!/usr/bin/env python3
"""Offline rejection tests plus a real local dpkg archive; no Launchpad claim.

HTTP documents below are deliberately synthetic fixtures. They test the
collector's binding/transport contracts, not any Ubuntu producer or signature.
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('launchpad_records_test', ROOT / 'build/package/launchpad_build_records.py')
records = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(records)


def digest(data):
    return {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def checksum(name, data):
    value = digest(data)
    return f" {value['sha256']} {value['size']} {name}\n"


class Response(io.BytesIO):
    status = 200

    def __init__(self, url, data, headers=None):
        super().__init__(data)
        self.url = url
        self.headers = {'Content-Length': str(len(data)), **(headers or {})}

    def geturl(self):
        return self.url


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='launchpad-offline-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = {'binaryPackage': 'libfixture:amd64', 'version': '1.0-1+b1', 'architecture': 'amd64',
                        'sourcePackage': 'source-fixture', 'sourceVersion': '1.0-1'}
        self.archive = self.root / 'libfixture_1.0-1+b1_amd64.deb'
        self.archive.write_bytes(b'Explicit offline checksum fixture, not an Ubuntu binary archive\n')
        self.changes = self.root / 'source-fixture_1.0-1+b1_amd64.changes'
        self.buildinfo = self.root / 'source-fixture_1.0-1+b1_amd64.buildinfo'
        self.log = self.root / 'source-fixture.buildlog.gz'
        self.log.write_bytes(b'Explicit offline raw build-log fixture\n')
        self.make_controls()
        self.series = records.UBUNTU + '/noble'
        self.architecture = self.series + '/amd64'
        self.build_link = records.UBUNTU + '/+source/source-fixture/1.0-1/+build/123'
        self.publication = {'self_link': records.ARCHIVE + '/+binarypub/42', 'archive_link': records.ARCHIVE,
            'binary_package_name': 'libfixture', 'binary_package_version': '1.0-1+b1', 'source_package_name': 'source-fixture',
            'source_package_version': '1.0-1', 'distro_arch_series_link': self.architecture,
            'architecture_specific': True, 'build_link': self.build_link, 'status': 'Published', 'pocket': 'Updates',
            'copied_from_archive_link': None}
        self.build = {'self_link': self.build_link, 'arch_tag': 'amd64', 'archive_link': records.ARCHIVE,
            'source_package_name': 'source-fixture', 'source_package_version': '1.0-1',
            'distribution_link': records.UBUNTU, 'distro_series_link': self.series, 'buildstate': 'Successfully built',
            'datebuilt': '2026-01-01T00:00:00Z',
            **{field: 'https://launchpad.net/ubuntu/+source/source-fixture/1.0-1/+build/123/+files/' + path.name
               for field, path in (('changesfile_url', self.changes), ('buildinfo_url', self.buildinfo), ('build_log_url', self.log))}}

    def make_controls(self, *, buildinfo_extra='', changes_extra='', signed=False):
        common = ('Format: 1.0\nSource: source-fixture (1.0-1)\nBinary: libfixture\n'
                  'Architecture: amd64\nVersion: 1.0-1+b1\n')
        text = (common + 'Build-Architecture: amd64\nBuild-Origin: Ubuntu\n'
                'Installed-Build-Depends:\n gcc (= 1.0)\nEnvironment:\n LANG="C.UTF-8"\n'
                'Checksums-Sha256:\n' + checksum(self.archive.name, self.archive.read_bytes()) + buildinfo_extra)
        self.buildinfo.write_bytes(text.encode())
        text = (common + 'Checksums-Sha256:\n' + checksum(self.archive.name, self.archive.read_bytes()) +
                checksum(self.buildinfo.name, self.buildinfo.read_bytes()) + changes_extra)
        if signed:
            text = ('-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n\n' + text +
                    '-----BEGIN PGP SIGNATURE-----\nTEST-ONLY-NOT-A-SIGNATURE\n-----END PGP SIGNATURE-----\n')
        self.changes.write_bytes(text.encode())

    def bind(self):
        return records.bind_records(self.package, self.archive, self.changes, self.buildinfo, self.log)


class ProtocolTests(Fixture):
    def test_exact_hash_binding_preserves_distinct_source_and_binary_versions(self):
        self.make_controls(signed=True)
        result = self.bind()
        self.assertEqual(result['archive'], {'name': self.archive.name, **digest(self.archive.read_bytes())})
        self.assertEqual(result['changesSignature'], {'present': True, 'verified': False})
        self.assertEqual(result['buildinfoSignature'], {'present': False, 'verified': False})
        self.assertFalse(result['buildLogListedInChanges'])
        self.assertIn('installed-build-depends', result['observedFields'])

    def test_original_changes_must_bind_raw_buildinfo_bytes(self):
        self.buildinfo.write_bytes(self.buildinfo.read_bytes().replace(b'LANG="C.UTF-8"', b'LANG="C"'))
        with self.assertRaisesRegex(ValueError, 'raw buildinfo'):
            self.bind()

    def test_both_controls_must_bind_original_deb_not_just_filename(self):
        self.archive.write_bytes(self.archive.read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'exact retained .deb'):
            self.bind()

    def test_shared_source_file_hashes_cannot_disagree(self):
        self.make_controls(buildinfo_extra=checksum('source-fixture_1.0-1.dsc', b'original source'),
                           changes_extra=checksum('source-fixture_1.0-1.dsc', b'different source'))
        with self.assertRaisesRegex(ValueError, 'shared file'):
            self.bind()

    def test_wrong_control_identity_or_architecture_is_rejected(self):
        for before, after in ((b'Source: source-fixture (1.0-1)', b'Source: other-source (1.0-1)'),
                              (b'Version: 1.0-1+b1', b'Version: 1.0-2'),
                              (b'Binary: libfixture', b'Binary: libother'),
                              (b'Architecture: amd64', b'Architecture: arm64'),
                              (b'Build-Architecture: amd64', b'Build-Architecture: arm64')):
            with self.subTest(before=before):
                self.make_controls()
                self.buildinfo.write_bytes(self.buildinfo.read_bytes().replace(before, after))
                with self.assertRaises(ValueError):
                    self.bind()

    def test_duplicate_control_fields_and_unsafe_checksum_paths_are_rejected(self):
        self.make_controls(changes_extra=checksum('../escape.deb', b'fixture'))
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            self.bind()
        self.make_controls()
        self.changes.write_bytes(self.changes.read_bytes() + b'Source: other-source\n')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.bind()

    def test_build_requires_exact_success_source_version_architecture_and_series(self):
        for key, wrong in (('buildstate', 'Failed to build'), ('source_package_name', 'other-source'),
                           ('source_package_version', '1.0-2'), ('arch_tag', 'arm64'),
                           ('distro_series_link', records.UBUNTU + '/jammy'), ('buildinfo_url', None)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                records.validate_build({**self.build, key: wrong}, self.build_link, self.package,
                                       self.series, [self.publication])

    def test_copied_primary_publication_must_bind_the_actual_build_archive(self):
        archive = records.API + '/~ubuntu-security-proposed/+archive/ubuntu/ppa'
        build = {**self.build, 'archive_link': archive}
        with self.assertRaisesRegex(ValueError, 'copy provenance'):
            records.validate_build(build, self.build_link, self.package, self.series, [self.publication])
        records.validate_build(build, self.build_link, self.package, self.series,
                               [{**self.publication, 'copied_from_archive_link': archive}])

    def test_publications_do_not_take_the_first_different_producer(self):
        downloader = records.Downloader(self.root)
        entries = [self.publication, {**self.publication, 'self_link': records.ARCHIVE + '/+binarypub/43',
                                     'build_link': self.build_link.replace('123', '124')}]
        with patch.object(downloader, 'get_json', return_value={'entries': entries}), self.assertRaisesRegex(ValueError, 'Ambiguous'):
            records.publications(downloader, self.package, self.architecture, self.root)

    def test_publication_fields_and_pagination_filters_cannot_drift(self):
        downloader = records.Downloader(self.root)
        for key, wrong in (('source_package_version', '1.0-2'), ('binary_package_version', '1.0-2'),
                           ('archive_link', records.API + '/~other/+archive/ubuntu/ppa'),
                           ('distro_arch_series_link', self.series + '/arm64')):
            with self.subTest(key=key), patch.object(downloader, 'get_json', return_value={
                    'entries': [{**self.publication, key: wrong}]}), self.assertRaisesRegex(ValueError, 'publication differs'):
                records.publications(downloader, self.package, self.architecture, self.root)
        with patch.object(downloader, 'get_json', return_value={'entries': [self.publication],
                'next_collection_link': records.ARCHIVE + '?ws.op=getPublishedBinaries&binary_name=other&ws.start=75&ws.size=75'}), \
                self.assertRaisesRegex(ValueError, 'pagination changed'):
            records.publications(downloader, self.package, self.architecture, self.root)

    def test_transport_rejects_unofficial_or_downgraded_redirects(self):
        handler = records.OfficialRedirect(api=False)
        request = Request('https://launchpad.net/original')
        for url in ('http://launchpad.net/file', 'https://launchpad.net.evil.test/file',
                    'https://evil.test/launchpad.net/file', 'https://user@launchpad.net/file',
                    'https://launchpad.net:444/file', 'https://launchpad.net/file#fragment',
                    'https://launchpad.net/\nfile'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                handler.redirect_request(request, None, 302, 'Found', {}, url)
        redirected = handler.redirect_request(request, None, 302, 'Found', {},
                                               'https://launchpadlibrarian.net/123/original.buildinfo')
        self.assertEqual(redirected.full_url, 'https://launchpadlibrarian.net/123/original.buildinfo')
        with self.assertRaises(ValueError):
            records.OfficialRedirect(api=True).redirect_request(request, None, 302, 'Found', {}, redirected.full_url)
        for name in ('../escape', '%2e%2e', 'folder%2ffile'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                records.artifact_name('https://launchpad.net/' + name)

    def test_downloader_retains_original_bytes_and_rejects_oversize_or_truncation(self):
        url = 'https://launchpad.net/original.buildinfo'
        raw = b'original\r\nbytes\xff\x00'
        downloader = records.Downloader(self.root)
        with patch.object(records, 'build_opener') as factory:
            factory.return_value.open.return_value = Response(url, raw)
            result = downloader.fetch(url, self.root / 'original', limit=100)
            self.assertEqual((self.root / 'original').read_bytes(), raw)
            self.assertEqual({key: result[key] for key in ('size', 'sha256')}, digest(raw))
            factory.return_value.open.return_value = Response(url, b'123456789', {'Content-Length': '8'})
            with self.assertRaisesRegex(ValueError, 'size limit'):
                downloader.fetch(url, self.root / 'oversize', limit=8)
            factory.return_value.open.return_value = Response(url, b'123', {'Content-Length': '8'})
            with self.assertRaisesRegex(ValueError, 'truncated'):
                downloader.fetch(url, self.root / 'truncated', limit=8)

    def test_duplicate_api_identity_fields_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON'):
            records.strict_json(b'{"buildstate":"Successfully built","buildstate":"Failed to build"}')


@unittest.skipUnless(shutil.which('dpkg-deb'), 'Actual dpkg archive integration requires dpkg-deb')
class OriginalPackageTests(Fixture):
    def setUp(self):
        super().setUp()
        package_root = self.root / 'package'
        (package_root / 'DEBIAN').mkdir(parents=True)
        (package_root / 'DEBIAN/control').write_text('Package: libfixture\nVersion: 1.0-1+b1\nArchitecture: amd64\n'
            'Source: source-fixture (1.0-1)\nMaintainer: Offline Test <test@example.invalid>\n'
            'Description: Explicit local collector fixture, not an Ubuntu package\n', encoding='utf-8')
        (package_root / 'usr/lib').mkdir(parents=True)
        payload = package_root / 'usr/lib/fixture.txt'
        payload.write_bytes(b'Explicit original local payload fixture\n')
        self.archive.unlink()
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(package_root), str(self.archive)],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
        self.make_controls()
        self.input_root = self.root / 'original'
        self.input_root.mkdir()
        index = self.input_root / 'Packages-records.txt'
        index.write_text('Package: libfixture\nVersion: 1.0-1+b1\nArchitecture: amd64\n'
            'Source: source-fixture (1.0-1)\n' +
            f"SHA256: {digest(self.archive.read_bytes())['sha256']}\nSize: {self.archive.stat().st_size}\n", encoding='utf-8')
        self.report_path = self.input_root / 'system-sources.json'
        self.original = {'schemaVersion': 1, 'kind': 'huhucapture-debian-source-materials',
            'status': 'original-packages-and-sources-retained',
            'buildIdentity': {'platform': 'linux', 'architecture': 'amd64', 'sourceCommit': 'a' * 40},
            'binaryPackages': [{**self.package, 'archive': {'path': str(self.archive), **digest(self.archive.read_bytes())},
                               'originalIndex': {'path': str(index), **digest(index.read_bytes())},
                               'files': [{'installedPath': 'lib/fixture.txt', 'packagePath': 'usr/lib/fixture.txt',
                                          **digest(payload.read_bytes())}]}],
            'gaps': ['Original Release chain unverified', 'Original native producer receipt missing']}
        self.report_path.write_text(json.dumps(self.original), encoding='utf-8')
        self.requested = []

    def response(self, request, timeout):
        url = request.full_url
        self.requested.append(url)
        parsed = urlsplit(url)
        query = parse_qs(parsed.query)
        operation = query.get('ws.op', [None])[0]
        if operation == 'getSeries':
            value = {'self_link': self.series, 'name': 'noble', 'distribution_link': records.UBUNTU,
                     'main_archive_link': records.ARCHIVE}
        elif operation == 'getDistroArchSeries':
            value = {'self_link': self.architecture, 'architecture_tag': 'amd64', 'distroseries_link': self.series,
                     'main_archive_link': records.ARCHIVE}
        elif operation == 'getPublishedBinaries':
            self.assertEqual(query['binary_name'], ['libfixture'])
            self.assertEqual(query['version'], ['1.0-1+b1'])
            self.assertEqual(query['distro_arch_series'], [self.architecture])
            self.assertEqual(query['exact_match'], ['true'])
            value = {'entries': [self.publication]}
        elif url == self.build_link:
            value = self.build
        else:
            for path in (self.changes, self.buildinfo, self.log):
                if url.endswith('/' + path.name):
                    return Response(url, path.read_bytes())
            self.fail('Unexpected offline test request: ' + url)
        return Response(url, json.dumps(value).encode())

    def test_actual_deb_metadata_and_offline_api_collection_keep_all_gaps(self):
        before = self.report_path.read_bytes()
        output = self.root / 'collected'
        with patch.object(records, 'build_opener') as factory:
            factory.return_value.open.side_effect = self.response
            result = records.collect(self.report_path, 'noble', output, ROOT)
        self.assertEqual(result['status'], 'original-build-records-bound-to-debs')
        self.assertFalse(result['correspondingSourceComplete'])
        self.assertEqual(result['originalCollectionGaps'], self.original['gaps'])
        self.assertTrue(any('signed Release' in gap for gap in result['gaps']))
        self.assertTrue(any('static-input' in gap for gap in result['gaps']))
        self.assertTrue(any('producer receipt' in gap for gap in result['gaps']))
        self.assertEqual(self.report_path.read_bytes(), before)
        plan = json.loads((output / 'materials-plan.json').read_bytes())
        self.assertNotIn('buildReceipts', plan)
        self.assertNotIn('sources', plan)
        self.assertNotIn('sourceBuildReceipt', json.dumps(plan))
        assembler = runpy.run_path(str(ROOT / 'scripts/assemble-source-plan.py'))
        relocated = assembler['relocate_plan'](plan, output)
        merged = assembler['merge_plans']([
            {'schemaVersion': 1, 'platform': 'linux', 'project': str(ROOT),
             'buildIdentity': str(self.root / 'actual-build-identity.json')}, relocated])
        self.assertEqual(merged['buildIdentity'], str(self.root / 'actual-build-identity.json'))
        self.assertEqual(merged['materials'], sorted(relocated['materials'], key=lambda row: row['id']))
        self.assertEqual(result['buildIdentity'], self.original['buildIdentity'])
        for material in plan['materials']:
            self.assertGreater(Path(material['path']).stat().st_size, 0)
        for field, path in (('changesfile_url', self.changes), ('buildinfo_url', self.buildinfo), ('build_log_url', self.log)):
            self.assertEqual((output / result['binaryPackages'][0]['originals'][field]).read_bytes(), path.read_bytes())
        self.assertEqual(len(self.requested), 7)

    def test_changed_original_deb_fails_before_any_network_request(self):
        self.archive.write_bytes(self.archive.read_bytes() + b'changed')
        with patch.object(records, 'build_opener') as factory, self.assertRaisesRegex(ValueError, 'input changed'):
            records.collect(self.report_path, 'noble', self.root / 'failed', ROOT)
        factory.assert_not_called()
        result = json.loads((self.root / 'failed/launchpad-build-records.json').read_bytes())
        self.assertEqual(result['status'], 'failed')
        self.assertFalse((self.root / 'failed/materials-plan.json').exists())

    def test_actual_deb_source_fields_cannot_be_replaced_by_report_claims(self):
        package = self.original['binaryPackages'][0]
        package['sourcePackage'] = 'other-source'
        index = Path(package['originalIndex']['path'])
        index.write_bytes(index.read_bytes().replace(b'source-fixture', b'other-source'))
        package['originalIndex'].update(digest(index.read_bytes()))
        self.report_path.write_text(json.dumps(self.original), encoding='utf-8')
        with patch.object(records, 'build_opener') as factory, self.assertRaisesRegex(ValueError, 'Actual .deb metadata'):
            records.collect(self.report_path, 'noble', self.root / 'failed', ROOT)
        factory.assert_not_called()

    def test_http_denial_retains_original_error_and_never_tries_another_endpoint(self):
        body = b'Forbidden. Calls to this URL are not allowed.'
        output = self.root / 'denied'
        with patch.object(records, 'build_opener') as factory:
            factory.return_value.open.side_effect = HTTPError(records.UBUNTU, 403, 'Forbidden', {}, io.BytesIO(body))
            with self.assertRaises(HTTPError):
                records.collect(self.report_path, 'noble', output, ROOT)
            self.assertEqual(factory.return_value.open.call_count, 1)
        self.assertEqual((output / 'api/series.json.http-error').read_bytes(), body)
        result = json.loads((output / 'launchpad-build-records.json').read_bytes())
        self.assertEqual(result['status'], 'failed')
        self.assertFalse((output / 'materials-plan.json').exists())


if __name__ == '__main__':
    unittest.main()
