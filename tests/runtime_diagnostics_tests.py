#!/usr/bin/env python3
"""Real file/process retention contracts; these do not simulate a runtime build."""
import hashlib
import copy
import json
import os
from pathlib import Path
import runpy
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

    @unittest.skipUnless(all(shutil.which(name) for name in ('cc', 'ar', 'objcopy')),
                         'Requires actual compiler, archive and object tools')
    def test_original_final_runtime_and_library_bytes_are_copied_directly(self):
        # Genuine host compiler outputs exercise retention only. They are not
        # presented as a target toolchain build or as an AppImage runtime.
        source = self.write('runtime-source-receipt/probe/diagnostic.c', b'int main(void) { return 0; }\n')
        install, libraries = self.workspace / 'install', self.workspace / 'libraries/lib'
        install.mkdir(); libraries.mkdir(parents=True)
        obj, unstripped = install / 'runtime.o', install / 'runtime.unstripped'
        binary, debug = install / 'runtime-x86_64', install / 'runtime-x86_64.debug'
        archive = libraries / 'libdiagnostic-fixture.a'
        atomic = self.workspace / 'toolchain/x86_64-linux-musl/lib/libatomic.a'
        atomic.parent.mkdir(parents=True)
        subprocess.run(['cc', '-g', '-c', str(source), '-o', str(obj)], check=True, capture_output=True)
        subprocess.run(['ar', 'crs', str(archive), str(obj)], check=True, capture_output=True)
        subprocess.run(['ar', 'crs', str(atomic), str(obj)], check=True, capture_output=True)
        subprocess.run(['cc', '-g', str(obj), '-o', str(unstripped)], check=True, capture_output=True)
        subprocess.run(['objcopy', '--only-keep-debug', str(unstripped), str(debug)], check=True, capture_output=True)
        subprocess.run(['objcopy', '--strip-debug', str(unstripped), str(binary)], check=True, capture_output=True)
        subprocess.run([str(binary)], check=True, capture_output=True)
        notice = self.write('install/LICENSE.runtime-sources', b'Diagnostic fixture notice, not a target runtime build.\n')
        (libraries / 'unrelated.so').symlink_to(archive)
        expected = {str(path): path.read_bytes() for path in (obj, unstripped, binary, debug, archive, atomic, notice)}
        report = api.retain(self.workspace, self.output)
        indexed = {item['originalPath']: item for item in report['files']}
        self.assertTrue(set(expected) <= set(indexed))
        for original, data in expected.items():
            retained = self.output / indexed[original]['path']
            self.assertEqual(retained.read_bytes(), data)
            self.assertEqual(hashlib.sha256(data).hexdigest(), indexed[original]['sha256'])
        subprocess.run([str(self.output / 'materials/install/runtime-x86_64')], check=True, capture_output=True)
        api.verify_retained(self.output, report)
        shutil.rmtree(self.output)
        (libraries / 'liblinked.a').symlink_to(archive)
        with self.assertRaisesRegex(ValueError, 'Linked diagnostic path'):
            api.retain(self.workspace, self.output)

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

    @unittest.skipUnless(sys.platform == 'linux', 'The real AppImage packaging/extraction regression runs on Linux')
    def test_actual_packaging_probe_icon_links_survive_retention_and_relocation(self):
        configured = os.environ.get('HUHUCAPTURE_TEST_PACKAGING_TOOLS')
        tools = Path(configured) if configured else ROOT / '.deps/runtime-packaging-tools'
        if not configured and not tools.is_dir():
            self.skipTest('Restore fixed packaging tools or set HUHUCAPTURE_TEST_PACKAGING_TOOLS')
        lock = json.loads((ROOT / 'build/package/dependencies.lock.json').read_text())
        selected = {}
        for key in ('appimageRuntime', 'appimagetool'):
            pin = lock['tools'][key]
            path = tools / key / pin['name']
            self.assertEqual(path.stat().st_size, pin['size'])
            self.assertEqual(api.digest(path), pin['sha256'])
            selected[key] = path
        builder = runpy.run_path(str(ROOT / 'scripts/build-appimage-runtime.py'))
        recorder = builder['API']['ExecutionLog'](self.workspace / 'control', 'linux',
                                                   ROOT / 'scripts/build-appimage-runtime.py')
        # Use the real fixed official runtime/tool only to reproduce probe icon
        # creation, extraction and retention. This is not a source-runtime build.
        probe_path = builder['probe_runtime'](recorder, self.workspace, selected['appimageRuntime'],
                                               selected['appimagetool'], builder['clean_environment']())
        recorder.raw_record({'scope': 'Official-tool diagnostic regression, not a source-built runtime'},
                            {'probe': builder['descriptor'](probe_path)})
        original_payload = builder['bundle'].inventory(self.workspace / 'probe/AppDir')
        self.assertEqual(original_payload, builder['bundle'].inventory(self.workspace / 'probe/extract/squashfs-root'))
        for name in api.PROBE_ICON_LINKS:
            path = self.workspace / name
            self.assertTrue(path.is_symlink())
            self.assertEqual(os.readlink(path), api.PROBE_ICON_TARGET)
        report = api.retain(self.workspace, self.output)
        self.assertEqual(len(report['knownProbeLinks']), 2)
        self.assertNotIn('buildComplete', report)
        api.verify_retained(self.output, report)
        for base in ('probe/AppDir', 'probe/extract/squashfs-root'):
            prefix = 'materials/' + base + '/'
            retained_payload = [{'path':item['path'][len(prefix):], 'type':'file',
                'size':item['size'], 'sha256':item['sha256'], 'mode':0o755 if item['mode'] & 0o111 else 0o644}
                for item in report['files'] if item['path'].startswith(prefix)]
            relation = next(item for item in report['knownProbeLinks']
                            if item['link']['originalPath'] == str(self.workspace / base / '.DirIcon'))
            link = relation['link']
            retained_payload.append({'path':'.DirIcon', 'type':'symlink', 'target':link['target'],
                                     'size':len(os.fsencode(link['target'])), 'sha256':link['targetSha256'], 'mode':0o777})
            self.assertEqual(sorted(retained_payload, key=lambda item:item['path']), original_payload)
            self.assertFalse((self.output / prefix / '.DirIcon').exists())
            self.assertEqual((self.output / relation['targetPath']).read_bytes(),
                             (self.workspace / base / api.PROBE_ICON_TARGET).read_bytes())

        for key, changed_value in (('target', '../escape.svg'), ('originalPath', str(self.workspace / 'other/.DirIcon'))):
            changed = copy.deepcopy(report)
            changed['knownProbeLinks'][0]['link'][key] = changed_value
            with self.assertRaisesRegex(ValueError, 'relationship changed'):
                api.verify_retained(self.output, changed)
        changed = copy.deepcopy(report)
        changed['knownProbeLinks'][0]['targetFile']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'fixed original bytes'):
            api.verify_retained(self.output, changed)
        changed = copy.deepcopy(report)
        changed['knownProbeLinks'][0]['targetPath'] = 'materials/elsewhere.svg'
        with self.assertRaisesRegex(ValueError, 'fixed original bytes'):
            api.verify_retained(self.output, changed)

        relocated = self.root / 'relocated'
        shutil.copytree(self.output, relocated)
        shutil.rmtree(self.output)
        api.verify_retained(relocated, report)
        link = self.workspace / api.PROBE_ICON_LINKS[0]
        link.unlink(); link.symlink_to('../escape.svg')
        with self.assertRaisesRegex(ValueError, 'Unknown or escaped'):
            api.retain(self.workspace, self.output)
        self.assertFalse(self.output.exists())
        link.unlink(); link.symlink_to(api.PROBE_ICON_TARGET)
        target = link.parent / api.PROBE_ICON_TARGET; original_icon = target.read_bytes()
        target.write_bytes(original_icon.replace(b'#333', b'#444'))
        with self.assertRaisesRegex(ValueError, 'icon bytes changed'):
            api.retain(self.workspace, self.output)
        target.write_bytes(original_icon)
        other = link.with_name('unknown-icon-link'); link.rename(other)
        with self.assertRaisesRegex(ValueError, 'Linked diagnostic path'):
            api.retain(self.workspace, self.output)
        other.rename(link)
        real_copy = api.copy_original
        def changed_link(source, destination, expected):
            result = real_copy(source, destination, expected)
            if source == target:
                link.unlink(); link.symlink_to('../escape.svg')
            return result
        with mock.patch.object(api, 'copy_original', changed_link), self.assertRaisesRegex(ValueError, 'Unknown or escaped'):
            api.retain(self.workspace, self.output)
        self.assertFalse((self.output / api.INDEX).exists())
        shutil.rmtree(self.workspace)
        api.verify_retained(relocated, report)

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

    @unittest.skipUnless(shutil.which('cc'), 'Original configure requires a real C compiler')
    def test_actual_original_musl_configure_link_is_retained_without_following_other_links(self):
        configured = os.environ.get('HUHUCAPTURE_TEST_MUSL_SOURCE')
        original = Path(configured) if configured else ROOT / '.deps/appimage-runtime-source/sources/appimage-musl'
        if not configured and not original.is_dir():
            self.skipTest('Restore original musl sources or set HUHUCAPTURE_TEST_MUSL_SOURCE')
        self.assertTrue(original.is_dir())
        for name, expected in api.MUSL_FILES.items():
            data = (original / name).read_bytes()
            self.assertEqual(len(data), expected['size'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), expected['sha256'])
        source = self.workspace / api.MUSL_SOURCE
        shutil.copytree(original, source, symlinks=True)
        build = self.workspace / 'toolchain-build'
        obj = build / 'obj_musl'; obj.mkdir(parents=True)
        alias = build / 'src_musl'; alias.symlink_to(source, target_is_directory=True)
        arguments = ['../src_musl/configure', '--disable-shared',
                     '--prefix=' + str(self.workspace / 'toolchain/x86_64-linux-musl'), 'CC=cc']
        actual = subprocess.run(arguments, cwd=obj, capture_output=True, check=False)
        self.assertEqual(actual.returncode, 0, actual.stderr.decode(errors='replace'))
        self.write('control/logs/original-configure.stdout.log', actual.stdout)
        self.write('control/logs/original-configure.stderr.log', actual.stderr)
        link = obj / 'Makefile'
        self.assertEqual(os.readlink(link), '../src_musl/Makefile')
        report = api.retain(self.workspace, self.output)
        relation = report['knownSourceLinks'][0]
        self.assertEqual(relation['link']['target'], os.readlink(link))
        self.assertEqual(relation['sourceDirectoryLink']['target'], os.readlink(alias))
        self.assertEqual((self.output / relation['targetPath']).read_bytes(), (source / 'Makefile').read_bytes())
        self.assertFalse((self.output / 'materials' / api.MUSL_MAKEFILE_LINK).exists())
        api.verify_retained(self.output, report)
        relation['link']['target'] = '../../escape/Makefile'
        with self.assertRaisesRegex(ValueError, 'relationship changed'):
            api.verify_retained(self.output, report)
        shutil.rmtree(self.output)
        link.unlink(); link.symlink_to('../../escape/Makefile')
        with self.assertRaisesRegex(ValueError, 'Unknown or escaped'):
            api.retain(self.workspace, self.output)
        link.unlink(); link.symlink_to('../src_musl/Makefile')
        alias.unlink(); alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'Unknown or escaped'):
            api.retain(self.workspace, self.output)
        alias.unlink(); alias.symlink_to(source, target_is_directory=True)
        (source / 'Makefile').write_bytes(b'changed target bytes')
        with self.assertRaisesRegex(ValueError, 'source bytes changed'):
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
