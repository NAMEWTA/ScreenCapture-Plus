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
HEADER_LICENSES = runpy.run_path(str(ROOT / 'build/package/linux_header_licenses.py'))


class ProfileTests(unittest.TestCase):
    def test_original_header_license_object_chain_and_texts_are_fixed(self):
        pin=BUNDLE.read_json(ROOT/'build/package/dependencies.lock.json')['runtimeSourceBuild']['sources'][HEADERS['COMPONENT']]
        original=HEADER_LICENSES['validate_profile'](HEADERS['COMPONENT'],pin,'linux',ROOT)
        self.assertEqual(len(original['kernel']['licenseTexts']),11)
        self.assertEqual(original['headers']['commit'],'22bba0124dfe2728ef5b4555baa1ae0960492665')
        self.assertEqual(original['kernel']['commit'],'fb683b5e3f53a73e761952735736180939a313df')
        for component,platform,changed in (
                ('unrelated-component','linux',pin),(HEADERS['COMPONENT'],'windows',pin),
                (HEADERS['COMPONENT'],'linux',{**pin,'licenseSupplement':{'profile':'unknown'}}),
                (HEADERS['COMPONENT'],'linux',{**pin,'sourceArchive':{'sha256':'0'*64}})):
            with self.subTest(component=component,platform=platform),self.assertRaisesRegex(ValueError,'exact original'):
                HEADER_LICENSES['validate_profile'](component,changed,platform,ROOT)
        with tempfile.TemporaryDirectory() as value:
            project=Path(value);relative=Path(HEADER_LICENSES['PROVENANCE']['path']).parent
            shutil.copytree(ROOT/relative,project/relative)
            license_file=project/relative/'linux/LICENSES/preferred/GPL-2.0'
            saved=license_file.read_bytes();license_file.write_bytes(saved+b'changed original license\n')
            with self.assertRaisesRegex(ValueError,'original Git blob'):
                HEADER_LICENSES['validate_profile'](HEADERS['COMPONENT'],pin,'linux',project)
            license_file.write_bytes(saved)
            original_file=project/HEADER_LICENSES['PROVENANCE']['path']
            altered=BUNDLE.read_json(original_file)
            altered['kernel']['trees'][0]['entries'][0]['sha']='0'*40
            original_file.write_bytes(BUNDLE.canonical(altered))
            with self.assertRaisesRegex(ValueError,'locked bytes'):
                HEADER_LICENSES['validate_profile'](HEADERS['COMPONENT'],pin,'linux',project)
            changed=copy.deepcopy(pin);changed['licenseSupplement']['provenance'].update(
                size=original_file.stat().st_size,sha256=BUNDLE.digest(original_file))
            with self.assertRaisesRegex(ValueError,'exact original'):
                HEADER_LICENSES['validate_profile'](HEADERS['COMPONENT'],changed,'linux',project)

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
            for key in ('patchApplication','sourcePathProfile','path_profile','licenseSupplement','externalLicenses','licenseTexts'):
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

    def test_only_the_exact_optional_eh_not_found_result_avoids_a_file_lookup(self):
        prefix = self.root/'installed'; prefix.mkdir()
        archive = prefix/'libgcc.a'
        subprocess.run(['ar','rcs',archive,self.object], check=True, capture_output=True)
        installed = {'libgcc.a': {**HELPER['file_identity'](archive), 'component':'actual-fixture-copy'}}
        self.assertIsNone(HELPER['compiler_file_result']('libgcc_eh.a','libgcc_eh.a',prefix,installed))
        result = HELPER['compiler_file_result']('libgcc.a',str(archive),prefix,installed)
        self.assertEqual(result['producerOutput'],'libgcc.a')
        for name,reported in (('libgcc.a','libgcc.a'),('rcrt1.o','rcrt1.o'),
                              ('libgcc_eh.a','other.a'),('libgcc_eh.a','lib/libgcc_eh.a')):
            with self.assertRaisesRegex(ValueError,'owned installed file'):
                HELPER['compiler_file_result'](name,reported,prefix,installed)
        with self.assertRaisesRegex(ValueError,'escaped'):
            HELPER['compiler_file_result']('libgcc.a',str(self.object),prefix,installed)
        separate = prefix/'libgcc_eh.a'; shutil.copyfile(archive,separate)
        installed[separate.name] = {**HELPER['file_identity'](separate), 'component':'actual-fixture-copy'}
        self.assertIsNotNone(HELPER['compiler_file_result']('libgcc_eh.a',str(separate),prefix,installed))
        separate.write_bytes(separate.read_bytes()+b'changed actual input')
        with self.assertRaisesRegex(ValueError,'matching actual installation'):
            HELPER['compiler_file_result']('libgcc_eh.a',str(separate),prefix,installed)

    def test_real_static_relink_accounts_for_all_copied_inputs_and_archive_members(self):
        # These are actual host compiler outputs copied into a fixture prefix,
        # not a claim that the target musl compiler has been built locally.
        prefix = self.root/'copied-host-inputs'; prefix.mkdir()
        (prefix/'alias').mkdir()
        installed, arguments = {}, []
        for index,path in enumerate(HELPER['link_loads'](self.map.read_text())):
            if path == self.object:
                arguments.append(path)
                continue
            copied = prefix/(str(index)+'-'+path.name)
            shutil.copyfile(path,copied)
            installed[copied.name] = {**HELPER['file_identity'](copied), 'component':'actual-fixture-copy'}
            arguments.append(prefix/'alias'/('..')/copied.name if copied.suffix=='.a' else copied)
        map_path, binary = self.root/'copied.map', self.root/'copied-probe'
        subprocess.run(['gcc','-static-pie','-nostdlib','-Wl,--start-group',*arguments,'-Wl,--end-group',
                        '-Wl,-Map='+str(map_path),'-o',binary], check=True, capture_output=True)
        subprocess.run([binary], check=True, capture_output=True)
        HELPER['inspect_elf'](binary,marked=False)
        records = HELPER['toolchain_probe_inputs'](map_path,self.object,prefix,installed)
        self.assertEqual({item['path'] for item in records},{str(path.resolve()) for path in arguments})
        archives = [item for item in records if item['path'].endswith('.a')]
        self.assertTrue(archives)
        self.assertIn('/alias/../',map_path.read_text())
        self.assertTrue(any(item['membersMentionedInMap'] for item in archives))
        for item in archives:
            self.assertEqual(item['members'],HELPER['archive_members'](Path(item['path'])))
        removed = next(iter(installed)); missing = dict(installed); del missing[removed]
        with self.assertRaisesRegex(ValueError,'matching actual installation'):
            HELPER['toolchain_probe_inputs'](map_path,self.object,prefix,missing)
        with self.assertRaisesRegex(ValueError,'outside its actual new installation'):
            HELPER['toolchain_probe_inputs'](self.map,self.object,prefix,installed)

    @unittest.skipUnless(shutil.which('g++'), 'The exception probe requires a real C++ compiler')
    def test_real_exception_fixture_unwinds_and_its_host_inputs_are_not_target_evidence(self):
        source = ROOT/'build/package/toolchain-eh-probe.cpp'
        obj, binary, map_path = self.root/'exception.o', self.root/'exception', self.root/'exception.map'
        subprocess.run(['g++',*HELPER['EH_COMPILE_FLAGS'],'-c',source,'-o',obj],check=True,capture_output=True)
        subprocess.run(['g++','-static-pie',obj,'-Wl,-Map='+str(map_path),'-o',binary],check=True,capture_output=True)
        HELPER['inspect_elf'](binary,marked=False)
        subprocess.run([binary],check=True,capture_output=True)
        prefix = self.root/'empty-target'; prefix.mkdir()
        with self.assertRaisesRegex(ValueError,'outside its actual new installation'):
            HELPER['toolchain_probe_inputs'](map_path,obj,prefix,{})

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
    def test_actual_archive_keeps_external_license_texts_separate_and_native_gaps_open(self):
        configured=os.environ.get('HUHUCAPTURE_TEST_LINUX_HEADER_ARCHIVE')
        archive=Path(configured) if configured else ROOT/'.deps/appimage-runtime-source/downloads/appimage-linux-headers-linux-headers-4.19.88-2.tar.xz'
        if not configured and not archive.is_file():
            self.skipTest('Restore the original header archive or set HUHUCAPTURE_TEST_LINUX_HEADER_ARCHIVE')
        self.assertTrue(archive.is_file() and not archive.is_symlink())
        self.assertEqual(archive.stat().st_size,1081424)
        self.assertEqual(BUNDLE.digest(archive),HEADERS['ARCHIVE_SHA256'])
        lock=BUNDLE.read_json(ROOT/'build/package/dependencies.lock.json')
        pin=lock['runtimeSourceBuild']['sources'][HEADERS['COMPONENT']]
        with tempfile.TemporaryDirectory() as value:
            root=Path(value)
            def collector(name,original_pin):
                stage=root/name;stage.mkdir()
                actual=BUNDLE.Collector({},root,stage)
                actual.platform='linux';actual.pins={HEADERS['COMPONENT']:original_pin}
                actual.exported_project=ROOT
                copied=actual.sources/'huhucapture/build/package/dependencies.lock.json'
                copied.parent.mkdir(parents=True);shutil.copyfile(ROOT/'build/package/dependencies.lock.json',copied)
                return actual
            item={'id':HEADERS['COMPONENT'],'kind':'tar','archive':str(archive),'prefix':HEADERS['ARCHIVE_PREFIX']}
            missing=collector('without-supplement',{key:value for key,value in pin.items() if key!='licenseSupplement'})
            missing.collect_source(item)
            expected_gap=HEADERS['COMPONENT']+': no retained license/copyright text'
            self.assertIn(expected_gap,missing.gaps)
            missing.retain('arbitrary-unbound-license',str(ROOT/'third_party/appimage/linux-headers-licenses/linux/COPYING'))
            self.assertIn(expected_gap,missing.gaps,'An unrelated materials-only entry cannot resolve a component license gap')
            actual=collector('with-exact-supplement',pin)
            actual.gap('unrelated-source: original evidence is still missing')
            actual.collect_source(item)
            self.assertEqual(actual.gaps,['unrelated-source: original evidence is still missing'])
            component=actual.components[HEADERS['COMPONENT']]
            self.assertEqual(component['files'],missing.components[HEADERS['COMPONENT']]['files'])
            self.assertEqual(component['sourceTreeSha256'],HEADERS['TREE_SHA256'])
            self.assertEqual(component['licenses'],[])
            external=component['externalLicenses']
            self.assertEqual(len(external['originals']),14)
            self.assertTrue(all(record['material']['path'].startswith('materials/') for record in external['originals']))
            observations=BUNDLE.read_json(actual.stage/external['observations']['path'])
            self.assertEqual(len(observations['files']),1325)
            self.assertEqual(len(observations['withoutSpdx']),311)
            self.assertEqual(len(observations['spdxExpressions']),18)
            self.assertIn('tools/install.sh',observations['withoutSpdx'])
            self.assertIn('MIT',observations['spdxExpressions'])
            self.assertIn('(GPL-2.0 OR CDDL-1.0)',observations['spdxExpressions'])
            source=actual.sources/HEADERS['COMPONENT']
            for record in observations['files']:
                data=(source/record['path']).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(),record['sha256'])
                for block in record['noticeBlocks']:
                    self.assertEqual(hashlib.sha256(data[block['offset']:block['offset']+block['size']]).hexdigest(),block['sha256'])
            actual.collect_receipts()
            self.assertIn(HEADERS['COMPONENT']+': matching source/build-parameter receipt is missing',actual.gaps)
            self.assertIn('unrelated-source: original evidence is still missing',actual.gaps)
            self.assertEqual(actual.receipts,{})
            retained=root/'actual-source-materials.tar.gz'
            BUNDLE.make_archive(actual.stage,retained,1575535105,path_profile=actual.archive_path_profile())
            with tarfile.open(retained) as stream:
                names=set(stream.getnames())
                self.assertIn('huhucapture-sources/'+external['observations']['path'],names)
                self.assertNotIn('huhucapture-sources/'+component['path']+'/COPYING',names)
            header=source/'generic/include/linux/types.h';saved=header.read_bytes()
            header.write_bytes(saved+b'/* changed original header */\n')
            with self.assertRaisesRegex(ValueError,'original header file changed'):
                HEADER_LICENSES['review_source'](HEADERS['COMPONENT'],pin,'linux',ROOT,source,component['files'],BUNDLE.__dict__)
            header.write_bytes(saved)
            self.assertEqual(BUNDLE.digest(archive),HEADERS['ARCHIVE_SHA256'])

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


@unittest.skipUnless(sys.platform=='linux', 'Runtime dependency checkout executes on the native Linux producer')
class RuntimeGitCheckoutTests(unittest.TestCase):
    def test_real_runtime_restore_keeps_original_blobs_and_records_policy_before_fetch(self):
        with tempfile.TemporaryDirectory(prefix='runtime-git-input-') as value:
            root=Path(value);upstream=root/'upstream';upstream.mkdir()
            def git(directory,*arguments,data=None):
                return subprocess.run(['git','-c','core.hooksPath='+os.devnull,'-C',str(directory),*arguments],
                    input=data,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                    env=BUNDLE.git_environment(),check=True).stdout
            git(upstream,'init','--quiet')
            files={'.gitattributes':b'* text eol=lf\n*.vcproj* text eol=crlf\n*.cmd text eol=crlf\n',
                   'build/VS2008/fullbench/fullbench.vcproj':b'<Project>original LF bytes</Project>\n',
                   'original-crlf.cmd':b'@echo off\r\necho original CRLF\r\n'}
            for name,data in files.items():
                blob=git(upstream,'hash-object','-w','--stdin',data=data).decode().strip()
                git(upstream,'update-index','--add','--cacheinfo','100644,'+blob+','+name)
            git(upstream,'-c','user.name=Runtime checkout fixture','-c','user.email=runtime-fixture@example.invalid',
                'commit','--quiet','-m','Actual original source-byte fixture')
            commit=git(upstream,'rev-parse','HEAD').decode().strip()
            original_index=git(upstream,'ls-files','--stage')
            workspace=root/'workspace';workspace.mkdir()
            for name in ('sources','source-inputs','downloads','checkouts'):(workspace/name).mkdir()
            runtime_bundle=BUILDER['bundle'];original_command=runtime_bundle.git_command
            observed=[]
            def local_fixture_transport(directory,*arguments):
                command=original_command(directory,*arguments)
                if 'fetch' in arguments:
                    # Only this offline test enables its real local Git transport.
                    # The production HTTPS source validation and commands are unchanged.
                    observed.append(BUILDER['CHECKOUT']['verify_policy'](directory))
                    self.assertEqual(git(directory,'ls-files','--stage'),b'')
                    self.assertEqual(git(directory,'for-each-ref','--format=%(refname)'),b'')
                    index=command.index('fetch');command[index:index]=['-c','protocol.file.allow=always']
                return command
            recorder=BUILDER['API']['ExecutionLog'](workspace/'control','linux',ROOT/'scripts/build-appimage-runtime.py')
            with mock.patch.object(runtime_bundle,'git_command',side_effect=local_fixture_transport):
                item=BUILDER['restore_input']('native-source-fixture',{'repository':upstream.as_uri(),'commit':commit},
                                               workspace,recorder,BUILDER['clean_environment']())
            checkout=Path(item['collection']['directory'])
            original=BUNDLE.read_json(item['manifest']['path'])
            policy=original['sourceCheckout']
            self.assertEqual(len(observed),1)
            for name,data in files.items():
                self.assertEqual((checkout/name).read_bytes(),data)
                self.assertEqual((item['directory']/name).read_bytes(),data)
            self.assertEqual(git(upstream,'ls-files','--stage'),original_index)
            self.assertEqual(git(upstream,'rev-parse','HEAD').decode().strip(),commit)
            BUILDER['HELPER']['verify_checkout_policy'](checkout,policy)
            runtime_bundle.compare_checkout(checkout,item['files'],commit)
            attributes=checkout/'.git/info/attributes'
            attributes.write_bytes(attributes.read_bytes()+b'# changed policy\n')
            with self.assertRaisesRegex(ValueError,'policy is missing or changed'):
                BUILDER['HELPER']['verify_checkout_policy'](checkout,policy)
            attributes.write_bytes(BUILDER['CHECKOUT']['ATTRIBUTES'])
            retained=Path(policy['retainedPolicy']['path']);retained.write_bytes(retained.read_bytes()+b'# changed evidence\n')
            with self.assertRaisesRegex(ValueError,'build evidence changed'):
                BUILDER['HELPER']['verify_checkout_policy'](checkout,policy)


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
