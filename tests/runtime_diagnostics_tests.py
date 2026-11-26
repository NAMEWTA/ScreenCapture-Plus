#!/usr/bin/env python3
"""Real file/process retention contracts; these do not simulate a runtime build."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/retain-appimage-runtime-diagnostics.py'
api = types.ModuleType('runtime_diagnostics')
api.__file__ = str(SCRIPT)
exec(compile(SCRIPT.read_bytes(), str(SCRIPT), 'exec'), api.__dict__)


class RuntimeDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='runtime-diagnostics-')
        self.root = Path(self.temporary.name).resolve()
        self.workspace = self.root / 'original'
        self.workspace.mkdir()
        self.output = self.root / 'retained'

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, name, data):
        path = self.workspace / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_actual_failed_process_original_receipt_and_static_bytes_survive(self):
        arguments = [sys.executable, '-c', 'import sys;print("actual diagnostic");sys.stderr.write("actual failure\\n");sys.exit(7)']
        result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual(result.returncode, 7)
        self.write('toolchain-source-receipt/logs/0001.stdout.log', result.stdout)
        self.write('toolchain-source-receipt/logs/0001.stderr.log', result.stderr)
        execution = self.write('toolchain-source-receipt/build-execution.json', json.dumps(
            {'kind': 'diagnostic-test-execution', 'arguments': arguments, 'exitCode': result.returncode}).encode())
        build_info = self.write('build-info.json', b'{"buildComplete":false,"failures":["fixture"]}\n')
        self.write('control/logs/empty.log', b'')
        self.write('source-inputs/original.json', b'{"fixture":"original source identity"}\n')
        self.write('toolchain-build/config.mak', b'TARGET = x86_64-linux-musl\n')
        self.write('toolchain-build/obj_gcc/config.log', b'actual configuration log fixture\n')
        self.write('toolchain-build/obj_gcc/x86_64-linux-musl/libgcc/Makefile', b'original diagnostic fixture\n')
        self.write('toolchain-source-receipt/probe/probe.cc', b'int main() { return 0; }\n')
        for name in ('libgcc.a', 'libgcc_eh.a', 'crtbeginS.o'):
            self.write('toolchain/lib/gcc/x86_64-linux-musl/fixture/' + name, b'diagnostic fixture ' + name.encode())
        for name in ('libc.a', 'libm.a', 'libstdc++.a', 'rcrt1.o', 'crti.o', 'crtn.o'):
            self.write('toolchain/x86_64-linux-musl/lib/' + name, b'diagnostic fixture ' + name.encode())
        self.write('toolchain/libexec/gcc/cc1', b'unselected compiler program')
        self.write('sources/never-read', b'original sources remain in the complete evidence')
        before = {str(path): path.read_bytes() for path in self.workspace.rglob('*') if path.is_file()}
        completed = subprocess.run([sys.executable, str(SCRIPT), '--workspace', str(self.workspace),
                                    '--output', str(self.output)], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads((self.output / api.INDEX).read_bytes())
        self.assertEqual(report['state'], 'materials-retained')
        self.assertNotIn('buildComplete', report)
        self.assertEqual(report['materialBytes'], sum(item['size'] for item in report['files']))
        for item in report['files']:
            data = (self.output / item['path']).read_bytes()
            self.assertEqual(data, before[item['originalPath']])
            self.assertEqual(hashlib.sha256(data).hexdigest(), item['sha256'])
        self.assertEqual((self.output / 'materials' / execution.relative_to(self.workspace)).read_bytes(), execution.read_bytes())
        self.assertEqual((self.output / 'materials/build-info.json').read_bytes(), build_info.read_bytes())
        self.assertFalse((self.output / 'materials/sources').exists())
        self.assertFalse((self.output / 'materials/toolchain/libexec').exists())
        self.assertEqual(before, {str(path): path.read_bytes() for path in self.workspace.rglob('*') if path.is_file()})
        shutil.rmtree(self.workspace)
        api.verify_retained(self.output, report)
        first = self.output / report['files'][0]['path']
        first.write_bytes(b'changed retained file')
        with self.assertRaisesRegex(ValueError, 'indexed bytes'):
            api.verify_retained(self.output, report)

    @unittest.skipUnless(shutil.which('cc') and shutil.which('ar'), 'Requires actual C compiler and archive tool')
    def test_original_compiled_probe_object_and_archive_are_retained(self):
        source = self.write('toolchain-source-receipt/probe/diagnostic.c', b'int main(void) { return 0; }\n')
        obj = self.workspace / 'toolchain/lib/gcc/fixture/crtfixture.o'
        obj.parent.mkdir(parents=True)
        archive = obj.parent / 'libgcc-diagnostic-fixture.a'
        program = source.with_suffix('')
        subprocess.run(['cc', '-c', str(source), '-o', str(obj)], check=True, capture_output=True)
        subprocess.run(['ar', 'crs', str(archive), str(obj)], check=True, capture_output=True)
        subprocess.run(['cc', str(source), '-o', str(program)], check=True, capture_output=True)
        subprocess.run([str(program)], check=True, capture_output=True)
        report = api.retain(self.workspace, self.output)
        self.assertTrue({str(obj), str(archive), str(program)} <= {item['originalPath'] for item in report['files']})
        subprocess.run([str(self.output / 'materials' / program.relative_to(self.workspace))], check=True)
        api.verify_retained(self.output, report)

    def test_missing_workspace_does_not_claim_build_success_or_overwrite(self):
        self.workspace.rmdir()
        report = api.retain(self.workspace, self.output)
        self.assertEqual(report['state'], 'workspace-absent')
        self.assertEqual(report['files'], [])
        self.assertNotIn('buildComplete', report)
        with self.assertRaisesRegex(ValueError, 'new diagnostic output'):
            api.retain(self.workspace, self.output)
        self.workspace.mkdir()
        with self.assertRaisesRegex(ValueError, 'separate'):
            api.retain(self.workspace, self.workspace / 'nested')

    def test_selected_links_are_rejected_and_source_links_are_not_traversed(self):
        self.write('build-info.json', b'original')
        outside = self.root / 'outside'; outside.mkdir()
        (self.workspace / 'sources').symlink_to(outside, target_is_directory=True)
        api.retain(self.workspace, self.output)
        self.assertFalse((self.output / 'materials/sources').exists())
        shutil.rmtree(self.output)
        linked = self.workspace / 'control'
        linked.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Linked diagnostic path'):
            api.retain(self.workspace, self.output)
        linked.unlink(); linked.mkdir()
        (linked / 'record.json').symlink_to(self.workspace / 'build-info.json')
        with self.assertRaisesRegex(ValueError, 'Linked diagnostic path'):
            api.retain(self.workspace, self.output)

    def test_static_selection_ignores_unrelated_links_but_rejects_archive_links(self):
        selected = self.write('toolchain/lib/gcc/fixture/libgcc.a', b'original fixture archive')
        outside = self.root / 'outside'; outside.mkdir()
        (outside / 'libgcc-hidden.a').write_bytes(b'must not traverse linked directory')
        (self.workspace / 'toolchain/lib/libunrelated.so').symlink_to(selected)
        (self.workspace / 'toolchain/lib/headers-alias').symlink_to(outside, target_is_directory=True)
        report = api.retain(self.workspace, self.output)
        self.assertEqual([item['originalPath'] for item in report['files']], [str(selected)])
        api.verify_retained(self.output, report)
        shutil.rmtree(self.output)
        (selected.parent / 'libgcc_eh.a').symlink_to(selected)
        with self.assertRaisesRegex(ValueError, 'Linked diagnostic path'):
            api.retain(self.workspace, self.output)

    def test_size_limits_and_concurrent_original_changes_fail_without_an_index(self):
        source = self.write('build-info.json', b'original')
        with mock.patch.object(api, 'MAX_FILE', 4), self.assertRaisesRegex(ValueError, 'per-file'):
            api.retain(self.workspace, self.output)
        self.write('source-plan-fragment.json', b'original')
        with mock.patch.object(api, 'MAX_TOTAL', 12), self.assertRaisesRegex(ValueError, 'total size'):
            api.retain(self.workspace, self.output)
        copy = api.copy_original
        def changing(source_path, target, expected):
            result = copy(source_path, target, expected)
            if source_path == source:
                source.write_bytes(b'replaced')
            return result
        with mock.patch.object(api, 'copy_original', changing), self.assertRaisesRegex(ValueError, 'changed before index'):
            api.retain(self.workspace, self.output)
        self.assertFalse((self.output / api.INDEX).exists())


if __name__ == '__main__':
    unittest.main()
