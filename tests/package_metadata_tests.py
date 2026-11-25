#!/usr/bin/env python3
"""Real-file tamper and package identity checks; not installer acceptance."""
import importlib.util
import json
import os
from pathlib import Path
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


class MetadataBoundaries(unittest.TestCase):
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
