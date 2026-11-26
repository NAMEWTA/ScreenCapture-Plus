#!/usr/bin/env python3
"""Actual producer command/source/output contracts; no product build claim."""
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
receipts = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
bundle = receipts['bundle']


class SourceReceiptsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='actual-build-receipt-')
        self.root = Path(self.temporary.name).resolve(strict=True)
        self.source = self.root / 'source'
        self.source.mkdir()
        for args in (('init', '--quiet'), ('config', 'user.name', 'Source receipt contract'),
                     ('config', 'user.email', 'source-receipt@example.invalid'), ('config', 'core.autocrlf', 'false')):
            bundle.git(self.source, *args)
        (self.source / 'source.c').write_bytes(b'#include <stdio.h>\nint main(void) { puts("actual receipt fixture"); return 0; }\n')
        (self.source / 'LICENSE').write_bytes(b'Contract fixture notice\n')
        bundle.git(self.source, 'add', '.')
        bundle.git(self.source, 'commit', '--quiet', '-m', 'Actual producer source fixture')
        self.pin = {'repository': 'https://example.invalid/fixture.git',
                    'commit': bundle.git(self.source, 'rev-parse', 'HEAD').decode().strip()}
        self.prefix = self.root / 'prefix'
        self.prefix.mkdir()
        (self.prefix / 'previous-producer.txt').write_bytes(b'Existing prefix data is not this build output\n')

    def tearDown(self):
        self.temporary.cleanup()

    def producer(self, name='receipt'):
        return receipts['NativeBuild'](self.root / name, 'linux' if os.name != 'nt' else 'windows',
                                       __file__, 'fixture', self.source, self.pin, self.prefix)

    @unittest.skipIf(os.name == 'nt', 'Actual cc/ELF compilation is a Linux producer gate')
    def test_real_compile_binds_checked_source_actual_commands_and_only_new_outputs(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.fail('The actual producer output test requires the host C compiler')
        producer = self.producer()
        executable = self.prefix / 'probe'
        command = [compiler, str(self.source / 'source.c'), '-o', str(executable)]
        producer.run(command, cwd=self.root)
        stdout, stderr = producer.run([str(executable)], cwd=self.root, capture=True)
        self.assertEqual(stdout.strip(), 'actual receipt fixture')
        self.assertEqual(stderr, '')
        plan = producer.finish()
        receipt = bundle.read_json(Path(plan['path']))
        self.assertEqual(receipt['commands'][0], command)
        self.assertEqual([item['path'] for item in receipt['outputs']], ['probe'])
        self.assertEqual(receipt['outputs'][0]['sha256'], bundle.digest(executable))
        execution = bundle.read_json(producer.directory / 'build-execution.json')
        self.assertEqual([item['exitCode'] for item in execution['executions']], [0, 0])
        self.assertEqual(execution['inputs']['sources'][0]['sourceTreeSha256'], producer.source_hash)
        # Pass the actual produced receipt through the independent consumer.
        stage = self.root / 'collector'; stage.mkdir()
        collector = bundle.Collector({'buildReceipts': [plan]}, self.root, stage)
        collector.platform = 'linux'; collector.lock = {'qtMigration': {}}
        collector.components = {'fixture': {'sourceTreeSha256': producer.source_hash}}
        collector.collect_receipts()
        self.assertEqual(collector.gaps, [])
        self.assertEqual(collector.receipts['fixture']['outputs'], receipt['outputs'])
        native = runpy.run_path(str(ROOT / 'scripts/collect-native-source-materials.py'))
        native_output = self.root / 'native-materials'; native_output.mkdir()
        # Same real source identities expressed in the native archive collector's
        # format, which has already checked each archive blob at this boundary.
        original = [{'path': item['path'], 'mode': '100755' if item['mode'] & 0o111 else '100644',
                     'size': item['size'], 'sha256': item['sha256']} for item in producer.files]
        (native_output / 'source-inputs.json').write_bytes(bundle.canonical({'archiveFiles': original}))
        source_record = {'sourceInventory': {'path': 'source-inputs.json'}}
        runtime = [{'path': 'probe', 'originalPath': str(executable), **receipts['identity'](executable)}]
        bound = native['producer_receipt']('fixture', plan, self.root, self.prefix, source_record, runtime, native_output, {})
        self.assertEqual(bound['runtimeOutputsVerified'], 1)
        self.assertEqual(bound['sourceTreeSha256'], producer.source_hash)
        changed_output = self.root / 'changed-native-materials'; changed_output.mkdir()
        shutil.copyfile(native_output / 'source-inputs.json', changed_output / 'source-inputs.json')
        with executable.open('ab') as stream:
            stream.write(b'changed after producer')
        runtime[0].update(receipts['identity'](executable))
        with self.assertRaisesRegex(ValueError, 'actual producer-time output identity'):
            native['producer_receipt']('fixture', plan, self.root, self.prefix, source_record, runtime, changed_output, {})

    def test_actual_failed_command_keeps_exit_and_logs_without_successful_receipt(self):
        producer = self.producer()
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            producer.run([sys.executable, '-c', 'import sys; print("actual failure fixture"); sys.exit(23)'], cwd=self.root)
        self.assertEqual(failure.exception.returncode, 23)
        record = bundle.read_json(producer.directory / 'execution-0.json')
        self.assertEqual(record['exitCode'], 23)
        self.assertIn('actual failure fixture', Path(record['stdout']).read_text())
        with self.assertRaisesRegex(ValueError, 'Unsuccessful commands'):
            producer.raw_record({}, {})
        self.assertFalse((producer.directory / 'source-build-receipt.json').exists())

    def test_changed_original_source_cannot_receive_a_build_receipt(self):
        producer = self.producer()
        producer.run([sys.executable, '-c', 'from pathlib import Path; Path("made.txt").write_bytes(b"actual output")'], cwd=self.prefix)
        (self.source / 'source.c').write_bytes(b'int changed_after_build;\n')
        with self.assertRaisesRegex(ValueError, 'unrecorded source change'):
            producer.finish()
        self.assertFalse((producer.directory / 'source-build-receipt.json').exists())

    @unittest.skipIf(os.name == 'nt', 'The controlled POSIX shell contract runs on the Linux host')
    def test_controlled_script_binds_only_successful_unchanged_executed_arguments(self):
        shell = shutil.which('sh')
        if not shell:
            self.fail('The real controlled-script contract requires a POSIX shell')
        producer = self.producer()
        commands = [[sys.executable, '-c', 'from pathlib import Path; Path("output with space.txt").write_bytes(b"actual shell output")']]
        script = self.root / 'fixed script.sh'
        expected = 'set -eu\ncd ' + shlex.quote(str(self.prefix)) + '\n' + shlex.join(commands[0]) + '\n'
        script.write_text(expected, encoding='utf-8', newline='\n')
        indices = producer.run_fixed_script([shell, str(script)], script, commands,
                    build_directory=self.root, posix_directory=str(self.prefix), env=os.environ)
        plan = producer.finish()
        receipt = bundle.read_json(Path(plan['path']))
        self.assertEqual(receipt['commands'][indices[0]], commands[0])
        self.assertEqual(receipt['commandOrigins'][str(indices[0])]['line'], 3)
        self.assertEqual((self.prefix / 'output with space.txt').read_bytes(), b'actual shell output')
        other = self.producer('invalid-script')
        with script.open('a') as stream:
            stream.write('printf unrecorded\n')
        with self.assertRaisesRegex(ValueError, 'script differs'):
            other.run_fixed_script([shell, str(script)], script, commands,
                        build_directory=self.root, posix_directory=str(self.prefix), env=os.environ)
        self.assertEqual(other.executions, [])


if __name__ == '__main__':
    unittest.main()
