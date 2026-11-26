#!/usr/bin/env python3
"""Offline contracts with real local ar/ELF files, not a runtime-build claim."""
import hashlib
import copy
import io
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import tarfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
HELPER = runpy.run_path(str(ROOT / 'build/package/appimage_runtime.py'))
BUILDER = runpy.run_path(str(ROOT / 'scripts/build-appimage-runtime.py'))
BUNDLE = HELPER['bundle']
sys.path.insert(0, str(ROOT / 'scripts'))
PACKAGE = runpy.run_path(str(ROOT / 'scripts/package-qt.py'))
PATCHES = HELPER['PATCHES']
HEADERS = HELPER['HEADERS']


class ProfileTests(unittest.TestCase):
    def test_exact_header_path_profile_cannot_be_selected_for_other_inputs(self):
        _, _, profile = HELPER['profile']()
        pin=profile['sources'][HEADERS['COMPONENT']]
        self.assertEqual(HEADERS['source_profile'](HEADERS['COMPONENT'],pin,'linux'),HEADERS['SOURCE'])
        for component,altered,platform in (
                ('unrelated-source',pin,'linux'),(HEADERS['COMPONENT'],pin,'windows'),
                (HEADERS['COMPONENT'],{**pin,'sourcePathProfile':{'profile':'unknown'}},'linux'),
                (HEADERS['COMPONENT'],{**pin,'sourceArchive':{'sha256':'0'*64}},'linux')):
            with self.assertRaisesRegex(ValueError,'exact original Linux archive'):
                HEADERS['source_profile'](component,altered,platform)
        with mock.patch.object(sys,'platform','win32'):
            self.assertEqual(HEADERS['entries'](None),{})
            with self.assertRaisesRegex(ValueError,'Only the reviewed'):
                HEADERS['entries'](HEADERS['SOURCE'])

    def test_canonical_patch_profile_is_narrow_and_complete(self):
        _, _, profile = HELPER['profile']()
        for name in PATCHES['ARCHIVES']:
            pin = profile['sources'][name]
            PATCHES['validate_profile'](name,pin,ROOT)
            for change in ({'profile':'unknown'}, {'pristineTreeSha256':'0'},
                           {'patchedTreeSha256':'0'}):
                altered=copy.deepcopy(pin)
                altered['patchApplication'].update(change)
                with self.assertRaisesRegex(ValueError,'patch profile'):
                    PATCHES['validate_profile'](name,altered,ROOT)
            altered=copy.deepcopy(pin); altered['patches'].reverse()
            with self.assertRaisesRegex(ValueError,'patch set'):
                PATCHES['validate_profile'](name,altered,ROOT)
            altered=copy.deepcopy(pin); altered['sourceArchive']['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'exact source archive'):
                PATCHES['validate_profile'](name,altered,ROOT)
        with self.assertRaisesRegex(ValueError,'patch profile'):
            PATCHES['validate_profile']('unreviewed-source',pin,ROOT)

    def test_original_cowpatch_script_tampering_is_rejected(self):
        _, _, profile = HELPER['profile']()
        with tempfile.TemporaryDirectory() as value:
            project=Path(value)
            for path in ('third_party/appimage/toolchain-patches/upstream.json', PATCHES['RECIPE']['cowpatch']['path']):
                target=project/path; target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(ROOT/path,target)
            target.write_bytes(target.read_bytes()+b'\n# changed original script\n')
            with self.assertRaisesRegex(ValueError,'exact Git blob'):
                PATCHES['validate_profile']('appimage-musl',profile['sources']['appimage-musl'],project)

    def test_collection_plan_cannot_supply_a_patch_application_profile(self):
        with tempfile.TemporaryDirectory() as value:
            root=Path(value); collector=BUNDLE.Collector({},root,root)
            for key in ('patchApplication','sourcePathProfile','path_profile'):
                with self.assertRaisesRegex(ValueError,'cannot (override|choose)'):
                    collector.collect_source({'id':'appimage-musl',key:{'profile':'anything'}})

    def test_exact_static_toolchain_and_original_asset_pins_coexist(self):
        _, lock, profile = HELPER['profile']()
        self.assertEqual(lock['tools']['appimageRuntime']['sha256'],
                         '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d')
        self.assertEqual(set(profile['sources']), HELPER['SOURCE_IDS'])
        self.assertTrue(HELPER['STATIC_IDS'] <= set(profile['sources']))
        original = BUNDLE.read_json(ROOT / 'third_party/appimage/toolchain-patches/upstream.json')
        self.assertEqual(len(original['patches']), 26)
        for item in original['patches']:
            data = (ROOT / item['path']).read_bytes()
            self.assertEqual(BUNDLE.object_digest('blob', data), item['blob'])

    def test_real_build_system_source_is_not_misidentified_as_missing_c(self):
        _, _, profile = HELPER['profile']()
        pin = profile['sources']['appimage-toolchain-build']
        files = [{'path': path, 'type': 'file', 'size': 1}
                 for path in ('Makefile', 'litecross/Makefile', 'LICENSE', 'cowpatch.sh')]
        self.assertTrue(BUNDLE.recognizable_source('appimage-toolchain-build', pin, files))
        self.assertFalse(BUNDLE.recognizable_source('arbitrary-tool', pin, files))
        self.assertFalse(BUNDLE.recognizable_source('appimage-toolchain-build', {**pin, 'commit':'0'*40}, files))
        self.assertFalse(BUNDLE.recognizable_source('appimage-toolchain-build', pin, files[:-1]))

    def test_old_sdk_compiler_loader_and_make_environment_cannot_leak(self):
        hostile = {'PATH':'/old/qt/bin:/usr/bin', 'CC':'old-gcc', 'CXX':'old-g++', 'CMAKE_PREFIX_PATH':'old-qt',
                   'LD_LIBRARY_PATH':'old-sdk/lib', 'LIBRARY_PATH':'old/lib', 'CPATH':'old/include',
                   'PKG_CONFIG_LIBDIR':'old/pkgconfig', 'LD_PRELOAD':'old.so', 'MAKEFLAGS':'-e',
                   'GCC_EXEC_PREFIX':'old/gcc/', 'COMPILER_PATH':'old/bin', 'GIT_CONFIG_COUNT':'1',
                   'CC_FOR_BUILD':'old-compiler', 'LDFLAGS_FOR_TARGET':'old-flags', 'BASH_ENV':'old/script'}
        with mock.patch.dict(os.environ, hostile, clear=True):
            cleaned = BUILDER['clean_environment']()
            self.assertEqual(cleaned, {'PATH':os.defpath, 'LC_ALL':'C.UTF-8', 'CONFIG_SITE':os.devnull})

    @unittest.skipUnless(sys.platform=='linux', 'This native builder accepts Linux workspace paths')
    def test_refuse_reused_or_make_interpreted_workspaces(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            with self.assertRaisesRegex(ValueError, 'new separate'):
                BUILDER['new_path'](root)
            for name in ('space here', 'command;value', 'dollar$value', 'hash#value', "quote'value"):
                with self.assertRaisesRegex(ValueError, 'metacharacters'):
                    BUILDER['new_path'](root/name)
            self.assertEqual(BUILDER['new_path'](root/'actual-build'), root/'actual-build')

    def test_packager_does_not_accept_an_unbuilt_runtime_workspace(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            with self.assertRaises((ValueError, OSError)):
                PACKAGE['select_source_runtime'](root, root/'package', {}, object())
            self.assertFalse((root/'package').exists())


@unittest.skipUnless(sys.platform=='linux' and shutil.which('gcc') and shutil.which('ar'),
                     'Native Linux parser contracts require real gcc/ar')
class NativeFileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='runtime-evidence-tests-')
        self.root = Path(self.temporary.name)
        source = self.root/'fixture.c'
        source.write_text('const unsigned char runtime_digest[16] __attribute__((section(".digest_md5"),used))={0};\n'
                          'int fixture_value(void) { return 7; }\nint main(void) { return fixture_value()-7; }\n')
        self.object = self.root/'a_genuinely_long_object_member_name_for_gnu_ar.o'
        subprocess.run(['gcc','-fPIE','-c',source,'-o',self.object], check=True, capture_output=True)
        self.runtime = self.root/'fixture-runtime'
        self.map = self.root/'actual.map'
        subprocess.run(['gcc','-static-pie',self.object,'-Wl,-Map='+str(self.map),'-o',self.runtime],
                       check=True, capture_output=True)
        subprocess.run([self.runtime], check=True, capture_output=True)

    def tearDown(self):
        self.temporary.cleanup()

    def marked_runtime(self):
        with self.runtime.open('r+b') as output:
            output.seek(8); output.write(b'AI\x02')

    def test_actual_gnu_archive_long_names_and_member_bytes(self):
        archive = self.root/'actual.a'
        subprocess.run(['ar','rcs',archive,self.object], check=True, capture_output=True)
        members = HELPER['archive_members'](archive)
        self.assertEqual(members, [{'name':self.object.name, 'occurrence':1, 'size':self.object.stat().st_size,
                                   'sha256':hashlib.sha256(self.object.read_bytes()).hexdigest()}])
        data = archive.read_bytes()
        archive.write_bytes(data[:-3])
        with self.assertRaises(ValueError): HELPER['archive_members'](archive)
        thin = self.root/'thin.a'
        subprocess.run(['ar','rcsT',thin,self.object], check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError,'complete regular'): HELPER['archive_members'](thin)

    def test_real_link_map_lists_crt_and_compiler_archives_and_unowned_inputs_fail(self):
        loads = HELPER['link_loads'](self.map.read_text())
        self.assertIn(self.object, loads)
        self.assertIn('libgcc.a', {path.name for path in loads})
        self.assertIn('rcrt1.o', {path.name for path in loads})
        with self.assertRaisesRegex(ValueError,'no actual producer'):
            BUILDER['collect_link_inputs'](self.map, {}, {})
        for record in ('LOAD relative.a\n','LOAD /tmp/host.so\n','not a GNU link map\n'):
            with self.assertRaises(ValueError): HELPER['link_loads'](record)

    def test_elf_inspection_rejects_dynamic_host_fallback(self):
        HELPER['inspect_elf'](self.runtime, marked=False)
        with self.assertRaisesRegex(ValueError,'magic'): HELPER['inspect_elf'](self.runtime)
        self.marked_runtime()
        HELPER['inspect_elf'](self.runtime)
        dynamic = self.root/'dynamic'
        subprocess.run(['gcc', self.object,'-o',dynamic], check=True, capture_output=True)
        with self.assertRaisesRegex(ValueError,'dynamic interpreter'):
            HELPER['inspect_elf'](dynamic, marked=False)

    def test_embedding_permits_only_actual_elf_md5_field_and_checks_all_other_bytes(self):
        self.marked_runtime()
        image = self.root/'synthetic-appended-fixture'
        image.write_bytes(self.runtime.read_bytes()+b'fixture appended bytes, not an AppImage build claim')
        record = HELPER['runtime_embedding'](self.runtime,image)
        offset = record['changes'][0]['offset']
        with image.open('r+b') as output:
            output.seek(offset); output.write(bytes(range(16)))
        changed = HELPER['runtime_embedding'](self.runtime,image)
        self.assertNotEqual(record['embeddedRuntimeSha256'], changed['embeddedRuntimeSha256'])
        self.assertEqual(changed['changes'][0]['afterHex'], bytes(range(16)).hex())
        with image.open('r+b') as output:
            output.seek(offset-1); original=output.read(1); output.seek(offset-1); output.write(bytes([original[0]^1]))
        with self.assertRaisesRegex(ValueError,'outside its original'):
            HELPER['runtime_embedding'](self.runtime,image)

    def test_source_build_receipt_records_only_successful_actual_commands(self):
        source = self.root/'source'; source.mkdir()
        (source/'value.c').write_text('int value=1;\n')
        files=BUNDLE.inventory(source)
        manifest=self.root/'source.json'
        manifest.write_bytes(BUNDLE.canonical({'files':files}))
        item={'id':'contract-fixture', 'pin':{}, 'directory':source, 'files':files, 'sourceTreeSha256':BUNDLE.tree_digest(files),
              'manifest':BUILDER['descriptor'](manifest)}
        prefix=self.root/'install';prefix.mkdir()
        build=BUILDER['Build'](self.root/'receipt','contract-fixture',[item],prefix)
        build.run(['gcc','-c',source/'value.c','-o',prefix/'value.o'],cwd=prefix)
        _, report, receipt=build.finish([])
        self.assertEqual(receipt['staticInputs'], [])
        self.assertEqual(receipt['outputs'][0]['path'], 'value.o')
        HELPER['verify_execution'](report['execution'])
        failed=BUILDER['Build'](self.root/'failed-receipt','failed-fixture',[item],prefix)
        with self.assertRaises(subprocess.CalledProcessError):
            failed.run([sys.executable,'-c','raise SystemExit(7)'],cwd=prefix)
        with self.assertRaisesRegex(ValueError,'Unsuccessful'):
            failed.raw_record({}, {})
        self.assertFalse((self.root/'failed-receipt/source-build-receipt.json').exists())


@unittest.skipUnless(sys.platform=='linux', 'The exact original header profile is Linux-only')
class HeaderPathTests(unittest.TestCase):
    def test_eight_original_pairs_never_admit_extra_names_or_duplicate_entries(self):
        entries=HEADERS['entries'](HEADERS['SOURCE'])
        self.assertEqual(len(entries),16)
        self.assertEqual(len({name.casefold() for name in entries}),8)
        names={}
        for name in entries:
            BUNDLE.check_unique_path(name,names,entries)
        with self.assertRaisesRegex(ValueError,'Duplicate'):
            BUNDLE.check_unique_path(next(iter(entries)),names,entries)
        with self.assertRaisesRegex(ValueError,'case-colliding'):
            BUNDLE.check_unique_path('generic/include/linux/netfilter/XT_CONNMARK.h',names,entries)
        with self.assertRaisesRegex(ValueError,'case-colliding'):
            BUNDLE.check_unique_path('other.h',{'other.h':{'Other.h'}},entries)

    def test_source_and_sysroot_exceptions_require_exact_original_header_bytes(self):
        with tempfile.TemporaryDirectory() as value:
            root=Path(value)
            for index,profile in enumerate((HEADERS['SOURCE'],HEADERS['SYSROOT'],HEADERS['BUNDLE'])):
                name=next(iter(HEADERS['entries'](profile)))
                directory=root/str(index);path=directory/name;path.parent.mkdir(parents=True)
                path.write_bytes(b'An unrelated file cannot use an allowed header path.\n')
                with self.assertRaisesRegex(ValueError,'exact original file'):
                    BUNDLE.inventory(directory,path_profile=profile)
            directory=root/'ordinary';directory.mkdir()
            (directory/'Original.h').write_bytes(b'one');(directory/'original.h').write_bytes(b'two')
            for profile in (None,HEADERS['SOURCE']):
                with self.assertRaisesRegex(ValueError,'case-colliding'):
                    BUNDLE.inventory(directory,path_profile=profile)
            with self.assertRaisesRegex(ValueError,'Only the reviewed'):
                BUNDLE.inventory(directory,path_profile='arbitrary-case-sensitive-mode')

    def test_real_archives_reject_duplicates_links_and_wrong_original_bytes(self):
        with tempfile.TemporaryDirectory() as value:
            root=Path(value);name=next(iter(HEADERS['CASE_FILES']))
            for kind in ('duplicate','symlink','changed-bytes'):
                archive=root/(kind+'.tar')
                with tarfile.open(archive,'w') as output:
                    entry=tarfile.TarInfo(name)
                    if kind=='symlink':
                        entry.type=tarfile.SYMTYPE;entry.linkname='other.h';output.addfile(entry)
                    else:
                        data=b'changed original header bytes';entry.size=len(data)
                        output.addfile(entry,io.BytesIO(data))
                        if kind=='duplicate':output.addfile(entry,io.BytesIO(data))
                with self.assertRaises(ValueError):
                    BUNDLE.extract_tar(archive,root/kind,path_profile=HEADERS['SOURCE'])


@unittest.skipUnless(sys.platform=='linux' and shutil.which('patch') and shutil.which('sh'),
                     'Canonical patch replay requires the real native GNU patch and POSIX shell')
class CanonicalPatchReplayTests(unittest.TestCase):
    """Small execution fixtures; the complete original archives are checked separately."""
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='canonical-patch-tests-')
        self.root=Path(self.temporary.name)
        self.source=self.root/'source'; self.source.mkdir()
        before=b'actual-one\nactual-two\nkeep-before\nint value = 1;\nkeep-after\nactual-three\nactual-four\n'
        (self.source/'source.c').write_bytes(before)
        (self.source/'LICENSE').write_bytes(b'Actual patch execution fixture.\n')
        self.patch=self.root/'original.patch'
        self.patch.write_bytes(b'--- a/source.c\n+++ b/source.c\n@@ -1,7 +1,7 @@\n'
                               b' expected-one\n expected-two\n keep-before\n-int value = 1;\n+int value = 2;\n'
                               b' keep-after\n expected-three\n expected-four\n')
        expected=self.root/'expected'; shutil.copytree(self.source,expected)
        (expected/'source.c').write_bytes(before.replace(b'value = 1',b'value = 2'))
        (expected/'source.c.orig').write_bytes(before)
        self.expected=BUNDLE.inventory(expected)
        self.profile={'profile':PATCHES['PROFILE'],
                      'pristineTreeSha256':BUNDLE.tree_digest(BUNDLE.inventory(self.source)),
                      'patchedTreeSha256':BUNDLE.tree_digest(self.expected)}
        self.pin={'patches':[{'path':'original.patch','sha256':BUNDLE.digest(self.patch)}]}
        self.originals=[{'path':'original.patch','blob':BUNDLE.object_digest('blob',self.patch.read_bytes())}]
        self.recorder=HELPER['API']['ExecutionLog'](self.root/'execution','linux',Path(__file__))

    def tearDown(self):
        self.temporary.cleanup()

    def fixture_profile(self):
        # Production validation above admits only three exact upstream archives.
        # This fixture changes that boundary only inside this test's mock scope;
        # the real command, complete output checks and retention remain active.
        return mock.patch.dict(PATCHES['apply'].__globals__,{'validate_profile':lambda *args:
            (self.profile,self.originals,ROOT/PATCHES['RECIPE']['cowpatch']['path'])})

    def replay(self):
        return PATCHES['apply']('native-patch-fixture',self.source,[self.patch],self.pin,self.root,
                                BUNDLE.__dict__,recorder=self.recorder)

    def test_actual_fuzz_two_recipe_retains_backups_and_original_execution(self):
        with self.assertRaisesRegex(ValueError,'patch does not apply'):
            BUNDLE.apply_patches(self.source,[self.patch])
        self.assertFalse((self.source/'.git').exists())
        with self.fixture_profile():
            result=self.replay()
            self.assertEqual(BUNDLE.inventory(self.source),self.expected)
            self.assertIn('fuzz 2',result['executions'][1]['stdout'])
            self.assertEqual([item['path'] for item in result['backupFiles']],['source.c.orig'])
            execution=BUNDLE.read_json(self.recorder.raw_record({},{}))
            PATCHES['verify_replay']('native-patch-fixture',self.pin,self.root,result,self.expected,
                                     BUNDLE.__dict__,execution,self.source)
            changed=copy.deepcopy(result); changed['executions'][1]['stdout']='invented success'
            with self.assertRaisesRegex(ValueError,'actual original command'):
                PATCHES['verify_replay']('native-patch-fixture',self.pin,self.root,changed,self.expected,
                                         BUNDLE.__dict__,execution,self.source)
            (self.source/'source.c.orig').unlink()
            with self.assertRaisesRegex(ValueError,'complete source output'):
                PATCHES['verify_replay']('native-patch-fixture',self.pin,self.root,result,
                                         BUNDLE.inventory(self.source),BUNDLE.__dict__,execution,self.source)

    def test_changed_original_bytes_and_incomplete_post_tree_are_rejected(self):
        with self.fixture_profile():
            original=self.patch.read_bytes(); self.patch.write_bytes(original+b'\n')
            with self.assertRaisesRegex(ValueError,'patch byte changed'):
                self.replay()
            self.patch.write_bytes(original)
            (self.source/'unrecorded.c').write_bytes(b'not an original input\n')
            with self.assertRaisesRegex(ValueError,'before patching'):
                self.replay()
            (self.source/'unrecorded.c').unlink()
            self.assertEqual(self.recorder.executions,[])
            self.profile['patchedTreeSha256']=BUNDLE.tree_digest(
                [item for item in self.expected if item['path']!='source.c.orig'])
            with self.assertRaisesRegex(ValueError,'including copy-on-write/backup'):
                self.replay()

    def test_actual_failed_hunk_retains_nonzero_exit_and_original_output(self):
        self.patch.write_bytes(self.patch.read_bytes().replace(b'keep-before',b'missing-context'))
        self.pin['patches'][0]['sha256']=BUNDLE.digest(self.patch)
        self.originals[0]['blob']=BUNDLE.object_digest('blob',self.patch.read_bytes())
        with self.fixture_profile(), self.assertRaises(subprocess.CalledProcessError):
            self.replay()
        actual=self.recorder.executions[-1]
        self.assertEqual(actual['exitCode'],1)
        self.assertIn('FAILED',Path(actual['stdout']).read_text())
        self.assertTrue((self.source/'source.c.rej').is_file())
        with self.assertRaisesRegex(ValueError,'Unsuccessful'):
            self.recorder.raw_record({}, {})


if __name__=='__main__':
    unittest.main()
