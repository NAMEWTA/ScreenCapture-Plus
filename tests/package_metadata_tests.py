#!/usr/bin/env python3
"""Real-file tamper and package identity checks; not installer acceptance."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('package_metadata',ROOT/'build/package/metadata.py')
metadata=importlib.util.module_from_spec(spec);spec.loader.exec_module(metadata)
sys.path.insert(0,str(ROOT/'scripts'))
spec=importlib.util.spec_from_file_location('package_qt',ROOT/'scripts/package-qt.py')
packager=importlib.util.module_from_spec(spec);spec.loader.exec_module(packager)


def nsis_layout_fixture(root):
    # Text-only directory contract fixture. These files are never executed or
    # packaged as a product; the separate Windows test uses the real archive.
    for relative in ('makensis.exe', 'Bin/makensis.exe', 'COPYING',
                     'Include/MUI2.nsh', 'Include/LogicLib.nsh', 'Include/x64.nsh', 'Include/WinVer.nsh',
                     'Stubs/uninst', 'Stubs/lzma_solid-x86-unicode',
                     'Plugins/x86-unicode/nsDialogs.dll', 'Plugins/x86-unicode/System.dll',
                     'Contrib/Modern UI 2/MUI2.nsh', 'Contrib/Language files/English.nsh'):
        path=root/relative;path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text('Directory selection fixture only: '+relative+'\n')


class MetadataBoundaries(unittest.TestCase):
    def test_stable_manifest_requires_explicit_false_flags(self):
        original={'schemaVersion':1,'product':'huhucapture','repository':'NAMEWTA/huhucapture',
                  'version':'0.0.5','tag':'v0.0.5','sourceCommit':'1'*40,
                  'sourceDirty':False,'developmentOnly':False}
        # This is only a metadata contract, never a fabricated package/release.
        identity=packager.release_identity(original)
        encoded=json.dumps({**identity,'assets':[]})
        self.assertIs(json.loads(encoded)['developmentOnly'],False)
        for key in ('developmentOnly','sourceDirty'):
            for value in (None,True,0,'false'):
                with self.subTest(key=key,value=value):
                    with self.assertRaisesRegex(ValueError,'package metadata'):
                        packager.release_identity({**original,key:value})
            missing=dict(original);del missing[key]
            with self.assertRaisesRegex(ValueError,'package metadata'):
                packager.release_identity(missing)
        with self.assertRaisesRegex(ValueError,'Noncanonical'):
            packager.release_identity({**original,'schemaVersion':True})

    def test_final_source_map_binds_real_bytes_and_link_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'appimage/usr/bin').mkdir(parents=True)
            payload=root/'appimage/usr/bin/owned-contract-file'
            payload.write_bytes(b'Not an application; actual final-tree inventory contract.\n')
            if os.name!='nt': (payload.parent/'owned-alias').symlink_to(payload.name)
            collector=packager.source_inventory_api()
            source=collector.inventory(root)
            metadata_files=metadata.inventory(root)
            deployment={'files':source,'treeSha256':collector.tree_digest(source)}
            packager.verify_inventory_binding(metadata_files,deployment)
            altered=json.loads(json.dumps(metadata_files))
            next(entry for entry in altered if 'linkTarget' not in entry)['sha256']='0'*64
            with self.assertRaisesRegex(ValueError,'bytes differ'):
                packager.verify_inventory_binding(altered,deployment)
            with self.assertRaisesRegex(ValueError,'paths differ'):
                packager.verify_inventory_binding(metadata_files[:-1],deployment)
            if os.name!='nt':
                altered=json.loads(json.dumps(metadata_files))
                next(entry for entry in altered if 'linkTarget' in entry)['linkTarget']='another-file'
                with self.assertRaisesRegex(ValueError,'link differs'):
                    packager.verify_inventory_binding(altered,deployment)
            payload.write_bytes(b'Changed bytes after collection must not be accepted.\n')
            with self.assertRaisesRegex(ValueError,'bytes differ'):
                packager.verify_inventory_binding(metadata.inventory(root),deployment)

    def test_generated_nsis_file_arguments_quote_the_complete_oname_token(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);prefix=root/'NSIS input with spaces $value';prefix.mkdir()
            names=('plain.txt','payload with spaces 中文 $value.txt')
            for name in names:
                (prefix/name).write_text('Script generation fixture only; never installed as a product.\n')
            output=root/'not-built.exe'
            script=packager.windows_installer_script(prefix,output,'0.0.5',metadata.inventory(prefix))
            commands=[line for line in script.splitlines() if line.startswith('    File ')]
            self.assertEqual(len(commands),len(names))
            for command,name in zip(commands,sorted(names)):
                # The fixed official NSIS parser recognizes quoting at the
                # beginning of each token and rejects /oname="filename".
                match=re.fullmatch(r'    File "/oname=([^"]+)" "([^"]+)"',command)
                self.assertIsNotNone(match,command)
                self.assertEqual(match[1],name.replace('$','$$'))
                self.assertEqual(match[2],str(prefix/name).replace('$','$$'))
            self.assertFalse(output.exists(),'Generating the script must not fabricate a compiled installer')
            self.assertIn('AllowSkipFiles off',script)
            self.assertIn('Call CheckRegisteredLocation',script)
            self.assertNotIn('InstallDirRegKey ', '\n'.join(line for line in script.splitlines() if not line.startswith(';')))
            self.assertIn('ExecWait \'"$PLUGINSDIR\\previous-uninstall.exe" /S _?=$INSTDIR\' $2',script)
            lines=script.splitlines()
            for index,line in enumerate(lines):
                if line.startswith(('    File ', '    SetOutPath ')):
                    self.assertEqual(lines[index+1],'    IfErrors install_failed')
            uninstall=script[script.index('Section "Uninstall"'):]
            ownership=uninstall.index('${If} $0 == $INSTDIR')
            self.assertLess(ownership,uninstall.index('StrCpy $OperationTarget "$SMPROGRAMS'))

    def test_nsis_launcher_and_bin_compiler_are_one_distribution(self):
        with tempfile.TemporaryDirectory() as directory:
            extracted=Path(directory); root=extracted/'distribution'
            nsis_layout_fixture(root)
            selected,compiler,evidence=packager.nsis_distribution(extracted)
            self.assertEqual(selected,root)
            self.assertEqual(compiler,root/'Bin/makensis.exe')
            self.assertEqual(evidence['compilerPath'],'distribution/Bin/makensis.exe')
            self.assertEqual({item['path'] for item in evidence['compilerCandidates']},
                             {'distribution/makensis.exe','distribution/Bin/makensis.exe'})
            for item in evidence['compilerCandidates']:
                actual=extracted/item['path']
                self.assertEqual(item['size'],actual.stat().st_size)
                self.assertEqual(item['sha256'],metadata.sha256(actual))
            # One launcher/compiler pair without the actual required stub is
            # not a usable distribution, even though both names still exist.
            stub=root/'Stubs/lzma_solid-x86-unicode'; saved=stub.read_bytes();stub.unlink()
            with self.assertRaisesRegex(ValueError,'complete distribution root'):
                packager.nsis_distribution(extracted)
            stub.write_bytes(saved)
            shutil.copytree(root,extracted/'second-distribution')
            with self.assertRaisesRegex(ValueError,'exactly one complete distribution root'):
                packager.nsis_distribution(extracted)

    def test_nsis_does_not_pick_the_first_of_extra_compilers(self):
        with tempfile.TemporaryDirectory() as directory:
            extracted=Path(directory);nsis_layout_fixture(extracted)
            unexpected=extracted/'other/makensis.exe';unexpected.parent.mkdir()
            shutil.copyfile(extracted/'Bin/makensis.exe',unexpected)
            with self.assertRaisesRegex(ValueError,'Unexpected compiler candidates'):
                packager.nsis_distribution(extracted)

    def test_nsis_rejects_unreviewed_archive_before_extracting(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);archive=root/'unreviewed.exe'
            archive.write_text('This is deliberately not the reviewed official distribution.')
            with self.assertRaisesRegex(ValueError,'exact reviewed NSIS distribution'):
                packager.prepare_nsis(archive,root)
            self.assertFalse((root/'nsis').exists())

    @unittest.skipUnless(os.name == 'nt' and (ROOT/'artifacts/packaging-tools/nsis/nsis-3.11-setup.exe').is_file(),
                         'Requires the restored official NSIS archive and a Windows compiler host')
    def test_real_official_nsis_archive_selects_and_runs_bin_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace=Path(directory)
            compiler,environment,evidence=packager.prepare_nsis(
                ROOT/'artifacts/packaging-tools/nsis/nsis-3.11-setup.exe',workspace)
            self.assertEqual(evidence['version'],'v3.11')
            self.assertTrue(compiler.samefile(Path(environment['NSISDIR'])/'Bin/makensis.exe'))
            self.assertEqual(len(evidence['compilerCandidates']),2)
            self.assertEqual(evidence['sha256'],metadata.sha256(compiler))
            unexpected=workspace/'nsis/unexpected/makensis.exe';unexpected.parent.mkdir()
            shutil.copyfile(compiler,unexpected)
            with self.assertRaisesRegex(ValueError,'Unexpected compiler candidates'):
                packager.nsis_distribution(workspace/'nsis')

    def test_real_inventory_changes_and_links(self):
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as other:
            root=Path(directory); (root/'unicode 中文').mkdir()
            file=root/'unicode 中文/test.txt'; file.write_text('Owned metadata test, not an application payload.\n')
            snapshot=metadata.inventory(root)
            metadata.verify_inventory(root,snapshot)
            file.write_text('Changed bytes of equal declared ownership.\n')
            with self.assertRaisesRegex(ValueError,'changed'): metadata.verify_inventory(root,snapshot)
            outsider=Path(other)/'user-file';outsider.write_text('Unrelated file must never enter package metadata.')
            if os.name != 'nt':
                (root/'outside').symlink_to(outsider)
                with self.assertRaisesRegex(ValueError,'escapes'): metadata.inventory(root)
            self.assertEqual(outsider.read_text(),'Unrelated file must never enter package metadata.')

    def test_actual_zip_is_bound_to_name_kind_hash_and_size(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); path=root/'huhucapture-v0.0.5-windows-x64.zip'
            with zipfile.ZipFile(path,'x') as archive:
                archive.writestr('TEST-SCOPE.txt','Transport metadata fixture only; not an installable product.')
            record=metadata.descriptor(path)
            self.assertEqual(metadata.verify_artifact(root,record,'portable-zip','0.0.5'),record)
            with self.assertRaisesRegex(ValueError,'name'): metadata.verify_artifact(root,record,'installer','0.0.5')
            with self.assertRaisesRegex(ValueError,'name'): metadata.verify_artifact(root,record,'portable-zip','0.0.6')
            with self.assertRaisesRegex(ValueError,'name'): metadata.verify_artifact(root,{**record,'name':'../'+path.name},'portable-zip','0.0.5')
            with path.open('ab') as output: output.write(b'changed after hashing')
            with self.assertRaisesRegex(ValueError,'bytes changed'): metadata.verify_artifact(root,record,'portable-zip','0.0.5')
            path.write_bytes(b'This is not a ZIP even if its updated hash matches.')
            with self.assertRaisesRegex(ValueError,'header'): metadata.verify_artifact(root,metadata.descriptor(path),'portable-zip','0.0.5')

    def test_json_writer_never_replaces_an_existing_result(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'metadata.json'; original={'real':'owned test result'}
            metadata.json_write(path,original)
            with self.assertRaises(FileExistsError): metadata.json_write(path,{'replaced':True})
            self.assertEqual(metadata.json_read(path),original)

    @unittest.skipIf(os.name == 'nt','The actual ELF identity probe is compiled on Linux; PE packaging is validated natively on Windows')
    def test_real_compiled_binary_rejects_a_swapped_receipt(self):
        compiler=shutil.which('c++')
        self.assertIsNotNone(compiler,'A real native compiler is required for this identity gate')
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            receipt={'schemaVersion':1,'product':'huhucapture','repository':'NAMEWTA/huhucapture','version':'0.0.5',
                     'tag':'v0.0.5','sourceCommit':'1'*40,'sourceDirty':True,'platform':'linux','architecture':'amd64',
                     'configuration':'RelWithDebInfo','compiler':{'id':'fixture','version':'fixture'},'cmakeVersion':'fixture',
                     'qtVersion':'6.12.1','dependencyLockSha256':'2'*64,'cargoLockSha256':'3'*64}
            # Compile the same retained binary-record template, in an explicitly
            # named metadata probe. No Qt/product success is fabricated here.
            source=(ROOT/'build/package/build-identity.cpp.in').read_text().replace('@HUHU_BINARY_BUILD_IDENTITY@',json.dumps(receipt))
            (root/'identity.cpp').write_text(source+'\nint main() { return 0; }\n')
            probe=root/'metadata-probe'
            subprocess.run([compiler,'-std=c++20','-Wl,--gc-sections','-o',str(probe),str(root/'identity.cpp')],check=True)
            metadata.verify_binary(probe,receipt)
            altered={**receipt,'sourceCommit':'4'*40}
            with self.assertRaisesRegex(ValueError,'sidecar'): metadata.verify_binary(probe,altered)
            bytes_=probe.read_bytes().replace(b'1'*40,b'5'*40)
            probe.write_bytes(bytes_)
            with self.assertRaisesRegex(ValueError,'sidecar'): metadata.verify_binary(probe,receipt)
            with self.assertRaisesRegex(ValueError,'Dirty sources'): metadata.identity(receipt)
            metadata.identity(receipt,development=True)


if __name__=='__main__':
    unittest.main(verbosity=2)
