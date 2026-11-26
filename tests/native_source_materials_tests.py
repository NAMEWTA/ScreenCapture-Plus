#!/usr/bin/env python3
"""Actual Git conversion/source comparisons; no dependency build is simulated."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/collect-native-source-materials.py'
native = types.ModuleType('native_source_materials')
native.__file__ = str(SCRIPT)
exec(compile(SCRIPT.read_bytes(), str(SCRIPT), 'exec'), native.__dict__)


class NativeSourceMaterialsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='native-source-contract-')
        self.root = Path(self.temporary.name).resolve(strict=True)
        self.source = self.root / 'upstream'
        self.source.mkdir()
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'Native source contract')
        self.git('config', 'user.email', 'native-source@example.invalid')
        self.git('config', 'core.autocrlf', 'false')
        self.git('remote', 'add', 'origin', 'https://example.invalid/fixture.git')
        self.mixed = b'fixture header\noriginal quoted line\r\nfixture trailer\n'
        (self.source / 'mixed.txt').write_bytes(self.mixed)
        (self.source / 'source.c').write_bytes(b'int original_fixture;\n')
        for name in ('LICENSE.md', 'COPYING.LGPLv2.1', 'COPYING.GPLv3'):
            (self.source / name).write_bytes(b'Explicit contract fixture notice; not a real FFmpeg distribution.\n')
        self.git('add', '.')
        self.git('commit', '--quiet', '-m', 'Exact mixed-line-ending source fixture')
        self.commit = self.git('rev-parse', 'HEAD')
        self.git('config', 'core.autocrlf', 'true')
        self.git('config', 'core.filemode', 'false')
        self.package = {'repository': 'https://example.invalid/fixture.git', 'commit': self.commit, 'patches': []}

    def tearDown(self):
        self.temporary.cleanup()

    def git(self, *arguments):
        environment = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        environment.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT='0')
        return subprocess.check_output(['git', *arguments], cwd=self.source, env=environment,
                                       stderr=subprocess.PIPE).decode().strip()

    def collect(self, name):
        output, source_output = self.root / (name + '-notices'), self.root / (name + '-sources')
        output.mkdir(); source_output.mkdir()
        return native.archive_source('ffmpeg', self.source, self.package, output, source_output, {}), source_output

    def test_mixed_upstream_bytes_survive_normal_conversion_without_renormalization(self):
        before = native.source_inventory(self.source)
        controls = {name: (self.source / '.git' / name).read_bytes() for name in ('HEAD', 'index', 'config')}
        expected_tree = self.git('rev-parse', 'HEAD^{tree}')
        # Reproduce the former failure against a real, separate object/index
        # directory. The original worktree, index and refs remain untouched.
        owned = self.root / 'proof.git'
        native.git(['clone', '--bare', '--shared', '--no-hardlinks', '--no-tags', '--', str(self.source), str(owned)], cwd=self.root)
        index = owned / 'proof.index'
        native.git(['read-tree', self.commit], cwd=self.source, git_dir=owned, index=index)
        native.git(['add', '--renormalize', '--', '.'], cwd=self.source, git_dir=owned, index=index,
                   conversion={'core.autocrlf': 'true', 'core.filemode': 'false'})
        self.assertNotEqual(native.git(['write-tree'], cwd=self.source, git_dir=owned, index=index), expected_tree)
        result, source_output = self.collect('normal')
        self.assertEqual(result['patchedTree'], expected_tree)
        self.assertTrue(result['originalWorktreeAndIndexUnchanged'])
        inventory = json.loads((source_output / result['sourceInventory']['path']).read_text())
        mixed = next(item for item in inventory['archiveFiles'] if item['path'] == 'mixed.txt')
        self.assertEqual(mixed['sha256'], hashlib.sha256(self.mixed).hexdigest())
        self.assertEqual(native.source_inventory(self.source), before)
        self.assertEqual((self.source / 'mixed.txt').read_bytes(), self.mixed)
        self.assertEqual({name: (self.source / '.git' / name).read_bytes() for name in controls}, controls)

    def test_normal_conversion_still_rejects_changed_authored_bytes(self):
        (self.source / 'source.c').write_bytes(b'int changed_fixture;\n')
        before = native.source_inventory(self.source)
        index = (self.source / '.git/index').read_bytes()
        with self.assertRaisesRegex(ValueError, 'changes beyond the exact ordered locked patches'):
            self.collect('changed')
        self.assertEqual(native.source_inventory(self.source), before)
        self.assertEqual((self.source / '.git/index').read_bytes(), index)
        diagnostic = json.loads((self.root / 'changed-sources/families/ffmpeg/source-differences.json').read_text())
        self.assertFalse(diagnostic['sourceAccepted'])
        self.assertTrue(diagnostic['originalWorktreeAndIndexUnchanged'])
        self.assertEqual([item['path'] for item in diagnostic['differences']], ['source.c'])


if __name__ == '__main__':
    unittest.main()
