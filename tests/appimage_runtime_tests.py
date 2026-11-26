#!/usr/bin/env python3
"""Offline contracts with real local ar/ELF files, not a runtime-build claim."""
import hashlib
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
HELPER = runpy.run_path(str(ROOT / 'build/package/appimage_runtime.py'))
BUILDER = runpy.run_path(str(ROOT / 'scripts/build-appimage-runtime.py'))
BUNDLE = HELPER['bundle']
sys.path.insert(0, str(ROOT / 'scripts'))
PACKAGE = runpy.run_path(str(ROOT / 'scripts/package-qt.py'))


class ProfileTests(unittest.TestCase):
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
        item={'id':'contract-fixture', 'directory':source, 'files':files, 'sourceTreeSha256':BUNDLE.tree_digest(files),
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


if __name__=='__main__':
    unittest.main()
