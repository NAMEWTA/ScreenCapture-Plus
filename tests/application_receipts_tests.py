#!/usr/bin/env python3
"""Actual CMake link-model and native execution-proof rejection checks."""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
builder = runpy.run_path(str(ROOT / 'scripts/build-application-with-sources.py'))
helpers = runpy.run_path(str(ROOT / 'build/package/application_receipts.py'))
bundle = helpers['bundle']


class ApplicationReceiptsTests(unittest.TestCase):
    @unittest.skipIf(os.name == 'nt', 'This actual host-C++/Ninja fixture runs on Linux')
    def test_actual_link_model_binds_the_linked_archive_and_rejects_an_unlinked_copy(self):
        for program in ('cmake', 'ninja', 'c++', 'ar'):
            self.assertIsNotNone(shutil.which(program), 'Real link-model validation needs ' + program)
        with tempfile.TemporaryDirectory(prefix='actual-cmake-link-proof-') as temporary:
            root = Path(temporary).resolve()
            (root / 'input.cpp').write_text('int native_value() { return 7; }\n')
            subprocess.run(['c++', '-c', str(root / 'input.cpp'), '-o', str(root / 'input.o')], check=True)
            library = root / 'actual native.a'
            subprocess.run(['ar', 'rcs', str(library), str(root / 'input.o')], check=True)
            other = root / 'not linked.a'
            shutil.copyfile(library, other)
            (root / 'main.cpp').write_text('int native_value(); int main() { return native_value() == 7 ? 0 : 1; }\n')
            (root / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.25)\n'
                'project(ActualLinkEvidence CXX)\nadd_library(native STATIC IMPORTED)\n'
                'set_target_properties(native PROPERTIES IMPORTED_LOCATION "' + library.as_posix() + '")\n'
                'add_executable(huhucapture main.cpp)\ntarget_link_libraries(huhucapture PRIVATE native)\n')
            build = root / 'build'
            query = build / '.cmake/api/v1/query'
            query.mkdir(parents=True)
            (query / 'codemodel-v2').touch()
            subprocess.run(['cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja',
                            '-DCMAKE_BUILD_TYPE=RelWithDebInfo'], check=True, stdout=subprocess.PIPE)
            evidence = builder['linked_native_library'](build, 'RelWithDebInfo', library)
            self.assertEqual(len(evidence), 3)
            subprocess.run(['cmake', '--build', str(build)], check=True, stdout=subprocess.PIPE)
            subprocess.run([str(build / 'huhucapture')], check=True)
            with self.assertRaisesRegex(ValueError, 'actual generated linker inputs'):
                builder['linked_native_library'](build, 'RelWithDebInfo', other)
            with self.assertRaisesRegex(ValueError, 'actual product configuration'):
                builder['linked_native_library'](build, 'Debug', library)

    def test_legacy_or_modified_native_proof_is_rejected_before_any_cpp_build(self):
        with tempfile.TemporaryDirectory(prefix='native-proof-rejection-') as temporary:
            root = Path(temporary)
            source = helpers['SourceInputs'].__new__(helpers['SourceInputs'])
            source.platform, source.commit, source.sources = 'linux', '1' * 40, []
            receipt = root / 'build-info.json'
            receipt.write_bytes(bundle.canonical({'library': str(root / 'not-created.a')}))
            with self.assertRaisesRegex(ValueError, 'pre-build source execution proof'):
                source.checked_native_proof(receipt)
            proof = root / 'proof.json'
            proof.write_bytes(bundle.canonical({'kind': 'huhucapture-native-source-build'}))
            reference = {'path': str(proof), **helpers['API']['identity'](proof)}
            receipt.write_bytes(bundle.canonical({'sourceBuildProof': reference}))
            proof.write_bytes(bundle.canonical({'kind': 'changed after producer'}))
            with self.assertRaisesRegex(ValueError, 'pre-build source execution proof'):
                source.checked_native_proof(receipt)


if __name__ == '__main__':
    unittest.main()
