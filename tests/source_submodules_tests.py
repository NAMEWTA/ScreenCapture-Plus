#!/usr/bin/env python3
"""Real local Git graphs exercise Qt gitlink restoration, never a Qt build."""
import copy
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SUBMODULES = runpy.run_path(str(ROOT / 'build/package/source_submodules.py'))
CHECKOUT, BUNDLE = SUBMODULES['CHECKOUT'], SUBMODULES['BUNDLE']
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
ORIGINS = ('https://github.com/qt/qttools.git', 'https://code.qt.io/playground/qlitehtml.git',
           'https://code.qt.io/qt/qttools-litehtml.git')


def git(directory, *arguments, data=None):
    return subprocess.run(['git', '-c', 'core.hooksPath=' + os.devnull, '-C', str(directory), *arguments],
                          input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=BUNDLE['git_environment'](), check=True).stdout


class FixtureRecorder(API['ExecutionLog']):
    """Replace only transport in the fixture; run and retain actual Git commands."""
    def __init__(self, directory, upstreams):
        super().__init__(directory, 'windows' if os.name == 'nt' else 'linux', __file__)
        self.upstreams = upstreams

    def run(self, arguments, *, cwd, env=None, capture=False):
        arguments = list(arguments)
        if 'fetch' in arguments:
            self.original_fetch = arguments.copy()
            origin = git(cwd, 'remote', 'get-url', 'origin').decode().strip()
            arguments[arguments.index('origin')] = str(self.upstreams[origin])
            arguments[arguments.index('protocol.https.allow=always')] = 'protocol.file.allow=always'
        return super().run(arguments, cwd=cwd, env=env, capture=capture)


class SourceSubmoduleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='original nested Git sources ')
        self.root = Path(self.temporary.name).resolve()
        self.upstreams, self.commits = {}, {}
        leaf = {'.gitattributes': b'*.cmd text eol=crlf\n', 'original.cmd': b'original LF\n',
                'original-crlf.cmd': b'original CRLF\r\n', 'LICENSE': b'Fixture license\n'}
        self.make_upstream(ORIGINS[2], leaf)
        self.make_upstream(ORIGINS[1], {'LICENSE': b'Fixture license\n'},
                           [('nested/litehtml', ORIGINS[2], self.commits[ORIGINS[2]])])
        self.make_upstream(ORIGINS[0], {'LICENSE': b'Fixture license\n'},
                           [('src/assistant/qlitehtml', ORIGINS[1], self.commits[ORIGINS[1]])])
        self.pin = {'repository': ORIGINS[0], 'commit': self.commits[ORIGINS[0]]}

    def tearDown(self):
        self.temporary.cleanup()

    def make_upstream(self, origin, files, children=()):
        directory = self.root / ('upstream-' + str(len(self.upstreams)))
        directory.mkdir()
        git(directory, 'init', '--quiet')
        git(directory, 'config', 'user.name', 'Original nested source fixture')
        git(directory, 'config', 'user.email', 'source-submodule@example.invalid')
        files = dict(files)
        if children:
            files['.gitmodules'] = ''.join('[submodule "' + path + '"]\n\tpath = ' + path + '\n\turl = ' + url + '\n'
                                          for path, url, _ in children).encode()
        for name, data in files.items():
            sha = git(directory, 'hash-object', '-w', '--stdin', data=data).decode().strip()
            git(directory, 'update-index', '--add', '--cacheinfo', '100644', sha, name)
        for path, _, commit in children:
            git(directory, 'update-index', '--add', '--cacheinfo', '160000', commit, path)
        tree = git(directory, 'write-tree').decode().strip()
        commit = git(directory, 'commit-tree', tree, '-m', 'Actual original fixture Git graph').decode().strip()
        git(directory, 'update-ref', 'refs/heads/main', commit)
        self.upstreams[origin], self.commits[origin] = directory, commit

    def checkout(self, name='source'):
        source = self.root / name
        source.mkdir()
        git(source, 'init', '--quiet')
        git(source, 'config', 'core.autocrlf', 'false')
        CHECKOUT['install_policy'](source)
        git(source, 'remote', 'add', 'origin', ORIGINS[0])
        git(source, '-c', 'protocol.file.allow=always', 'fetch', '--quiet', str(self.upstreams[ORIGINS[0]]),
            self.pin['commit'])
        git(source, 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
        return source

    def restore(self, source):
        recorder = FixtureRecorder(self.root / (source.name + '-execution'), self.upstreams)
        value = SUBMODULES['restore'](source, self.pin, recorder, BUNDLE['git_environment']())
        return value, recorder

    def test_actual_nested_fixed_commits_export_with_original_crlf_and_retained_provenance(self):
        source = self.checkout()
        with self.assertRaisesRegex(ValueError, 'own Git checkout'):
            BUNDLE['export_git'](source, self.pin['commit'], self.root / 'before-export')
        original_index = (source / '.git/index').read_bytes()
        original_modules = (source / '.gitmodules').read_bytes()
        result, recorder = self.restore(source)
        first = result['submodules'][0]
        second = first['submodules'][0]
        self.assertEqual(first['commit'], self.commits[ORIGINS[1]])
        self.assertEqual(second['commit'], self.commits[ORIGINS[2]])
        nested = source / first['path'] / second['path']
        self.assertEqual((nested / 'original.cmd').read_bytes(), b'original LF\n')
        self.assertEqual((nested / 'original-crlf.cmd').read_bytes(), b'original CRLF\r\n')
        actual, files = BUNDLE['export_git'](source, self.pin['commit'], self.root / 'complete-export')
        self.assertEqual(actual['submodules'][0]['submodules'][0]['commit'], self.commits[ORIGINS[2]])
        self.assertTrue(any(item['path'].endswith('/original-crlf.cmd') for item in files))
        self.assertEqual((source / '.git/index').read_bytes(), original_index)
        self.assertEqual((source / '.gitmodules').read_bytes(), original_modules)
        self.assertEqual(git(source, 'status', '--porcelain'), b'')
        SUBMODULES['verify'](source, self.pin)
        retained = [Path(item['path']).read_bytes() for item in recorder.records]
        self.assertIn(original_modules, retained)
        self.assertEqual(retained.count(CHECKOUT['ATTRIBUTES']), 2)
        self.assertIn((recorder.directory / 'source-submodules.json').read_bytes(), retained)
        fetch = recorder.original_fetch
        self.assertIn('protocol.allow=never', fetch)
        self.assertIn('protocol.https.allow=always', fetch)
        self.assertEqual(fetch[-2:], ['origin', self.commits[ORIGINS[2]]])
        CHECKOUT['verify_policy'](nested)
        (nested / '.git/info/attributes').write_bytes(b'* text eol=crlf\n')
        with self.assertRaisesRegex(ValueError, 'policy is missing or changed|not clean'):
            SUBMODULES['verify'](source, self.pin)

    def test_changed_gitmodules_or_parent_pin_cannot_supply_another_child(self):
        source = self.checkout()
        pin = copy.deepcopy(self.pin)
        pin['commit'] = '1' * 40
        with self.assertRaisesRegex(ValueError, 'pinned commit'):
            SUBMODULES['children'](source, pin)
        original = (source / '.gitmodules').read_bytes()
        (source / '.gitmodules').write_bytes(original.replace(ORIGINS[1].encode(), b'https://unreviewed.invalid/repo'))
        with self.assertRaisesRegex(ValueError, 'not clean'):
            self.restore(source)

    def test_unreviewed_original_url_is_rejected_before_any_child_fetch(self):
        self.make_upstream(ORIGINS[0], {'LICENSE': b'Fixture license\n'},
                           [('src/assistant/qlitehtml', 'https://unreviewed.invalid/repo.git', self.commits[ORIGINS[1]])])
        self.pin['commit'] = self.commits[ORIGINS[0]]
        source = self.checkout()
        with self.assertRaisesRegex(ValueError, 'Unreviewed official submodule repository'):
            self.restore(source)
        self.assertFalse((source / 'src/assistant/qlitehtml/.git').exists())

    def test_relative_official_url_uses_git_directory_resolution_and_branch_is_only_metadata(self):
        files = {'.gitmodules': b'[submodule "nested/litehtml"]\n'
                 b' path = nested/litehtml\n url = ../../qt/qttools-litehtml.git\n branch = moving-branch\n'}
        upstream = self.upstreams[ORIGINS[1]]
        blob = git(upstream, 'hash-object', '-w', '--stdin', data=files['.gitmodules']).decode().strip()
        git(upstream, 'update-index', '--add', '--cacheinfo', '100644', blob, '.gitmodules')
        tree = git(upstream, 'write-tree').decode().strip()
        child_commit = git(upstream, 'commit-tree', tree, '-m', 'Original relative URL fixture').decode().strip()
        self.commits[ORIGINS[1]] = child_commit
        self.make_upstream(ORIGINS[0], {'LICENSE': b'Fixture license\n'},
                           [('src/assistant/qlitehtml', ORIGINS[1], child_commit)])
        self.pin['commit'] = self.commits[ORIGINS[0]]
        source = self.checkout()
        result, recorder = self.restore(source)
        child = result['submodules'][0]['submodules'][0]
        self.assertEqual(child['declaredUrl'], '../../qt/qttools-litehtml.git')
        self.assertEqual(child['repository'], ORIGINS[2])
        self.assertEqual(child['declaredBranch'], 'moving-branch')
        self.assertEqual(recorder.original_fetch[-1], self.commits[ORIGINS[2]])
        self.assertNotIn('moving-branch', recorder.original_fetch)
        BUNDLE['export_git'](source, self.pin['commit'], self.root / 'relative-export')

    def test_existing_nested_checkout_or_other_input_is_not_overwritten(self):
        source = self.checkout()
        path = source / 'src/assistant/qlitehtml/earlier.txt'
        path.write_bytes(b'preserve earlier input\n')
        with self.assertRaisesRegex(ValueError, 'empty directories'):
            self.restore(source)
        self.assertEqual(path.read_bytes(), b'preserve earlier input\n')
        self.assertFalse((path.parent / '.git').exists())

    def test_changed_child_commit_is_rejected_even_when_its_files_are_unchanged(self):
        source = self.checkout()
        self.restore(source)
        child = source / 'src/assistant/qlitehtml'
        git(child, 'config', 'user.name', 'Source fixture change')
        git(child, 'config', 'user.email', 'source-submodule@example.invalid')
        tree = git(child, 'rev-parse', 'HEAD^{tree}').decode().strip()
        other = git(child, 'commit-tree', tree, '-m', 'Same files under another commit').decode().strip()
        git(child, 'checkout', '--quiet', '--detach', other)
        with self.assertRaisesRegex(ValueError, 'not clean|pinned commit'):
            SUBMODULES['verify'](source, self.pin)


if __name__ == '__main__':
    unittest.main()
