"""Regression for the actual copied Windows environment used by the TLS probe."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('package_runtime',ROOT/'scripts/test-qt-package-runtime.py')
runtime=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class RuntimeEnvironmentTests(unittest.TestCase):
    def test_windows_copied_uppercase_keys_preserve_system_and_exclude_sdk(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            prefix=root/'installed product';prefix.mkdir()
            system=root/'Windows';(system/'System32').mkdir(parents=True)
            environment={'SYSTEMROOT':str(system),'PATH':str(root/'developer SDK'),
                'QT_PLUGIN_PATH':str(root/'developer plugins'),'LD_LIBRARY_PATH':str(root/'developer libraries'),
                'SSL_CERT_FILE':str(root/'developer trust'),'OPENSSL_MODULES':str(root/'developer crypto'),
                'USERPROFILE':str(root/'profile')}
            # Exercise os.environ.copy() itself, including its ordinary dict
            # semantics; no Qt/TLS success is simulated by this regression.
            with patch.dict(os.environ,environment,clear=True):
                self.assertIn('SYSTEMROOT',os.environ.copy())
                actual=runtime.installed_environment(prefix,True)
            self.assertEqual(actual['PATH'].split(os.pathsep),list(map(str,(prefix,system/'System32',system))))
            self.assertEqual(actual['QT_PLUGIN_PATH'],str(prefix/'plugins'))
            self.assertEqual(actual['USERPROFILE'],environment['USERPROFILE'])
            for name in ('LD_LIBRARY_PATH','SSL_CERT_FILE','OPENSSL_MODULES'):
                self.assertNotIn(name,actual)

    def test_missing_windows_system_root_fails_before_probe(self):
        with patch.dict(os.environ,{},clear=True):
            with self.assertRaisesRegex(ValueError,'SystemRoot'):
                runtime.installed_environment(Path.cwd(),True)


if __name__=='__main__':unittest.main()
