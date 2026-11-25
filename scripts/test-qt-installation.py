#!/usr/bin/env python3
"""Exercise real native packages without changing their build/release identity.

Run on an otherwise uninstalled Windows desktop or Debian/Ubuntu X11 runner.
NSIS and DEB use their actual installation/removal mechanisms. ZIP and AppImage
have file-based replacement/removal, reported separately. Reinstalling the same
artifact is deliberately not described as a cross-version upgrade.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location('huhucapture_package_metadata', ROOT / 'build/package/metadata.py')
meta = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(meta)
WINDOWS = sys.platform == 'win32'


def clean_host_environment():
    environment = os.environ.copy()
    for name in list(environment):
        upper = name.upper()
        if upper.startswith(('QT_', 'QML_', 'LD_', 'PIPEWIRE_', 'SPA_')) or upper in (
                'APPIMAGE', 'APPDIR', 'ARGV0', 'APPIMAGE_EXTRACT_AND_RUN', 'NO_CLEANUP'):
            environment.pop(name)
    if WINDOWS:
        system = Path(os.environ.get('SystemRoot', ''))
        meta.require(system.is_absolute() and (system / 'System32').is_dir(), 'Windows system directory is unavailable')
        environment['PATH'] = os.pathsep.join(map(str, (system / 'System32', system)))
    else:
        environment['PATH'] = '/usr/sbin:/usr/bin:/sbin:/bin'
    return environment


def regular(path):
    return path.is_file() and not path.is_symlink()


def verify_preserved(records):
    for value in records:
        path = Path(value['path'])
        meta.require(regular(path) and meta.descriptor(path) == value['bytes'], 'User data changed or disappeared: ' + str(path))


def preserve_record(path):
    return {'path': str(path.resolve()), 'bytes': meta.descriptor(path)}


def registry_location():
    if not WINDOWS:
        return None
    import winreg
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\huhucapture\Installation', 0,
                             winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    except FileNotFoundError:
        return None
    with key:
        try:
            return winreg.QueryValueEx(key, 'Location')[0]
        except FileNotFoundError as error:
            raise ValueError('An existing installation key lacks a Location; it must be retained') from error


def registry_uninstall():
    import winreg
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Uninstall\huhucapture', 0,
                             winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    except FileNotFoundError:
        return None
    with key:
        try:
            return {name: winreg.QueryValueEx(key, name)[0] for name in
                    ('DisplayName', 'DisplayVersion', 'InstallLocation', 'UninstallString')}
        except FileNotFoundError as error:
            raise ValueError('An existing uninstall key is incomplete; it must be retained') from error


@contextmanager
def windows_read_lock(path):
    """Hold a real existing payload file open without write/delete sharing."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(str(path), 0x80000000, 0x1, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        if not kernel.CloseHandle(handle):
            raise ctypes.WinError(ctypes.get_last_error())


class Validation:
    def __init__(self, args):
        self.args = args
        self.output = args.artifacts.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        meta.require(not any(self.output.iterdir()), 'Use an empty evidence directory; previous results are retained')
        self.host_env = clean_host_environment()
        self.counter = 0
        self.report = {
            'schemaVersion': 1, 'product': 'huhucapture', 'passed': False,
            'host': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine()},
            'crossVersionUpgrade': {'status': 'not-run', 'reason': 'Only the exact same package bytes are reinstalled.'},
            'interactiveInstallerPages': {'status': 'not-run', 'reason': 'This harness exercises the real silent installers and actual product GUI.'},
            'physicalDesktopAcceptance': {'status': 'not-run', 'reason': 'A native CI desktop/Xvfb journey does not verify physical mixed-DPI, IME, GPU or Wayland desktops.'},
            'cases': [], 'commands': [], 'cleanup': [],
        }
        self.shared_preserved = []
        self.created_history = []
        self.history = None
        self.programs = None
        self.native_root = None
        self.native_started = False
        self.native_removed = False
        self.system_preserved = []
        self.packages = args.packages.resolve()
        self.probe = args.probe.resolve()
        self.video = args.video.resolve()
        meta.require(regular(self.probe) and regular(self.video), 'Supply the real compiled GUI probe and encoded video fixture')
        with self.video.open('rb') as incoming:
            header = incoming.read(32)
        meta.require(header[4:8] == b'ftyp', 'The media preservation fixture must be an actual encoded MP4')
        self.report['videoFixture'] = meta.descriptor(self.video)

    def run(self, arguments, *, tag, env=None, timeout=180, check=True, native_command_line=False):
        self.counter += 1
        name = f'{self.counter:02d}-{tag}'
        command = arguments if native_command_line else list(map(str, arguments))
        entry = {'tag': tag, 'command': command, 'log': name + '.log'}
        self.report['commands'].append(entry)
        print('RUN', tag, flush=True)
        started = time.monotonic()
        with (self.output / (name + '.log')).open('xb') as log:
            completed = subprocess.run(command, env=env if env is not None else self.host_env,
                                       stdout=log, stderr=subprocess.STDOUT, timeout=timeout,
                                       shell=False)
        entry.update({'returnCode': completed.returncode, 'seconds': round(time.monotonic() - started, 3)})
        content = (self.output / (name + '.log')).read_text(encoding='utf-8', errors='replace')
        if check and completed.returncode:
            raise RuntimeError(f'{tag} failed ({completed.returncode}); see {entry["log"]}')
        return completed.returncode, content

    def probe_call(self, arguments, *, tag, timeout=120):
        # The independent test driver may use the fixed development SDK. It
        # removes those paths from the product child's environment itself and
        # inspects the child's actual loaded Qt/CRT/platform modules.
        self.counter += 1
        stem = f'{self.counter:02d}-{tag}'
        command = [str(self.probe), *map(str, arguments)]
        entry = {'tag': tag, 'command': command, 'stdout': stem + '.json', 'stderr': stem + '.log'}
        self.report['commands'].append(entry)
        print('RUN', tag, flush=True)
        with (self.output / entry['stdout']).open('xb') as stdout, (self.output / entry['stderr']).open('xb') as stderr:
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                       start_new_session=not WINDOWS)
            orphaned = False
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                if WINDOWS:
                    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], env=self.host_env,
                                   stdout=stderr, stderr=subprocess.STDOUT, timeout=15, check=False)
                else:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=10)
                raise
            finally:
                if not WINDOWS:
                    # Include the real AppImage runtime's forked AppRun child.
                    # This group was created exclusively for this one probe.
                    try:
                        os.killpg(process.pid, 0)
                    except ProcessLookupError:
                        pass
                    else:
                        orphaned = True
                        os.killpg(process.pid, signal.SIGTERM)
                        deadline = time.monotonic() + 5
                        while time.monotonic() < deadline:
                            try:
                                os.killpg(process.pid, 0)
                            except ProcessLookupError:
                                break
                            time.sleep(0.05)
                        else:
                            try:
                                os.killpg(process.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
        entry['returnCode'] = code
        entry['orphanedDescendants'] = orphaned
        result = json.loads((self.output / entry['stdout']).read_text(encoding='utf-8'))
        meta.require(code == 0 and result.get('passed') is True and not orphaned,
                     tag + ' did not complete/drain: ' + result.get('failure', 'see probe logs and descendant evidence'))
        return result

    def load_packages(self):
        suffix = 'windows-x64' if WINDOWS else 'linux-amd64'
        manifest_path = self.packages / ('package-manifest-' + suffix + '.json')
        manifest = meta.json_read(manifest_path)
        info_path = self.packages / ('build-info-' + suffix + '.json')
        self.info = meta.json_read(info_path)
        identity = meta.identity(self.info, development=True)
        meta.require({key: manifest.get(key) for key in identity} == identity and
                     manifest.get('developmentOnly') is self.info.get('developmentOnly') and
                     type(manifest.get('developmentOnly')) is bool, 'Package manifest and build-info identities differ')
        native = 'windows' if WINDOWS else 'linux'
        meta.require(self.info['platform'] == native, 'Native installation must run on the package platform')
        assets = manifest.get('assets', [])
        expected = {'installer', 'portable-zip'} if WINDOWS else {'deb', 'appimage'}
        meta.require(len(assets) == 2 and {asset.get('packageKind') for asset in assets} == expected,
                     'The full native package pair is required')
        self.assets = {}
        for asset in assets:
            meta.require(asset.get('buildInfo') == meta.descriptor(info_path), 'Package build-info bytes changed')
            kind = asset['packageKind']
            meta.verify_artifact(self.packages, asset, kind, self.info['version'])
            self.assets[kind] = asset
        self.package_bytes = [preserve_record(manifest_path), preserve_record(info_path)]
        self.package_bytes += [preserve_record(self.packages / asset['name']) for asset in assets]
        self.report['buildIdentity'] = identity
        self.report['developmentOnly'] = manifest['developmentOnly']
        self.report['packages'] = [{key: item[key] for key in ('name', 'size', 'sha256', 'packageKind')} for item in assets]

    def payload(self, kind):
        prefix = kind + '/'
        result = []
        for record in self.info['files']:
            if record['path'].startswith(prefix):
                value = dict(record); value['path'] = meta.safe_relative(value['path'][len(prefix):]); result.append(value)
        meta.require(result and len({entry['path'] for entry in result}) == len(result), 'The actual package inventory is absent or duplicated')
        return result

    def verify_payload(self, root, kind):
        for record in self.payload(kind):
            path = root / record['path']
            meta.require(path.is_file() and path.stat().st_size == record['size'] and meta.sha256(path) == record['sha256'],
                         'Installed payload differs from the actual package: ' + str(path))
            if 'linkTarget' in record:
                meta.require(path.is_symlink() and os.readlink(path) == record['linkTarget'], 'The package link identity changed')
            else:
                meta.require(not path.is_symlink(), 'A regular installed file became a link')

    def assert_removed(self, root, kind):
        for record in self.payload(kind):
            meta.require(not os.path.lexists(root / record['path']), 'A recorded program file survived native removal: ' + record['path'])

    def new_profile(self, directory):
        directory.mkdir(parents=True, exist_ok=True)
        values = {
            'common': {'language': 'en-US', 'autoStart': False},
            'lan': {'enabled': False},
            'toolPin': {'rect': {'width': 3.5, 'colorIndex': 4, 'future': {'retained': '保留'}}},
            'unrecognized-user-section': {'nested': ['原始配置', 1729, {'keep': True}]},
        }
        config = directory / 'config.json'
        with config.open('xb') as output:
            output.write(b'\xff\xfe' + (json.dumps(values, ensure_ascii=False, indent=2) + '\n').encode('utf-16-le'))
        unknown = directory / '用户数据 请保留' / 'notes.txt'
        unknown.parent.mkdir(); unknown.write_bytes('独立用户数据；安装、同版本重装和卸载都必须保留。\n'.encode())
        image = unknown.parent / '20300102-030405.png'
        self.probe_call(['--write-png', image], tag='write-real-png')
        video = unknown.parent / '20300102-030406.mp4'
        with video.open('xb') as target, self.video.open('rb') as source:
            shutil.copyfileobj(source, target, 1024 * 1024)
        return [preserve_record(path) for path in (config, unknown, image, video)]

    def history_fixtures(self):
        actual = self.probe_call(['--paths'], tag='read-real-user-paths')
        self.history = Path(actual['history'])
        if WINDOWS:
            self.programs = Path(actual['programs']) / 'huhucapture'
            meta.require(self.programs.is_absolute() and not os.path.lexists(self.programs),
                         'A pre-existing Start Menu group must not be replaced by this validation')
        meta.require(self.history.is_absolute() and not self.history.is_symlink(), 'Actual user history path is unavailable or linked')
        self.history.mkdir(parents=True, exist_ok=True)
        # Retain unrelated pre-existing history too. Nothing outside our exact
        # newly created fixture names is removed by the harness's cleanup.
        self.shared_preserved = [preserve_record(path) for path in self.history.rglob('*') if regular(path)]
        suffix = str(int(uuid.uuid4().hex[:7], 16) + 1)
        image = self.history / ('20990102-030405-' + suffix + '.png')
        video = self.history / ('20990102-030406-' + suffix + '.mp4')
        unknown = self.history / ('installation-user-note-' + suffix + '.txt')
        self.probe_call(['--write-png', image], tag='write-real-history-png')
        self.created_history.append(preserve_record(image))
        with video.open('xb') as target, self.video.open('rb') as source:
            shutil.copyfileobj(source, target, 1024 * 1024)
        self.created_history.append(preserve_record(video))
        with unknown.open('xb') as output:
            output.write(b'An unrelated history file must remain untouched.\n')
        self.created_history.append(preserve_record(unknown))
        self.shared_preserved += self.created_history
        self.report['historyBefore'] = self.shared_preserved

    def open_product(self, kind, launch, profile, stage):
        directory = self.output / (kind + '-' + stage); directory.mkdir()
        relative = {'installer': 'huhucapture.exe', 'portable-zip': 'huhucapture.exe',
                    'deb': 'opt/huhucapture/bin/huhucapture', 'appimage': 'usr/bin/huhucapture'}[kind]
        records = [value for value in self.payload(kind) if value['path'] == relative]
        meta.require(len(records) == 1, 'The real product executable is absent from the exact package inventory')
        arguments = ['--launch', launch, '--profile', profile, '--executable-sha256', records[0]['sha256'], '--artifacts', directory]
        if kind == 'appimage':
            arguments.append('--appimage')
        observed = self.probe_call(arguments, tag=kind + '-' + stage)
        meta.require(observed.get('nativeSelectionObserved') is True and observed.get('exitCode') == 0,
                     'The package did not complete a real selection-and-close GUI journey')
        controls = observed.get('controlRejections', [])
        meta.require(len(controls) == 4 and all(value.get('finished') is True and value.get('mappedWindow') is False and
                     value.get('exitCode') == 2 for value in controls),
                     'The real packaged controls did not reject invalid or unavailable requests without a product window')
        return {'evidence': str(directory.relative_to(self.output)), 'runtime': observed}

    def checkpoint(self, case, stage, protected):
        verify_preserved(protected + self.shared_preserved)
        case['preservation'].append({'stage': stage, 'unchanged': protected + self.shared_preserved})

    def windows_installer(self):
        meta.require(registry_location() is None and registry_uninstall() is None,
                     'A pre-existing per-user installation must not be replaced by this validation')
        root = self.output / '已安装 Windows 应用'
        root.mkdir(); self.native_root = root
        protected = self.new_profile(root)
        case = {'kind': 'installer', 'mechanism': 'real-per-user-NSIS', 'preservation': [], 'gui': []}
        self.report['cases'].append(case)
        setup = self.packages / self.assets['installer']['name']
        # NSIS explicitly requires unquoted /D= as the final parameter, even
        # when the directory contains spaces. No shell interprets this string.
        meta.require(not any(c in str(root) for c in ('"', '\r', '\n', '\0')), 'NSIS installation directory contains a command delimiter')
        command = subprocess.list2cmdline([str(setup), '/S']) + ' /D=' + str(root)
        self.native_started = True
        # A regular user file occupying a required payload directory exercises
        # the real SetOutPath/File failure, including the silent default path.
        nested = next((entry['path'] for entry in self.payload('installer') if '/' in entry['path']), None)
        meta.require(nested is not None, 'The real installer has no nested payload for its write-failure check')
        collision = root / nested.split('/')[0]
        with collision.open('xb') as outgoing:
            outgoing.write(b'This pre-existing user file must not become an installation directory.\n')
        collision_record = preserve_record(collision)
        code, _ = self.run(command, tag='nsis-directory-conflict', native_command_line=True, check=False, timeout=60)
        meta.require(code != 0 and registry_location() is None and registry_uninstall() is None,
                     'A real payload directory conflict must fail without registering a partial installation')
        self.checkpoint(case, 'directory-conflict-retained', protected + [collision_record])
        case['nativeFailures'] = [{'kind': 'payload-directory-conflict', 'returnCode': code,
                                   'registrationAbsent': True, 'userFileUnchanged': collision_record}]
        verify_preserved([collision_record]); collision.unlink()
        for stage in ('installed', 'same-package-reinstalled'):
            self.run(command, tag='nsis-' + stage, native_command_line=True)
            meta.require(registry_location() is not None and Path(registry_location()).resolve() == root,
                         'NSIS did not register the exact per-user installation location')
            registered = registry_uninstall()
            meta.require(registered and registered['DisplayName'] == 'huhucapture' and registered['DisplayVersion'] == self.info['version'] and
                         Path(registered['InstallLocation']).resolve() == root, 'Actual uninstall registration is missing or mismatched')
            self.verify_payload(root, 'installer'); meta.audit_tree(root, 'installer', development=True)
            meta.require(regular(self.programs / 'huhucapture.lnk') and regular(self.programs / 'Uninstall.lnk'),
                         'The real per-user Start Menu shortcuts are missing')
            shortcuts = []
            for name, executable in (('huhucapture.lnk', 'huhucapture.exe'), ('Uninstall.lnk', 'uninstall.exe')):
                observed = self.probe_call(['--shortcut', self.programs / name], tag='nsis-read-shortcut')['shortcut']
                meta.require(Path(observed['target']).resolve() == root / executable and observed['arguments'] == '',
                             'The actual Shell shortcut does not target this installed program')
                shortcuts.append({'name': name, **observed})
            case.setdefault('shortcuts', []).append({'stage': stage, 'actual': shortcuts})
            self.checkpoint(case, stage, protected)
            case['gui'].append(self.open_product('installer', root / 'huhucapture.exe', root, stage))
            self.checkpoint(case, stage + '-after-normal-close', protected)
            if stage == 'installed':
                self.windows_locked_payload(case, command, protected)
        self.remove_windows_installation()
        self.assert_removed(root, 'installer')
        meta.require(not os.path.lexists(root / 'uninstall.exe') and registry_location() is None and registry_uninstall() is None,
                     'The actual NSIS uninstall did not remove its executable and registration')
        meta.require(not os.path.lexists(self.programs / 'huhucapture.lnk') and not os.path.lexists(self.programs / 'Uninstall.lnk'),
                     'The actual uninstaller retained its registered Start Menu shortcuts')
        self.checkpoint(case, 'native-uninstalled', protected)
        meta.require(len(case['nativeFailures']) == 3, 'All real silent installer/uninstaller failures must be observed')
        case['passed'] = True

    def windows_locked_payload(self, case, install_command, protected):
        root = self.native_root
        dll = next((entry['path'] for entry in self.payload('installer') if entry['path'].lower().endswith('.dll')), None)
        meta.require(dll is not None, 'The actual installed DLL inventory is required for a native sharing violation')
        locked = preserve_record(root / dll)
        uninstaller = preserve_record(root / 'uninstall.exe')
        registered = registry_uninstall()
        with windows_read_lock(root / dll):
            for kind in ('reinstall-locked-dll', 'uninstall-locked-dll'):
                if kind.startswith('reinstall'):
                    code, _ = self.run(install_command, tag='nsis-' + kind, native_command_line=True,
                                       check=False, timeout=60)
                else:
                    code = self.run_windows_uninstaller(tag='nsis-' + kind, check=False)
                meta.require(code != 0 and registry_location() is not None and Path(registry_location()).resolve() == root and
                             registry_uninstall() == registered,
                             'A real sharing violation must fail and retain the registered retry path')
                self.checkpoint(case, kind, protected + [locked, uninstaller])
                case['nativeFailures'].append({'kind': kind, 'returnCode': code,
                    'lockedFileUnchanged': locked, 'uninstallerUnchanged': uninstaller,
                    'registrationUnchanged': registered, 'programFilesMayBePartiallyRemoved': True})
        # The next normal same-package installation must repair any explicitly
        # listed files removed before the locked file, and run the real GUI.

    def run_windows_uninstaller(self, *, tag, check=True):
        root = self.native_root
        current = registry_location()
        meta.require(current and Path(current).resolve() == root and regular(root / 'uninstall.exe'),
                     'Only the just-created registered uninstaller may be executed')
        original = preserve_record(root / 'uninstall.exe')
        directory = self.output / ('uninstaller-process-' + uuid.uuid4().hex)
        directory.mkdir()
        executable = directory / 'uninstall.exe'
        with (root / 'uninstall.exe').open('rb') as incoming, executable.open('xb') as outgoing:
            shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
        verify_preserved([original])
        meta.require(meta.descriptor(executable) == original['bytes'], 'The actual uninstaller copy differs from its registered bytes')
        meta.require(not any(c in str(root) for c in ('"', '\r', '\n', '\0')), 'NSIS removal directory contains a command delimiter')
        # _?= is unquoted and last, as required by NSIS. Running the byte-exact
        # copy outside INSTDIR allows removal of the installed uninstaller and
        # makes this the real removal process, not a temporary-copy launcher.
        command = subprocess.list2cmdline([str(executable), '/S']) + ' _?=' + str(root)
        code, _ = self.run(command, tag=tag, native_command_line=True, check=check, timeout=60)
        return code

    def remove_windows_installation(self):
        if self.native_removed or not self.native_root:
            return
        root = self.native_root
        self.run_windows_uninstaller(tag='nsis-uninstall')
        meta.require(registry_location() is None and registry_uninstall() is None and
                     not os.path.lexists(root / 'uninstall.exe'), 'The real NSIS removal process retained its registration or executable')
        self.native_removed = True

    def extract_zip(self, archive, root):
        expected = {value['path']: value for value in self.payload('portable-zip')}
        with zipfile.ZipFile(archive) as package:
            meta.require(package.testzip() is None and len(package.namelist()) == len(expected) and set(package.namelist()) == set(expected),
                         'Portable ZIP inventory/CRC differs from its real build receipt')
            for name, record in expected.items():
                meta.require('linkTarget' not in record, 'The Windows portable ZIP must contain regular files')
                target = root / meta.safe_relative(name); target.parent.mkdir(parents=True, exist_ok=True)
                meta.require(not target.is_symlink(), 'Portable extraction cannot replace a file link')
                with package.open(name) as incoming, target.open('wb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
        self.verify_payload(root, 'portable-zip')

    def portable_zip(self):
        root = self.output / '便携 Windows 应用'; root.mkdir()
        protected = self.new_profile(root)
        case = {'kind': 'portable-zip', 'mechanism': 'real-zip-extraction-and-inventory-file-removal',
                'nativeUninstaller': False, 'preservation': [], 'gui': []}
        self.report['cases'].append(case)
        package = self.packages / self.assets['portable-zip']['name']
        for stage in ('extracted', 'same-package-overlaid'):
            self.extract_zip(package, root); meta.audit_tree(root, 'portable-zip', development=True)
            self.checkpoint(case, stage, protected)
            case['gui'].append(self.open_product('portable-zip', root / 'huhucapture.exe', root, stage))
            self.checkpoint(case, stage + '-after-normal-close', protected)
        self.verify_payload(root, 'portable-zip')
        for entry in self.payload('portable-zip'):
            (root / entry['path']).unlink()
        for directory in sorted((path for path in root.rglob('*') if path.is_dir()), key=lambda path: len(path.parts), reverse=True):
            try:
                directory.rmdir()
            except OSError:
                pass  # Unlisted user-data directories deliberately remain.
        self.assert_removed(root, 'portable-zip'); self.checkpoint(case, 'program-files-removed', protected)
        case['passed'] = True

    def privileged(self, arguments, **kwargs):
        prefix = [] if os.geteuid() == 0 else ['sudo', '-n', '--']
        return self.run([*prefix, *arguments], **kwargs)

    def deb(self):
        code, status = self.run(['dpkg-query', '-W', '-f=${Status}\n', 'huhucapture'], tag='deb-before-status', check=False)
        meta.require(code == 1 or status.strip() == 'unknown ok not-installed', 'A pre-existing dpkg package must not be replaced by this validation')
        root = Path('/opt/huhucapture')
        meta.require(not os.path.lexists(root) and not os.path.lexists('/usr/bin/huhucapture'), 'A pre-existing native installation directory must be retained')
        profile = self.output / 'Linux 用户配置'; protected = self.new_profile(profile)
        case = {'kind': 'deb', 'mechanism': 'real-apt-dpkg-install-reinstall-remove-purge', 'preservation': [], 'gui': []}
        self.report['cases'].append(case)
        package = self.packages / self.assets['deb']['name']
        self.native_root = root; self.native_started = True
        for index, stage in enumerate(('installed', 'same-package-reinstalled')):
            options = ['--reinstall'] if index else []
            self.privileged(['apt-get', '-y', '--no-install-recommends', '--no-remove', *options, 'install', str(package)],
                            tag='deb-' + stage, timeout=300)
            _, status = self.run(['dpkg-query', '-W', '-f=${Status}\n${Version}\n${Architecture}\n', 'huhucapture'], tag='deb-' + stage + '-status')
            meta.require(status.splitlines() == ['install ok installed', self.info['version'], 'amd64'], 'The actual dpkg package status/version/architecture is incorrect')
            self.verify_payload(Path('/'), 'deb'); meta.audit_tree(root, 'deb', development=True)
            if not index:
                directory = root / ('用户数据-' + uuid.uuid4().hex[:12])
                self.privileged(['install', '-d', '-m', '0755', directory], tag='deb-preserved-directory')
                for record in protected:
                    source = Path(record['path']); target = directory / source.name
                    self.privileged(['install', '-m', '0644', source, target], tag='deb-preserved-file')
                    self.system_preserved.append(preserve_record(target))
            self.checkpoint(case, stage, protected + self.system_preserved)
            case['gui'].append(self.open_product('deb', Path('/usr/bin/huhucapture'), profile, stage))
            self.checkpoint(case, stage + '-after-normal-close', protected + self.system_preserved)
        self.privileged(['dpkg', '--remove', 'huhucapture'], tag='deb-remove')
        self.assert_removed(Path('/'), 'deb'); self.checkpoint(case, 'native-removed', protected + self.system_preserved)
        self.privileged(['dpkg', '--purge', 'huhucapture'], tag='deb-purge')
        self.native_removed = True
        self.assert_removed(Path('/'), 'deb'); self.checkpoint(case, 'native-purged', protected + self.system_preserved)
        case['passed'] = True

    def appimage(self):
        root = self.output / '便携 Linux 应用'; root.mkdir()
        profile = root / '用户配置'; protected = self.new_profile(profile)
        case = {'kind': 'appimage', 'mechanism': 'actual-type2-extract-and-run-file-replacement-removal',
                'nativeUninstaller': False, 'fuseMount': 'not-run', 'preservation': [], 'gui': []}
        self.report['cases'].append(case)
        source = self.packages / self.assets['appimage']['name']
        image = root / source.name
        for stage in ('copied', 'same-package-replaced'):
            temporary = root / ('.replacement-' + uuid.uuid4().hex)
            with source.open('rb') as incoming, temporary.open('xb') as outgoing:
                shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
            temporary.chmod(0o755)
            meta.require(meta.sha256(temporary) == self.assets['appimage']['sha256'], 'AppImage copy changed bytes')
            if image.exists():
                expected = {key: self.assets['appimage'][key] for key in ('name', 'size', 'sha256')}
                meta.require(regular(image) and meta.descriptor(image) == expected, 'Owned AppImage changed before replacement')
            os.replace(temporary, image)
            self.checkpoint(case, stage, protected)
            case['gui'].append(self.open_product('appimage', image, profile, stage))
            self.checkpoint(case, stage + '-after-normal-close', protected)
        meta.require(regular(image) and meta.sha256(image) == self.assets['appimage']['sha256'], 'The owned AppImage changed before removal')
        image.unlink(); self.checkpoint(case, 'appimage-file-removed', protected)
        case['passed'] = True

    def cleanup(self):
        if self.native_started and not self.native_removed:
            try:
                if WINDOWS:
                    self.remove_windows_installation()
                else:
                    self.privileged(['dpkg', '--remove', 'huhucapture'], tag='cleanup-native-package', check=False)
                self.report['cleanup'].append({'nativeRemovalAttempted': True})
            except Exception as error:
                self.report['cleanup'].append({'nativeRemovalFailed': str(error)})
                self.report['passed'] = False
        # Preserve evidence copies first, then remove only test-owned global
        # fixtures by exact path/hash. Never recursively delete a user folder.
        kept = self.output / 'preserved-history'; kept.mkdir(exist_ok=True)
        for record in self.created_history:
            try:
                verify_preserved([record]); path = Path(record['path'])
                with path.open('rb') as incoming, (kept / path.name).open('xb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
                path.unlink()
                self.report['cleanup'].append({'ownedHistoryFixtureRemoved': str(path)})
            except Exception as error:
                self.report['cleanup'].append({'retainedHistoryFixture': record['path'], 'reason': str(error)})
                self.report['passed'] = False
        if not WINDOWS:
            directories = set()
            for record in self.system_preserved:
                try:
                    verify_preserved([record]); path = Path(record['path']); directories.add(path.parent)
                    self.privileged(['rm', '--', path], tag='cleanup-owned-system-fixture')
                except Exception as error:
                    self.report['cleanup'].append({'retainedSystemFixture': record['path'], 'reason': str(error)})
                    self.report['passed'] = False
            for directory in directories:
                self.privileged(['rmdir', '--', directory], tag='cleanup-owned-system-directory', check=False)
            if self.native_root and self.native_removed:
                self.privileged(['rmdir', '--', self.native_root], tag='cleanup-empty-native-directory', check=False)

    def execute(self):
        try:
            self.load_packages(); self.history_fixtures()
            if WINDOWS:
                self.windows_installer(); self.portable_zip()
            else:
                self.deb(); self.appimage()
            verify_preserved(self.package_bytes + self.shared_preserved)
            self.report['passed'] = all(case.get('passed') is True for case in self.report['cases'])
        except Exception as error:
            self.report['failure'] = str(error)
        finally:
            try:
                self.cleanup()
            except Exception as error:
                self.report['cleanup'].append({'failure': str(error)})
                self.report['passed'] = False
            meta.json_write(self.output / 'installation-results.json', self.report)
        meta.require(self.report['passed'], self.report.get('failure', 'Native installation validation did not complete'))
        print('PASS actual native packages, same-package reinstall, GUI selection/close and preserved user data; cross-version upgrade not run.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', type=Path, required=True, help='Unmodified real platform package output and generated metadata')
    parser.add_argument('--probe', type=Path, required=True, help='Actual compiled installation_gui_probe executable')
    parser.add_argument('--video', type=Path, required=True, help='Real valid.mp4 from the fixed video_inspection_fixtures pipeline')
    parser.add_argument('--artifacts', type=Path, required=True, help='Empty owned evidence/work directory')
    args = parser.parse_args()
    try:
        meta.require(WINDOWS or sys.platform.startswith('linux'), 'This test only supports the native Windows/Linux package targets')
        Validation(args).execute()
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print('Actual package installation validation failed:', error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
