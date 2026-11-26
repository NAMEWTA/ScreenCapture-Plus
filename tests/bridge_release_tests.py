#!/usr/bin/env python3
"""Publication admission regressions; no native run or release is fabricated."""
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts'))
path = ROOT / 'scripts/publish-bridge-release.py'
bridge = types.ModuleType('bridge_release_contract')
bridge.__file__ = str(path)
exec(compile(path.read_bytes(), str(path), 'exec'), bridge.__dict__)


class BridgeReleaseAdmission(unittest.TestCase):
    def declaration_root(self, root):
        (root / 'releases').mkdir()
        value = {'schemaVersion': 1, 'kind': 'legacy-windows-bridge', 'tag': 'v0.0.6',
                 'notes': 'releases/v0.0.6.md', 'previousRelease': 'v0.0.5'}
        (root / value['notes']).write_text('Unit-test declaration only; no release action.\n')
        return value

    def test_no_declaration_means_no_automatic_old_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.declaration_root(root)
            with self.assertRaisesRegex(ValueError, 'JSON evidence'):
                bridge.declaration('1' * 40, root=root, version='0.0.5')

    def test_bridge_intent_is_separate_and_version_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            value = self.declaration_root(root)
            declaration = root / 'releases/bridge-publish.json'
            declaration.write_text(json.dumps(value))
            actual, _ = bridge.declaration('1' * 40, root=root, version='0.0.6')
            self.assertEqual(actual, value)
            for change, message in (({'kind': 'qt-complete-release'}, 'separate explicit'),
                                    ({'schemaVersion': True}, 'separate explicit'),
                                    ({'tag': 'v0.1.0'}, 'single product version'),
                                    ({'previousRelease': 'v0.0.6'}, 'older stable'),
                                    ({'notes': '../outside.md'}, 'matching notes')):
                with self.subTest(change=change):
                    declaration.write_text(json.dumps({**value, **change}))
                    with self.assertRaisesRegex(ValueError, message):
                        bridge.declaration('1' * 40, root=root, version='0.0.6')

    def test_bridge_refuses_qt_or_sixth_asset_before_native_inspection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'release-manifest.json').write_text('{}\n')
            with self.assertRaisesRegex(ValueError, 'exactly the existing five'):
                bridge.verify_assets(root, '1' * 40, '0.0.6')

    def test_real_archive_paths_and_case_collisions_are_rejected(self):
        for names in (('../escape.txt',), ('/absolute.txt',), ('C:/drive.txt',),
                      ('folder\\escape.txt',), ('same.txt', 'SAME.txt')):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'not-a-package.zip'
                with zipfile.ZipFile(path, 'x') as archive:
                    for name in names:
                        archive.writestr(name, b'Owned archive admission fixture, never a product.')
                with self.assertRaisesRegex(ValueError, 'unsafe path|duplicate'):
                    bridge.archive_files(path, 1024)

    def test_real_archive_byte_budget_and_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'not-a-package.zip'
            data = b'Actual owned input; no executable or native result claimed.'
            with zipfile.ZipFile(path, 'x') as archive:
                archive.writestr('fixture.txt', data)
            result = bridge.archive_files(path, len(data))
            self.assertEqual(result['fixture.txt']['size'], len(data))
            import hashlib
            self.assertEqual(result['fixture.txt']['sha256'], hashlib.sha256(data).hexdigest())
            with self.assertRaisesRegex(ValueError, 'uncompressed budget'):
                bridge.archive_files(path, len(data) - 1)

    def incomplete_report(self):
        # Deliberately no invented native cases: these inputs must never pass
        # the actual producer-evidence gate, even when their labels look valid.
        executable = {'name': 'huhucapture.exe', 'size': 4096, 'sha256': '2' * 64}
        report = {'schemaVersion': 1, 'purpose': 'legacy-windows-bridge-contract',
                  'repository': bridge.REPOSITORY, 'sourceCommit': '1' * 40, 'version': '0.0.6',
                  'tag': 'v0.0.6', 'configuration': 'Release', 'nativeExecutable': executable,
                  'controlledPackageContractPassed': True, 'transportFixtureIncluded': True,
                  'actualCompletePackageRun': False, 'controlledPackageFixture': {
                      'name': 'huhucapture-v0.0.6-windows-x64.zip', 'size': 70 * 1024 * 1024, 'sha256': '3' * 64},
                  'cases': []}
        return report, executable

    def test_plausible_labels_cannot_replace_actual_native_cases(self):
        report, executable = self.incomplete_report()
        with self.assertRaisesRegex(ValueError, 'did not execute'):
            bridge.verify_report(report, 'Release', '1' * 40, '0.0.6', executable)

    def test_report_identity_and_scope_tampering_fail_closed(self):
        report, executable = self.incomplete_report()
        for change, message in (({'sourceCommit': '4' * 40}, 'actual source'),
                                ({'nativeExecutable': {**executable, 'sha256': '5' * 64}}, 'actual source'),
                                ({'schemaVersion': True}, 'actual source'),
                                ({'configuration': 'Debug'}, 'actual source'),
                                ({'controlledPackageContractPassed': 1}, 'explicit controlled'),
                                ({'actualCompletePackageRun': True}, 'explicit controlled')):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, message):
                bridge.verify_report({**report, **change}, 'Release', '1' * 40, '0.0.6', executable)

    def test_pr_association_or_another_source_is_not_main_native_evidence(self):
        value = {'head_sha': '1' * 40, 'head_branch': 'main', 'path': '.github/workflows/windows.yml',
                 'head_repository': {'full_name': bridge.REPOSITORY}, 'status': 'completed',
                 'conclusion': 'success', 'event': 'push'}
        for change in ({'event': 'pull_request'}, {'head_sha': '2' * 40}, {'head_branch': 'refactor'},
                       {'status': 'in_progress'}, {'conclusion': 'failure'}):
            with self.subTest(change=change), mock.patch.object(bridge, 'api', return_value={**value, **change}) as request:
                with self.assertRaisesRegex(ValueError, 'successful canonical main'):
                    bridge.verify_windows_run('1' * 40, '123')
                self.assertEqual(request.call_args_list, [mock.call('actions/runs/123')])


if __name__ == '__main__':
    unittest.main()
