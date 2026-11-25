#!/usr/bin/env python3
"""Bounded ownership/upgrade checks for an actual generated per-user installer.

Run only in a fresh Windows CI account. Pre-existing huhucapture installation
registration or Start Menu ownership is a hard refusal before executing setup.
This is not physical desktop or clean-machine dependency acceptance.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

if os.name == 'nt':
    import winreg


INSTALL_KEY = r'Software\huhucapture\Installation'
UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\huhucapture'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def values(key, view):
    try:
        handle = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_READ | view)
    except FileNotFoundError:
        return None
    result = {}
    with handle:
        count = winreg.QueryInfoKey(handle)[1]
        for index in range(count):
            name, value, kind = winreg.EnumValue(handle, index)
            result[name] = (value, kind)
    return result


def registration():
    return {key: values(key, winreg.KEY_WOW64_64KEY) for key in (INSTALL_KEY, UNINSTALL_KEY)}


def registered_location():
    record = values(INSTALL_KEY, winreg.KEY_WOW64_64KEY)
    if record is None or 'Location' not in record:
        return None
    value, kind = record['Location']
    require(kind == winreg.REG_SZ and isinstance(value, str), 'Unexpected registered location type')
    return Path(value)


def programs_folder():
    class GUID(ctypes.Structure):
        _fields_ = [('Data1', ctypes.c_uint32), ('Data2', ctypes.c_uint16),
                    ('Data3', ctypes.c_uint16), ('Data4', ctypes.c_ubyte * 8)]
    folder = GUID.from_buffer_copy(uuid.UUID('A77F5D77-2E2B-44C3-A6A2-ABA601054A51').bytes_le)
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    ole = ctypes.WinDLL('ole32', use_last_error=True)
    shell.SHGetKnownFolderPath.argtypes = [ctypes.POINTER(GUID), ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    shell.SHGetKnownFolderPath.restype = ctypes.c_long
    ole.CoTaskMemFree.argtypes = [ctypes.c_void_p]
    output = ctypes.c_void_p()
    result = shell.SHGetKnownFolderPath(ctypes.byref(folder), 0, None, ctypes.byref(output))
    require(result >= 0 and bool(output.value), 'Current-user Programs known folder is unavailable')
    try:
        path = Path(ctypes.wstring_at(output))
        require(path.is_absolute(), 'Programs known folder did not return an absolute path')
        return path
    finally:
        ole.CoTaskMemFree(output)


def run_native(executable, destination, *, uninstall=False, timeout=180):
    # NSIS documents /D= as the final, unquoted parameter, including spaces.
    # _?= similarly keeps an uninstaller in its actual directory so waiting for
    # this process observes completion instead of only a self-copying launcher.
    for value in (str(executable), str(destination)):
        require('"' not in value and '\0' not in value, 'Unsupported NSIS command path')
    command = '"' + str(executable) + '" /S ' + ('_?=' if uninstall else '/D=') + str(destination)
    return subprocess.run(command, executable=str(executable), shell=False, timeout=timeout,
                          capture_output=True, text=True, errors='replace').returncode


def shortcuts(folder):
    result = {}
    for name in ('huhucapture.lnk', 'Uninstall.lnk'):
        path = folder / name
        require(path.is_file(), 'The actual per-user Start Menu shortcut is missing: ' + name)
        result[name] = digest(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    require(os.name == 'nt', 'Actual per-user installer checks require native Windows')
    installer = args.installer.resolve()
    require(installer.is_file() and installer.suffix.lower() == '.exe', 'An actual generated installer EXE is required')
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        for key in (INSTALL_KEY, UNINSTALL_KEY):
            require(values(key, view) is None, 'Refusing to touch pre-existing per-user installation ownership: ' + key)
    menu = programs_folder() / 'huhucapture'
    require(not os.path.lexists(menu), 'Refusing to touch pre-existing huhucapture Start Menu ownership')
    args.evidence.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='native-installer-', dir=args.evidence.resolve()))
    installed = root / 'Installed with spaces 中文'
    installed.mkdir()
    sentinels = {'unrelated-user-file.txt': b'Not a packaged file; preserve through upgrade and uninstall.\n',
                 'config.json': b'{"userOwned":true,"unknownFutureField":"preserve exactly"}\n'}
    for name, content in sentinels.items():
        (installed / name).write_bytes(content)
    report = {'installer': installer.name, 'installerSha256': digest(installer), 'destination': str(installed),
              'scope': 'Actual native per-user installer ownership and upgrade flow; no physical desktop acceptance.',
              'checks': [], 'passed': False}
    uninstaller_hash = None
    started = time.monotonic()

    def check_user_files():
        for name, expected in sentinels.items():
            require((installed / name).read_bytes() == expected, 'User-owned file changed: ' + name)

    try:
        require(run_native(installer, installed) == 0, 'Initial silent installation failed')
        require(registered_location() == installed, 'Installer registered a different destination')
        marker = json.loads((installed / 'installation.json').read_text(encoding='utf-8'))
        require(marker == {'schemaVersion': 1, 'product': 'huhucapture', 'platform': 'windows',
                           'architecture': 'x64', 'packageKind': 'installer'}, 'Installed package identity is incorrect')
        require((installed / 'huhucapture.exe').is_file(), 'The actual application executable was not installed')
        uninstaller = installed / 'uninstall.exe'
        uninstaller_hash = digest(uninstaller)
        check_user_files()
        shortcuts(menu)
        report['checks'].append('actual installer, 64-bit HKCU, current-user shortcuts and package marker')

        require(run_native(installer, installed) == 0, 'Same-directory reinstall/upgrade failed')
        require(registered_location() == installed, 'Reinstall changed the registered destination')
        check_user_files()
        uninstaller_hash = digest(uninstaller)
        expected_registry = registration()
        expected_shortcuts = shortcuts(menu)
        expected_product = digest(installed / 'huhucapture.exe')
        report['checks'].append('same-directory reinstall preserves unrelated file and config bytes')

        other = root / 'Different destination 中文'
        changed_start = time.monotonic()
        require(run_native(installer, other, timeout=30) != 0, 'Changing a registered installation path was not refused')
        require(time.monotonic() - changed_start < 30, 'Silent changed-location refusal was not bounded')
        require(not other.exists() or not any(other.iterdir()), 'Refused migration wrote a second installation tree')
        require(registration() == expected_registry and shortcuts(menu) == expected_shortcuts,
                'Refused migration changed current registration or shortcuts')
        check_user_files()
        report['checks'].append('changed /D= returns nonzero without a blocking dialog or mutations')

        stale = root / 'Stale copy 中文'
        stale.mkdir()
        shutil.copyfile(uninstaller, stale / 'uninstall.exe')
        (stale / 'unrelated-stale-file.txt').write_bytes(b'Owned regression sentinel; not a package file.')
        require(run_native(stale / 'uninstall.exe', stale, uninstall=True, timeout=60) == 0, 'Stale copied uninstaller failed')
        require(registration() == expected_registry and shortcuts(menu) == expected_shortcuts and
                digest(installed / 'huhucapture.exe') == expected_product,
                'Stale uninstaller removed the active registration, shortcuts or actual product')
        require((stale / 'unrelated-stale-file.txt').is_file(), 'Stale uninstall removed an unrelated file')
        check_user_files()
        report['checks'].append('stale copied uninstaller cannot remove active ownership or user files')

        require(run_native(uninstaller, installed, uninstall=True, timeout=60) == 0, 'Current installation could not be uninstalled')
        check_user_files()
        require(all(value is None for value in registration().values()), 'Uninstall retained owned registry entries')
        require(not menu.exists(), 'Uninstall retained owned Start Menu entries')
        require(not (installed / 'huhucapture.exe').exists(), 'Uninstall retained the owned application executable')
        report['checks'].append('current uninstall removes its ownership and retains unrelated/config bytes')
        report['passed'] = True
    finally:
        # No raw registry deletion or directory recursion. On failure, try only
        # this run's known installed uninstaller while both registration and its
        # exact file hash still identify the object observed after installation.
        if not report['passed'] and uninstaller_hash is not None:
            owned = installed / 'uninstall.exe'
            try:
                if registered_location() == installed and owned.is_file() and digest(owned) == uninstaller_hash:
                    report['failureCleanupExitCode'] = run_native(owned, installed, uninstall=True, timeout=60)
                else:
                    report['failureCleanup'] = 'Retained: ownership differs or was not established; no unknown files/registry removed.'
            except Exception as error:
                report['failureCleanup'] = str(error)
        report['elapsedSeconds'] = time.monotonic() - started
        (root / 'installer-regression.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
        print(json.dumps(report, indent=2, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
