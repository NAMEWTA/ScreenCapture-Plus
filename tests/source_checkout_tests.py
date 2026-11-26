#!/usr/bin/env python3
"""Real Git fixtures for original source bytes; no Qt build success is claimed."""
import hashlib
import io
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = runpy.run_path(str(ROOT / 'build/package/source_checkout.py'))
BUNDLE = CHECKOUT['COLLECTOR']
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))


def git(directory, *arguments, data=None):
    result = subprocess.run(['git', '-c', 'core.hooksPath='+os.devnull, '-C', str(directory), *arguments],
                            input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=BUNDLE['git_environment'](), check=True)
    return result.stdout


class OriginalSourceCheckoutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='original Git bytes ')
        self.root = Path(self.temporary.name).resolve()
        self.upstream = self.root/'upstream';self.upstream.mkdir()
        git(self.upstream,'init','--quiet')
        git(self.upstream,'config','user.name','Original source fixture')
        git(self.upstream,'config','user.email','source-checkout@example.invalid')
        # Construct actual original Git blobs directly, so the fixture itself
        # does not accidentally normalize the CRLF input when it is committed.
        self.files = {
            '.gitattributes': b'*.bat.in text eol=crlf\n*.cmd text eol=crlf\n*.lf text eol=lf\n',
            'bin/qmake-and-qtpaths-wrapper.bat.in': b'@echo off\necho original LF\n',
            'vendor/double-conversion/upstream/msvc/testrunner.cmd': b'@echo off\necho original LF\n',
            'original-crlf.cmd': b'@echo off\r\necho original CRLF\r\n',
            'source.lf': b'original LF input\n',
            'source.c': b'int actual_source_fixture = 1;\n',
            'LICENSE': b'Original source fixture notice\n'}
        for name, data in self.files.items():
            sha = git(self.upstream,'hash-object','-w','--stdin',data=data).decode().strip()
            git(self.upstream,'update-index','--add','--cacheinfo','100644',sha,name)
        tree = git(self.upstream,'write-tree').decode().strip()
        self.commit = git(self.upstream,'commit-tree',tree,'-m','Original LF and CRLF source fixture').decode().strip()
        git(self.upstream,'update-ref','refs/heads/main',self.commit)

    def tearDown(self):
        self.temporary.cleanup()

    def repository(self, name):
        source = self.root/name;source.mkdir()
        git(source,'init','--quiet')
        git(source,'config','core.autocrlf','false')
        return source

    def checkout(self, name, raw):
        source = self.repository(name)
        if raw: CHECKOUT['install_policy'](source)
        git(source,'-c','protocol.file.allow=always','fetch','--quiet',str(self.upstream),self.commit)
        git(source,'checkout','--quiet','--detach','FETCH_HEAD')
        return source

    def test_false_autocrlf_does_not_prevent_upstream_crlf_conversion_and_strict_collector_rejects(self):
        source = self.checkout('ordinary',False)
        for name in ('bin/qmake-and-qtpaths-wrapper.bat.in',
                     'vendor/double-conversion/upstream/msvc/testrunner.cmd'):
            self.assertEqual((source/name).read_bytes(),self.files[name].replace(b'\n',b'\r\n'))
            self.assertNotEqual((source/name).read_bytes(),self.files[name])
        with self.assertRaisesRegex(ValueError,'unrecorded source change'):
            BUNDLE['export_git'](source,self.commit,self.root/'ordinary-export')
        with self.assertRaisesRegex(ValueError,'new empty repository'):
            CHECKOUT['install_policy'](source)
        self.assertFalse((source/'.git/info/attributes').exists())

    def test_first_checkout_preserves_each_original_lf_crlf_blob_and_clean_index(self):
        source = self.checkout('raw',True)
        before_index = (source/'.git/index').read_bytes()
        before_refs = git(source,'show-ref','--head')
        original_attributes = git(source,'show',self.commit+':.gitattributes')
        archive = git(source,'archive','--format=tar',self.commit)
        with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
            for name,data in self.files.items():
                self.assertEqual((source/name).read_bytes(),data)
                self.assertEqual(stream.extractfile(name).read(),data)
        self.assertEqual((source/'.gitattributes').read_bytes(),original_attributes)
        self.assertEqual(git(source,'status','--porcelain','--untracked-files=normal'),b'')
        _,files = BUNDLE['export_git'](source,self.commit,self.root/'raw-export')
        self.assertEqual({item['path']:item['sha256'] for item in files},
                         {name:hashlib.sha256(data).hexdigest() for name,data in self.files.items()})
        self.assertEqual((source/'.git/index').read_bytes(),before_index)
        self.assertEqual(git(source,'show-ref','--head'),before_refs)
        self.assertEqual(CHECKOUT['verify_policy'](source)['profile'],'original-git-blob-bytes-v1')

    def test_changed_source_or_checkout_policy_still_fails(self):
        source = self.checkout('changed',True)
        _,files = BUNDLE['export_git'](source,self.commit,self.root/'export')
        changed = source/'bin/qmake-and-qtpaths-wrapper.bat.in'
        changed.write_bytes(changed.read_bytes()+b'changed\n')
        with self.assertRaisesRegex(ValueError,'unrecorded source change'):
            BUNDLE['compare_checkout'](source,files,self.commit)
        changed.write_bytes(self.files['bin/qmake-and-qtpaths-wrapper.bat.in'])
        policy = source/'.git/info/attributes'
        policy.write_bytes(CHECKOUT['ATTRIBUTES']+b'# a changed build profile\n')
        with self.assertRaisesRegex(ValueError,'policy is missing or changed'):
            CHECKOUT['verify_policy'](source)
        policy.write_bytes(CHECKOUT['ATTRIBUTES'])
        git(source,'config','core.autocrlf','true')
        with self.assertRaisesRegex(ValueError,'configuration changed'):
            CHECKOUT['verify_policy'](source)

    def test_ident_encoding_and_configured_smudge_filter_are_disabled_by_the_same_policy(self):
        source = self.repository('transform attributes')
        CHECKOUT['install_policy'](source)
        driver = shlex.quote(Path(sys.executable).as_posix())+' -c '+shlex.quote(
            'import sys; sys.stdout.buffer.write(b"changed by smudge")')
        git(source,'config','filter.fixture.smudge',driver)
        git(source,'config','filter.fixture.required','true')
        files = {'.gitattributes': b'ident.txt ident\nencoded.txt text working-tree-encoding=UTF-16LE\nfiltered.txt filter=fixture\n',
                 'ident.txt': b'$Id$\n', 'encoded.txt': 'original UTF-8 é\n'.encode(),
                 'filtered.txt': b'original filter input\n'}
        for name,data in files.items():
            sha=git(self.upstream,'hash-object','-w','--stdin',data=data).decode().strip()
            git(self.upstream,'update-index','--add','--cacheinfo','100644',sha,name)
        tree=git(self.upstream,'write-tree').decode().strip()
        commit=git(self.upstream,'commit-tree',tree,'-m','Original conversion-attribute fixture').decode().strip()
        git(source,'-c','protocol.file.allow=always','fetch','--quiet',str(self.upstream),commit)
        git(source,'checkout','--quiet','--detach','FETCH_HEAD')
        for name,data in files.items(): self.assertEqual((source/name).read_bytes(),data)
        self.assertEqual(git(source,'status','--porcelain'),b'')
        BUNDLE['export_git'](source,commit,self.root/'attribute-export')

    def test_policy_and_helper_bytes_are_retained_in_real_execution_records(self):
        source=self.checkout('recorded',True)
        recorder=API['ExecutionLog'](self.root/'actual-execution','windows' if os.name=='nt' else 'linux',__file__)
        profile=CHECKOUT['retain_policy'](source,recorder)
        recorder.run([sys.executable,'-c','print("actual checkout policy evidence fixture")'],cwd=source)
        execution=recorder.raw_record({'sourceCheckoutProfile':profile},{})
        value=BUNDLE['read_json'](execution)
        self.assertEqual(value['inputs']['sourceCheckoutProfile'],profile)
        self.assertEqual(Path(profile['retainedPolicy']['path']).read_bytes(),CHECKOUT['ATTRIBUTES'])
        self.assertEqual(Path(profile['retainedHelper']['path']).read_bytes(),
                         (ROOT/'build/package/source_checkout.py').read_bytes())
        (source/'.git/info/attributes').write_bytes(b'* text eol=crlf\n')
        with self.assertRaisesRegex(ValueError,'policy is missing or changed'):
            CHECKOUT['retain_policy'](source,recorder)
        self.assertEqual(Path(profile['retainedPolicy']['path']).read_bytes(),CHECKOUT['ATTRIBUTES'])

    def test_existing_policy_or_nonempty_unborn_worktree_is_never_overwritten(self):
        source=self.repository('existing-policy')
        policy=source/'.git/info/attributes';policy.write_bytes(b'original policy\n')
        with self.assertRaisesRegex(ValueError,'cannot be replaced'):
            CHECKOUT['install_policy'](source)
        self.assertEqual(policy.read_bytes(),b'original policy\n')
        other=self.repository('existing-input')
        (other/'original.txt').write_bytes(b'existing source input\n')
        with self.assertRaisesRegex(ValueError,'new empty repository'):
            CHECKOUT['install_policy'](other)
        self.assertEqual((other/'original.txt').read_bytes(),b'existing source input\n')
        self.assertFalse((other/'.git/info/attributes').exists())


if __name__=='__main__':
    unittest.main()
