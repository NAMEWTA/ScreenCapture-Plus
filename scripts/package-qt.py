#!/usr/bin/env python3
"""Package the installed unified Qt product using native NSIS/CPack/linuxdeploy.

This script never publishes, tags, changes the product version, executes installers, or
replaces a previous output. Development bundles and release aggregation differ
explicitly; a bundle from dirty source cannot enter a release manifest.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import types
import zipfile
from version_source import read_version

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location('huhucapture_package_metadata', ROOT/'build/package/metadata.py')
meta = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(meta)
_execution_log = None
_file_lineage = None


def lineage_api():
    path = ROOT/'build/package/source_transforms.py'
    module = types.ModuleType('huhucapture_packaging_file_transformations')
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def copy_package_tree(source, destination):
    if _file_lineage is None:
        shutil.copytree(source,destination,symlinks=True)
    else:
        _file_lineage.copy_tree(source,destination)


def source_inventory_api():
    # One inventory format and digest algorithm with the source collector.
    # Metadata inventory follows symlink targets for updater accounting; the
    # source inventory intentionally records the link itself, including modes.
    path = ROOT/'scripts/create-source-bundle.py'
    module = types.ModuleType('huhucapture_source_inventory')
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def execution_api():
    path = ROOT/'build/package/source_receipts.py'
    module = types.ModuleType('huhucapture_packaging_execution')
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def runtime_source_api():
    path = ROOT/'build/package/appimage_runtime.py'
    module = types.ModuleType('huhucapture_actual_appimage_runtime')
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def select_source_runtime(directory, workspace, paths, execution):
    """Use only a complete actual producer; retain the official tool pins too."""
    api = runtime_source_api()
    directory = directory.resolve(strict=True)
    report, runtime, notice = api.verify_build(directory)
    destination = workspace/'source-runtime'
    destination.mkdir()
    chosen, license_file = destination/'runtime-x86_64', destination/'LICENSE.runtime-sources'
    copy_exclusive(runtime, chosen); chosen.chmod(0o755)
    copy_exclusive(notice, license_file)
    execution.retain_record(directory/'build-info.json')
    execution.retain_record(ROOT/'build/package/appimage_runtime.py')
    execution.retain_record(next(item['receipt']['path'] for item in report['receipts'] if item['id']=='appimage-runtime'))
    paths['appimageRuntime'], paths['appimageRuntimeLicense'] = chosen, license_file
    return {'schemaVersion':1, 'kind':'huhucapture-source-built-runtime-selection',
            'directory':str(directory), 'buildInfo':meta.descriptor(directory/'build-info.json'),
            'runtime':meta.descriptor(chosen), 'notice':meta.descriptor(license_file),
            'runtimeCommit':report['runtimeCommit'], 'runtimePath':'work/source-runtime/runtime-x86_64',
            'noticePath':'work/source-runtime/LICENSE.runtime-sources'}


def verify_runtime_selection(stage, selection, execution):
    if selection is None:
        return
    meta.require(selection.get('schemaVersion')==1 and selection.get('kind')=='huhucapture-source-built-runtime-selection' and
                 selection.get('runtimePath')=='work/source-runtime/runtime-x86_64' and
                 selection.get('noticePath')=='work/source-runtime/LICENSE.runtime-sources',
                 'Unknown or escaped source-built runtime selection')
    api = runtime_source_api()
    directory = Path(selection['directory'])
    report, runtime, notice = api.verify_build(directory)
    meta.require(selection['buildInfo']==meta.descriptor(directory/'build-info.json') and
                 selection['runtimeCommit']==report['runtimeCommit'],
                 'The selected runtime no longer matches its exact actual source-build report')
    _verify_runtime_packaging(stage, selection, execution, runtime, notice)


def _verify_runtime_packaging(stage, selection, execution, runtime, notice):
    """Check copied bytes and embedding after the original source build gate.

    This is only the packaging boundary. It cannot admit a runtime producer;
    verify_runtime_selection always independently verifies that build first.
    """
    meta.require(selection.get('runtimePath')=='work/source-runtime/runtime-x86_64' and
                 selection.get('noticePath')=='work/source-runtime/LICENSE.runtime-sources',
                 'Unknown or escaped source-built runtime selection')
    chosen, copied_notice = stage/selection['runtimePath'], stage/selection['noticePath']
    meta.require(meta.descriptor(chosen)==selection['runtime']==meta.descriptor(runtime) and
                 meta.descriptor(copied_notice)==selection['notice']==meta.descriptor(notice),
                 'The selected runtime/notice no longer matches its exact actual source-build output')
    index = selection['commandIndex']
    meta.require(type(index) is int and 0<=index<len(execution['commands']) and index<len(execution['executions']),
                 'Runtime embedding command is missing')
    command, actual = execution['commands'][index], execution['executions'][index]
    # verify_transformations has already checked the retained original producer
    # and command records. Their absolute argv is historical evidence: rebasing
    # it to this verification directory would change the claimed execution.
    producer = Path(execution['producer']['path'])
    meta.require(producer.is_absolute() and producer.parent.name=='records' and
                 producer.parent.parent.name=='executions', 'Unknown original packaging execution root')
    original_stage = producer.parent.parent.parent
    name = meta.safe_relative(selection['artifactName'])
    meta.require(Path(name).name==name, 'Runtime embedding artifact must be a top-level package')
    image = stage/'artifacts'/name
    lock = meta.json_read(ROOT/'build/package/dependencies.lock.json')['tools']['appimagetool']
    meta.require(actual.get('commandIndex')==index and actual.get('arguments')==command and
                 type(actual.get('exitCode')) is int and actual['exitCode']==0 and
                 command[1:]==['--runtime-file', str(original_stage/selection['runtimePath']),
                     '--comp', 'zstd', '--mksquashfs-opt', '-processors', '--mksquashfs-opt', '1',
                     str(original_stage/'payloads/appimage'), str(original_stage/'artifacts'/name)] and
                 all(actual['tool'][key]==lock[key] for key in ('size','sha256')) and
                 selection['embedding']==runtime_source_api().runtime_embedding(chosen,image),
                 'Final AppImage is not the exact source runtime with its recorded fixed-packager transformation')


def run(arguments, *, cwd=None, env=None, capture=False):
    if _execution_log is not None:
        # The shared producer recorder writes actual tool bytes, argv, exit
        # status and stdout/stderr even on failure. Only raw_record's successful
        # execution gate can later produce a completed transformation journal.
        result = _execution_log.run(arguments, cwd=cwd or Path.cwd(), env=env, capture=capture)
        return result[0] if capture else ''
    print('+', ' '.join(map(str, arguments)), flush=True)
    result = subprocess.run(list(map(str, arguments)), cwd=cwd, env=env, check=True,
                            text=True, encoding='utf-8', errors='replace',
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None)
    return result.stdout if capture else ''


def tool(name):
    value = shutil.which(name)
    meta.require(value is not None, 'Required native packaging tool is unavailable: '+name)
    return Path(value)


def copy_exclusive(source: Path, target: Path):
    meta.require(source.is_file() and not source.is_symlink(), 'Artifact source must be a regular file')
    with source.open('rb') as incoming, target.open('xb') as outgoing:
        shutil.copyfileobj(incoming, outgoing, 1024*1024)
    meta.require(meta.sha256(source) == meta.sha256(target), 'Artifact copy changed bytes')


def marker(prefix: Path, kind: str):
    platform, architecture, _, _ = meta.KINDS[kind]
    destination = prefix/('installation.json' if platform == 'windows' else 'bin/installation.json')
    meta.require(destination.is_file() and not destination.is_symlink(), 'Install tree did not produce its real package marker')
    # This tree is a new owned copy. Its marker describes the form being built,
    # rather than inheriting appimage/portable defaults for the DEB/installer.
    identity = meta.json_read(destination.with_name('build-identity.json'), 16384)
    meta.require(identity.get('platform') == platform and identity.get('architecture') == architecture,
                 'Copied package tree has a different native identity')
    if _file_lineage is None:
        destination.write_bytes(lineage_api().marker_bytes(identity,kind))
    else:
        _file_lineage.write_marker(destination,identity,kind)


def verify_source(build: dict, development: bool):
    meta.identity(build, development=development)
    version = read_version(ROOT)
    commit = run(['git','rev-parse','HEAD'], cwd=ROOT, capture=True).strip()
    dirty = bool(run(['git','status','--porcelain','--untracked-files=normal'], cwd=ROOT, capture=True).strip())
    meta.require(version == build['version'] and commit == build['sourceCommit'], 'Install receipt does not match the current source revision/version')
    meta.require(development or not dirty, 'Release packaging requires a clean exact source checkout')
    meta.require(build['dependencyLockSha256'] == meta.sha256(ROOT/'build/dependencies.lock.json') and
                 build['cargoLockSha256'] == meta.sha256(ROOT/'third_party/localsend/Cargo.lock'), 'Dependency locks changed since the app was configured')


def verify_tools(root: Path, destination: Path):
    lock = meta.json_read(ROOT/'build/package/dependencies.lock.json')
    paths, provenance = {}, []
    destination.mkdir()
    for key, specification in lock['tools'].items():
        meta.require(specification.get('sha256') and not specification.get('bootstrapOnly'), 'Unreviewed bootstrap packaging tool: '+key)
        source = root/key/specification['name']
        actual = meta.descriptor(source)
        meta.require(actual['size'] == specification['size'] and actual['sha256'] == specification['sha256'], 'Packager binary differs from its fixed official pin: '+key)
        target = destination/source.name; copy_exclusive(source, target); target.chmod(0o755)
        paths[key] = target
        for material, name in [('sourceArchive', 'source.tar.gz'), ('licenseRecord', specification['licenseFile'])]:
            related = root/key/name
            expected = specification[material]
            record = meta.descriptor(related)
            meta.require(record['size'] == expected['size'] and record['sha256'] == expected['sha256'],
                         'Packager source/license material differs from its reviewed pin: '+key)
            if key == 'appimageRuntime' and material == 'licenseRecord':
                paths['appimageRuntimeLicense'] = related
        provenance.append({'name':key, **specification})
    return paths, provenance


def zip_tree(root: Path, output: Path):
    expected = meta.inventory(root)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for entry in expected:
            meta.require('linkTarget' not in entry, 'Windows portable tree must not contain symbolic links')
            archive.write(root/entry['path'], entry['path'])
    with zipfile.ZipFile(output) as archive:
        meta.require(archive.testzip() is None and set(archive.namelist()) == {entry['path'] for entry in expected}, 'ZIP structure/CRC/inventory mismatch')
        for entry in expected:
            digest = hashlib.sha256()
            with archive.open(entry['path']) as stream:
                while chunk := stream.read(1024*1024): digest.update(chunk)
            meta.require(digest.hexdigest() == entry['sha256'] and archive.getinfo(entry['path']).file_size == entry['size'], 'ZIP changed a runtime file')
    meta.verify_inventory(root, expected)


def nsi_escape(value):
    meta.require('\n' not in str(value) and '\r' not in str(value), 'NSIS values must be single-line')
    return str(value).replace('$', '$$').replace('"', '$\\"')


def nsis_distribution(distribution: Path):
    # Official v311, Examples/makensis.nsi, installs a root launcher AND the
    # real Bin compiler. Source/build.cpp initializes NSISDIR from Bin's parent.
    source_commit = '7359413009afd4f0fff472d841fc2f2cc0e0a5f8'
    required = ('makensis.exe', 'Bin/makensis.exe', 'COPYING',
                'Include/MUI2.nsh', 'Include/LogicLib.nsh', 'Include/x64.nsh', 'Include/WinVer.nsh',
                'Stubs/uninst', 'Stubs/lzma_solid-x86-unicode',
                'Plugins/x86-unicode/nsDialogs.dll', 'Plugins/x86-unicode/System.dll',
                'Contrib/Modern UI 2/MUI2.nsh', 'Contrib/Language files/English.nsh')
    meta.require(distribution.is_dir() and not distribution.is_symlink(), 'NSIS extraction must be an owned directory')
    entries = sorted(distribution.rglob('*'), key=lambda path: path.relative_to(distribution).as_posix())
    meta.require(not any(path.is_symlink() for path in entries), 'NSIS extraction must not contain symbolic links')
    candidates = [path for path in entries if path.name.lower() == 'makensis.exe']
    records = []
    for path in candidates:
        meta.require(path.is_file(), 'NSIS compiler candidate is not a regular file: '+path.relative_to(distribution).as_posix())
        records.append({'path':path.relative_to(distribution).as_posix(),
                        'size':path.stat().st_size, 'sha256':meta.sha256(path)})
    layouts = []
    roots = []
    for path in candidates:
        if path.parent.name != 'Bin': continue
        root = path.parent.parent
        missing = [name for name in required if not (root/name).is_file() or not (root/name).stat().st_size]
        layouts.append({'root':root.relative_to(distribution).as_posix(), 'missing':missing})
        if not missing: roots.append(root)
    print('NSIS distribution inspection: '+json.dumps({'candidates':records, 'layouts':layouts}, sort_keys=True), flush=True)
    meta.require(len(roots) == 1, 'The official NSIS archive must contain exactly one complete distribution root')
    root = roots[0]
    compiler = root/'Bin/makensis.exe'
    meta.require(set(candidates) == {root/'makensis.exe', compiler},
                 'Unexpected compiler candidates outside the official NSIS launcher/Bin pair')
    selection = {'distributionRoot':root.relative_to(distribution).as_posix(),
                 'compilerPath':compiler.relative_to(distribution).as_posix(),
                 'compilerCandidates':records, 'layoutSourceCommit':source_commit}
    print('NSIS compiler selection: '+json.dumps(selection, sort_keys=True), flush=True)
    return root, compiler, selection


def prepare_nsis(nsis_archive: Path, workspace: Path):
    specification = meta.json_read(ROOT/'build/package/dependencies.lock.json')['windowsTools']['nsis']
    meta.require(nsis_archive is not None and nsis_archive.is_file() and
                 meta.sha256(nsis_archive) == specification['sha256'],
                 'Supply the exact reviewed NSIS distribution; arbitrary PATH compilers are not packaging inputs')
    distribution = workspace/'nsis'; distribution.mkdir()
    run([tool('7z'),'x','-y','-o'+str(distribution),nsis_archive])
    root, compiler, selection = nsis_distribution(distribution)
    # Ignore ambient NSIS locations even on Windows, where environment names
    # are case-insensitive. /NOCONFIG also excludes per-user nsisconf.nsh.
    environment = {key:value for key, value in os.environ.copy().items()
                   if key.upper() not in {'NSISDIR', 'NSISCONFDIR'}}
    environment['NSISDIR'] = str(root.resolve())
    compiler_version = run([compiler, '/NOCONFIG', '/VERSION'], env=environment, capture=True).strip()
    meta.require(compiler_version == 'v3.11', 'Use the reviewed NSIS 3.11 compiler, not an unspecified PATH version')
    provenance = {'name':'nsis', 'version':compiler_version, 'sha256':meta.sha256(compiler),
                  'source':specification['source'], 'distributionSha256':specification['sha256'], **selection}
    return compiler, environment, provenance


def windows_installer_script(prefix: Path, output: Path, version: str, records):
    install, remove, directories = [], [], set()
    for entry in records:
        relative = Path(entry['path'])
        meta.require('linkTarget' not in entry, 'Installer input cannot contain links')
        parent = str(relative.parent).replace('/', '\\')
        output_dir = '$INSTDIR' if parent == '.' else '$INSTDIR\\'+nsi_escape(parent)
        # v311 Source/script.cpp rejects a quote immediately after /oname=.
        # Quote the whole token, as Docs/src/basic.but documents:
        # https://github.com/NSIS-Dev/nsis/blob/7359413009afd4f0fff472d841fc2f2cc0e0a5f8/Docs/src/basic.but
        target = '$INSTDIR\\'+nsi_escape(str(relative).replace('/', '\\'))
        install.extend(['    StrCpy $OperationTarget "'+output_dir+'"', '    ClearErrors',
                        '    SetOutPath "'+output_dir+'"', '    IfErrors install_failed',
                        '    StrCpy $OperationTarget "'+target+'"',
                        '    File "/oname='+nsi_escape(relative.name)+'" "'+nsi_escape(prefix/relative)+'"',
                        '    IfErrors install_failed'])
        remove.extend(['    StrCpy $OperationTarget "'+target+'"', '    ClearErrors',
                       '    Delete "$OperationTarget"', '    IfErrors uninstall_failed',
                       '    IfFileExists "$OperationTarget" uninstall_failed'])
        for ancestor in relative.parents:
            if ancestor != Path('.'): directories.add(str(ancestor).replace('/', '\\'))
    remove += ['    RMDir "$INSTDIR\\'+nsi_escape(value)+'"' for value in sorted(directories, key=lambda x:(x.count('\\'),x), reverse=True)]
    replacements = {'OUTPUT':nsi_escape(output), 'VERSION':version, 'ICON':nsi_escape(ROOT/'resources/logo.ico'),
                    'LICENSE':nsi_escape(ROOT/'LICENSE'), 'INSTALL_FILES':'\n'.join(install), 'REMOVE_FILES':'\n'.join(remove)}
    source = (ROOT/'build/package/user-installer.nsi.in').read_text()
    for key, value in replacements.items(): source = source.replace('@'+key+'@', value)
    meta.require(not re.search(r'@[A-Z_]+@', source), 'Unexpanded installer template token')
    return source


def windows_installer(prefix: Path, output: Path, workspace: Path, version: str, nsis_archive: Path):
    compiler, environment, provenance = prepare_nsis(nsis_archive, workspace)
    records = meta.inventory(prefix)
    source = windows_installer_script(prefix, output, version, records)
    script = workspace/'installer.nsi'; script.write_text(source, encoding='utf-8-sig')
    run([compiler, '/NOCONFIG', '/INPUTCHARSET', 'UTF8', script], env=environment)
    meta.verify_inventory(prefix, records)
    meta.require(output.is_file() and output.stat().st_size > 0, 'NSIS did not create the complete installer')
    return {**provenance, 'templateSha256':meta.sha256(ROOT/'build/package/user-installer.nsi.in')}


def linux_environment(qmake: Path):
    environment = os.environ.copy()
    environment.update({'APPIMAGE_EXTRACT_AND_RUN':'1', 'QMAKE':str(qmake), 'NO_STRIP':'1', 'ARCH':'x86_64'})
    plugins = Path(run([qmake,'-query','QT_INSTALL_PLUGINS'], capture=True).strip())/'platforms'
    wayland = sorted(path.name for path in plugins.glob('libqwayland*.so') if path.is_file())
    meta.require(wayland, 'The selected Qt SDK lacks actual Wayland platform plugins')
    environment['EXTRA_PLATFORM_PLUGINS'] = ';'.join(wayland)
    return environment


def verify_windows_symbols(symbols: Path, executable: Path, build: dict):
    meta.require(symbols is not None, 'Supply the same build Symbols install component')
    record = meta.json_read(symbols/'build-ids.json', 16384)
    meta.require(record.get('schemaVersion') == 1 and record.get('sourceCommit') == build['sourceCommit'] and
                 record.get('version') == build['version'] and record.get('configuration') == build['configuration'] and
                 record.get('executableSha256') == meta.sha256(executable),
                 'Symbols were not produced with this exact executable/configuration')
    meta.require(record.get('pdb') == meta.descriptor(symbols/'huhucapture.pdb'), 'Installed PDB bytes changed after the native link receipt')


def prepare_appdir(installed: Path, appdir: Path, tools: dict, qmake: Path):
    copy_package_tree(installed, appdir/'usr')
    marker(appdir/'usr', 'appimage')
    environment = linux_environment(qmake)
    # A directly referenced custom AppRun keeps the original long-lived AppImage
    # path in APPIMAGE and sets only this bundle's PipeWire client search paths.
    exclusions = ['libGL*.so*','libEGL*.so*','libvulkan.so*','libvulkan_*.so*',
                  'libdrm.so*','libdrm_*.so*','libnvidia*.so*','libcuda*.so*','libgbm.so*','libglapi.so*']
    arguments = [tools['linuxdeploy'], '--appdir', appdir, '--plugin', 'qt', '--deploy-deps-only', appdir/'usr',
                 '--desktop-file', appdir/'usr/share/applications/io.github.NAMEWTA.huhucapture.desktop',
                 '--icon-file', appdir/'usr/share/icons/hicolor/scalable/apps/io.github.NAMEWTA.huhucapture.svg',
                 '--custom-apprun', ROOT/'build/package/AppRun']
    for exclusion in exclusions: arguments += ['--exclude-library', exclusion]
    if _file_lineage is not None:
        environment=_file_lineage.patchelf_environment(tools['linuxdeploy'],environment,run)
    run(arguments, env=environment)
    if _file_lineage is not None:
        _file_lineage.collect_patchelf()
        environment.pop('PATCHELF')
        environment.pop('HUHUCAPTURE_PATCHELF_RECORDING')
    runtime_license = appdir/'usr/share/huhucapture/licenses/appimage-runtime'
    runtime_license.mkdir(parents=True, exist_ok=True)
    copy_exclusive(tools['appimageRuntimeLicense'],runtime_license/'LICENSE')
    meta.audit_tree(appdir/'usr', 'appimage', development=True)
    return environment


def split_debug(prefix: Path, destination: Path):
    destination.mkdir()
    objcopy, readelf = tool('objcopy'), tool('readelf')
    records = []
    executable = prefix/'bin/huhucapture'
    for path in sorted(prefix.rglob('*')):
        if not path.is_file() or path.is_symlink(): continue
        with path.open('rb') as stream:
            if stream.read(4) != b'\x7fELF': continue
        relative = path.relative_to(prefix)
        sections = run([readelf,'--wide','--sections',path], capture=True)
        notes = run([readelf,'--notes',path], capture=True)
        identity = re.search(r'Build ID: ([0-9a-f]+)', notes)
        if '.debug_info' not in sections:
            meta.require(path != executable, 'The actual app has no debug data; build RelWithDebInfo for matching symbols')
            continue
        meta.require(identity is not None, 'ELF with debug data has no build-id: '+str(relative))
        original = meta.descriptor(path)
        debug = destination/relative.with_name(relative.name+'.debug'); debug.parent.mkdir(parents=True,exist_ok=True)
        debug_command=len(_execution_log.commands) if _execution_log is not None else None
        run([objcopy,'--only-keep-debug',path,debug])
        if _file_lineage is None:
            run([objcopy,'--strip-debug',path])
            run([objcopy,'--add-gnu-debuglink='+str(debug),path])
        else:
            _file_lineage.mutation(path,[objcopy,'--strip-debug',path],run,operation='objcopy-strip-debug')
            _file_lineage.mutation(path,[objcopy,'--add-gnu-debuglink='+str(debug),path],run,
                                  operation='objcopy-add-debuglink',debugCommandIndex=debug_command,
                                  debugInput={'path':str(debug),**{key:original_value for key,original_value
                                      in meta.descriptor(debug).items() if key in ('size','sha256')}})
        records.append({'path':relative.as_posix(),'buildId':identity.group(1), 'input':original,
                        'runtimeSha256':meta.sha256(path),
                        'debug':debug.relative_to(destination).as_posix(), 'debugSha256':meta.sha256(debug)})
    meta.require(any(item['path'] == 'bin/huhucapture' for item in records), 'Missing matching app debug symbols')
    meta.json_write(destination/'build-ids.json', {'schemaVersion':1,'files':records})
    return records


def tar_zstd(prefix: Path, output: Path, workspace: Path):
    archive = workspace/(output.name+'.tar')
    with tarfile.open(archive, 'x') as tar:
        for path in sorted(prefix.iterdir()): tar.add(path, arcname=path.name, recursive=True)
    run([tool('zstd'), '-q', '-T1', '-19', str(archive), '-o', str(output)])
    meta.require(output.is_file() and output.stat().st_size > 0, 'No complete compressed symbol archive')


def cmake_quote(path):
    value = str(path).replace('\\', '/')
    meta.require(']=]' not in value and '\0' not in value, 'Invalid CPack path')
    return '[=['+value+']=]'


def linux_deb(appdir: Path, output: Path, workspace: Path, version: str, root: Path):
    # CPack otherwise falls back to only explicit Depends when shlibdeps is
    # absent. This pipeline requires the actual ELF dependency calculation.
    tool('dpkg-shlibdeps'); tool('file')
    (root/'opt').mkdir(parents=True)
    copy_package_tree(appdir/'usr', root/'opt/huhucapture')
    marker(root/'opt/huhucapture','deb')
    (root/'usr/bin').mkdir(parents=True)
    shutil.copy2(ROOT/'build/package/deb-launcher',root/'usr/bin/huhucapture')
    (root/'usr/bin/huhucapture').chmod(0o755)
    for folder in ('applications','icons'):
        copy_package_tree(root/'opt/huhucapture/share'/folder,root/'usr/share'/folder)
    # The control action must resolve to the primary's exact persistent binary.
    # Its IPC-only child needs no PipeWire setup; ordinary startup retains the
    # existing /usr/bin launcher. Never relax identity checks for arbitrary
    # shell wrappers or hard-code this DEB path in the shared AppImage entry.
    for base in (root/'opt/huhucapture/share', root/'usr/share'):
        desktop = base/'applications/io.github.NAMEWTA.huhucapture.desktop'
        if _file_lineage is None:
            desktop.write_bytes(lineage_api().desktop_bytes(desktop.read_bytes()))
        else:
            _file_lineage.write_desktop(desktop)
    config = workspace/'CPackDEBConfig.cmake'
    # CPack places its _CPack_Packages scratch tree beside the generated DEB.
    # Keep that actual build output in the workspace and promote only the
    # declared package into the immutable artifact directory.
    cpack_output = workspace/'cpack-output'
    cpack_output.mkdir()
    settings = {
        'CPACK_GENERATOR':'DEB', 'CPACK_PACKAGE_NAME':'huhucapture', 'CPACK_PACKAGE_VERSION':version,
        'CPACK_PACKAGE_VENDOR':'NAMEWTA', 'CPACK_PACKAGE_CONTACT':'https://github.com/NAMEWTA/huhucapture/issues',
        'CPACK_PACKAGE_DESCRIPTION_SUMMARY':'Screen capture, annotation, recording and LAN sharing with Qt 6',
        'CPACK_PACKAGE_DESCRIPTION':'Complete Qt 6 desktop application for screen capture, annotation, recording, scrolling capture and LAN sharing.',
        'CPACK_PACKAGE_FILE_NAME':output.stem, 'CPACK_DEBIAN_FILE_NAME':output.name,
        'CPACK_PACKAGE_DIRECTORY':str(cpack_output), 'CPACK_PACKAGING_INSTALL_PREFIX':'/',
        'CPACK_DEBIAN_PACKAGE_ARCHITECTURE':'amd64', 'CPACK_DEBIAN_PACKAGE_SECTION':'graphics',
        'CPACK_DEBIAN_PACKAGE_SHLIBDEPS':'ON', 'CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS':str(root/'opt/huhucapture/lib'),
        'CPACK_DEBIAN_PACKAGE_DEPENDS':'xdg-desktop-portal, ca-certificates', 'CPACK_DEBIAN_PACKAGE_RECOMMENDS':'pipewire',
        'CPACK_DEBIAN_PACKAGE_HOMEPAGE':'https://github.com/NAMEWTA/huhucapture',
        'CPACK_DEBIAN_COMPRESSION_TYPE':'xz', 'CPACK_SET_DESTDIR':'OFF',
    }
    config.write_text('\n'.join('set('+key+' '+cmake_quote(value)+')' for key,value in settings.items())+
                      '\nset(CPACK_INSTALLED_DIRECTORIES '+cmake_quote(root)+' "/")\n')
    if _execution_log is not None:
        _execution_log.retain_record(config)
    run([tool('cpack'),'--config',config,'-G','DEB'],cwd=workspace)
    copy_exclusive(cpack_output/output.name,output)
    dpkg = tool('dpkg-deb')
    fields = run([dpkg,'--field',output,'Package','Version','Architecture'],capture=True)
    meta.require('Package: huhucapture' in fields and 'Version: '+version in fields and 'Architecture: amd64' in fields,
                 'Generated DEB control identity is wrong')
    depends = run([dpkg,'--field',output,'Depends'],capture=True).strip()
    # CPack intentionally uses --ignore-missing-info for private bundled libs.
    # Without a real host dpkg symbols database that also silently drops every
    # system ABI requirement. A syntactically valid DEB is not sufficient.
    for dependency in ('libc6', 'libstdc++6', 'libgcc-s1'):
        meta.require(re.search(r'(?:^|[,|])\s*'+re.escape(dependency)+r'(?::(?:amd64|any))?\s*\(>=\s*[^)]+\)', depends),
                     'Generated DEB lacks the resolved host ABI dependency '+dependency+
                     '; package on the actual baseline with its dpkg package/symbol database')
    extracted = workspace/'deb-verified'; run([dpkg,'--extract',output,extracted])
    meta.require(meta.inventory(root) == meta.inventory(extracted), 'DEB extraction differs from the real staged package')
    meta.audit_tree(extracted/'opt/huhucapture','deb',development=True)
    # Retain the actual CPack-generated control bytes too. They belong to the
    # final source mapping, not an inferred reconstruction from package fields.
    run([dpkg,'--control',output,root/'DEBIAN'])
    return root


def verify_deb_control(stage, prepared, execution):
    """Bind control files to the original CPack command and replay extraction."""
    records = [item for item in execution['records'] if Path(item['inputPath']).name == 'CPackDEBConfig.cmake']
    if not records:
        return {}
    meta.require(len(records) == 1, 'Only one original CPack DEB configuration is expected')
    record = records[0]
    original = Path(record['inputPath'])
    meta.require(original.is_absolute() and original.parent.name == 'work',
                 'The original CPack DEB configuration must belong to the preparation workspace')
    retained = stage / 'executions/records' / Path(record['path']).name
    meta.require(execution_api().identity(retained) == {key: record[key] for key in ('size', 'sha256')},
                 'Original CPack DEB configuration changed after packaging')
    original_stage = original.parents[1]
    package = next(item for item in prepared['packages'] if item['packageKind'] == 'deb')
    archive = stage / 'artifacts' / package['name']
    original_archive = original_stage / 'artifacts' / archive.name
    original_control = original_stage / 'payloads/deb/DEBIAN'
    commands = execution['commands']
    builds = [i for i, args in enumerate(commands) if Path(args[0]).name == 'cpack' and
              args[1:] == ['--config', str(original), '-G', 'DEB']]
    extracts = [i for i, args in enumerate(commands) if Path(args[0]).name == 'dpkg-deb' and
                args[1:] == ['--control', str(original_archive), str(original_control)]]
    meta.require(len(builds) == len(extracts) == 1 and builds[0] < extracts[0],
                 'Original CPack and dpkg-deb commands do not describe the actual DEB control archive')
    for index in (builds[0], extracts[0]):
        program = commands[index][0]
        recorded = execution['toolchain'][program]
        meta.require(execution_api().identity(Path(recorded['path'])) == {key: recorded[key] for key in ('size', 'sha256')},
                     'The original Debian packaging tool has changed: ' + program)
    dpkg = execution['toolchain'][commands[extracts[0]][0]]['path']
    with tempfile.TemporaryDirectory(prefix='verified-deb-control-') as temporary:
        extracted = Path(temporary) / 'DEBIAN'
        run([dpkg, '--control', archive, extracted])
        control = meta.inventory(extracted, 'deb/DEBIAN/')
    meta.require(control == meta.inventory(stage / 'payloads/deb/DEBIAN', 'deb/DEBIAN/') and
                 {item['path'] for item in control} == {'deb/DEBIAN/control', 'deb/DEBIAN/md5sums'} and
                 all('linkTarget' not in item for item in control),
                 'Actual CPack-generated Debian control files differ from the frozen deployment')
    return {item['path']: item for item in control}


def package_products(platform):
    if platform == 'windows':
        return [{'root':kind, 'executable':'huhucapture.exe', 'packageKind':kind}
                for kind in ('installer','portable-zip')]
    return [{'root':'appimage/usr', 'executable':'bin/huhucapture', 'packageKind':'appimage'},
            {'root':'deb/opt/huhucapture', 'executable':'bin/huhucapture', 'packageKind':'deb'}]


def empty_output(path: Path):
    meta.require(not path.is_symlink(), 'Output directory must not be a link')
    path.mkdir(parents=True, exist_ok=True)
    meta.require(not any(path.iterdir()), 'Use an empty output directory; existing results are retained')


def prepare(args):
    """Create and retain the actual final bytes before corresponding-source closure.

    A prepared release candidate has no release manifest. Its explicit intent,
    clean build identity and frozen payloads are verified again by finalize.
    Development preparation can only produce development metadata afterward.
    """
    global _execution_log, _file_lineage
    installed = args.installed.resolve(); stage = args.stage.resolve()
    meta.require(installed.is_dir(), 'The actual CMake-installed tree is required')
    meta.require(not stage.is_relative_to(installed) and not installed.is_relative_to(stage),
                 'Package staging and installed input must be separate directories')
    empty_output(stage)
    platform = 'windows' if sys.platform == 'win32' else 'linux'
    kind = 'portable-zip' if platform == 'windows' else 'appimage'
    build, _ = meta.audit_tree(installed,kind,development=args.development)
    verify_source(build,args.development)
    meta.require(build['platform'] == platform, 'Package on the actual target platform; cross-host fake assets are forbidden')
    version = build['version']
    collector = source_inventory_api()
    original = collector.inventory(installed)
    meta.json_write(stage/'installed-input-inventory.json',original)
    payloads=stage/'payloads'; payloads.mkdir()
    output=stage/'artifacts'; output.mkdir()
    workspace=stage/'work'; workspace.mkdir()
    packages, extra_assets, transforms, deployment_stages = [], [], [], []
    runtime_selection = None
    execution=execution_api().ExecutionLog(stage/'executions',platform,Path(__file__).resolve())
    execution.retain_record(ROOT/'build/package/dependencies.lock.json')
    execution.retain_record(stage/'installed-input-inventory.json')
    lineage=lineage_api().Recorder(stage,installed,execution,collector)
    _execution_log=execution
    _file_lineage=lineage
    try:
        if platform == 'windows':
            meta.require(getattr(args,'appimage_runtime_build',None) is None, 'A Linux runtime source build cannot select Windows packaging inputs')
            portable=payloads/'portable-zip'; copy_package_tree(installed,portable); marker(portable,'portable-zip')
            installer=payloads/'installer'; copy_package_tree(installed,installer); marker(installer,'installer')
            meta.audit_tree(portable,'portable-zip',development=args.development)
            meta.audit_tree(installer,'installer',development=args.development)
            zip_path=output/meta.asset_name(version,'portable-zip'); zip_tree(portable,zip_path)
            installer_path=output/meta.asset_name(version,'installer')
            compiler=windows_installer(installer,installer_path,workspace,version,args.nsis_installer)
            verify_windows_symbols(args.symbols,portable/'huhucapture.exe',build)
            symbols=output/f'huhucapture-v{version}-windows-x64-symbols.zip'; zip_tree(args.symbols,symbols)
            extra_assets.append(meta.descriptor(symbols))
            for item,artifact in [('portable-zip',zip_path),('installer',installer_path)]:
                packages.append({**meta.descriptor(artifact),'packageKind':item,'contentRoot':item})
            build['packagingTools']=[compiler]
        else:
            meta.require(args.tools is not None and args.qmake is not None, 'Linux requires verified pinned packaging tools and the matching Qt qmake')
            paths,provenance=verify_tools(args.tools,workspace/'tools')
            if getattr(args,'appimage_runtime_build',None) is not None:
                runtime_selection=select_source_runtime(args.appimage_runtime_build,workspace,paths,execution)
                provenance.append({'name':'appimageRuntimeSourceBuild',
                    'runtimeCommit':runtime_selection['runtimeCommit'],
                    'runtime':runtime_selection['runtime'], 'buildInfo':runtime_selection['buildInfo']})
            qmake=args.qmake.resolve()
            meta.require(run([qmake,'-query','QT_VERSION'],capture=True).strip() == build['qtVersion'], 'Packager Qt SDK does not match the actual binary')
            appdir=payloads/'appimage'; first_command=len(execution.commands)
            environment=prepare_appdir(installed,appdir,paths,qmake)
            deployed = collector.inventory(appdir)
            meta.json_write(stage/'linuxdeploy-output-inventory.json',deployed)
            deployment_stages.append({'operation':'native-deployment',
                'commands':list(range(first_command,len(execution.commands))),
                'inventory':meta.descriptor(stage/'linuxdeploy-output-inventory.json'),
                'path':'linuxdeploy-output-inventory.json','root':'appimage'})
            first_command=len(execution.commands)
            symbols=workspace/'debug'; transforms=split_debug(appdir/'usr',symbols)
            meta.json_write(stage/'linux-stripped-inventory.json',collector.inventory(appdir))
            deployment_stages.append({'operation':'split-debug',
                'commands':list(range(first_command,len(execution.commands))),
                'inventory':meta.descriptor(stage/'linux-stripped-inventory.json'),
                'path':'linux-stripped-inventory.json','root':'appimage'})
            symbol_path=output/f'huhucapture-v{version}-linux-amd64-debug.tar.zst';tar_zstd(symbols,symbol_path,workspace)
            extra_assets.append(meta.descriptor(symbol_path))
            deb_path=output/meta.asset_name(version,'deb'); linux_deb(appdir,deb_path,workspace,version,payloads/'deb')
            image_path=output/meta.asset_name(version,'appimage')
            runtime_command=len(execution.commands)
            run([paths['appimagetool'],'--runtime-file',paths['appimageRuntime'],'--comp','zstd','--mksquashfs-opt','-processors','--mksquashfs-opt','1',appdir,image_path],env=environment)
            if runtime_selection is not None:
                runtime_selection.update(commandIndex=runtime_command,artifactName=image_path.name,
                    embedding=runtime_source_api().runtime_embedding(paths['appimageRuntime'],image_path))
            meta.require(meta.machine(image_path) == 'linux', 'AppImage has the wrong outer runtime architecture')
            with image_path.open('rb') as stream:
                stream.seek(8); meta.require(stream.read(3)==b'AI\x02','AppImage type-2 magic is missing')
            extracted=workspace/'appimage-verified'; extracted.mkdir()
            run([image_path,'--appimage-extract'],cwd=extracted,env=environment)
            extracted=extracted/'squashfs-root'
            meta.require(meta.inventory(appdir) == meta.inventory(extracted), 'AppImage extraction differs from the actual staged payload')
            meta.audit_tree(extracted/'usr','appimage',development=args.development)
            for item,artifact in [('deb',deb_path),('appimage',image_path)]:
                packages.append({**meta.descriptor(artifact),'packageKind':item,'contentRoot':item})
            build['packagingTools']=provenance
    finally:
        _execution_log=None
        _file_lineage=None
    products=package_products(platform)
    for product in products:
        actual,_=meta.audit_tree(payloads/product['root'],product['packageKind'],development=args.development)
        meta.require(actual == {key:value for key,value in build.items() if key != 'packagingTools'},
                     'Prepared product identities disagree')
    meta.require(collector.inventory(installed)==original, 'Packaging changed the original CMake-installed input')
    files=collector.inventory(payloads)
    meta.json_write(stage/'source-deployment-inventory.json',files)
    file_transformations=lineage.finish({key:value for key,value in build.items() if key != 'packagingTools'},files)
    execution_record=execution.raw_record(
        {'buildIdentity':{key:value for key,value in build.items() if key != 'packagingTools'},
         'installedInput':original},
        {'deploymentTreeSha256':collector.tree_digest(files),'files':files,'products':products,
         'artifacts':[*packages,*extra_assets],'deploymentStages':deployment_stages,'fileTransformations':file_transformations,
         'runtimeSelection':runtime_selection})
    meta.json_write(stage/'transformations.json',{'schemaVersion':1, 'kind':'huhucapture-package-transformations',
        'buildIdentity':{key:value for key,value in build.items() if key != 'packagingTools'},
        'installedInput':meta.descriptor(stage/'installed-input-inventory.json'),
        'packagerSha256':meta.sha256(Path(__file__)), 'commands':execution.commands, 'debugTransforms':transforms,
        'executionRecord':{**meta.descriptor(execution_record),'path':'executions/build-execution.json'},
        'executionInventory':collector.inventory(stage/'executions'),
        'deploymentStages':deployment_stages,
        'fileTransformations':file_transformations,
        'runtimeSelection':runtime_selection,
        'deploymentTreeSha256':collector.tree_digest(files), 'products':products,
        'artifacts':[*packages,*extra_assets]})
    prepared={'schemaVersion':1,'kind':'huhucapture-prepared-packages',
              'intent':'development' if args.development else 'release','build':build,
              'minimumSystem':args.minimum_system,'products':products,
              'runtimeSelection':runtime_selection,
              'packages':packages,'additionalAssets':extra_assets,
              'deployment':{'files':files,'treeSha256':collector.tree_digest(files),'products':products},
              'transformations':meta.descriptor(stage/'transformations.json')}
    meta.json_write(stage/'prepared.json',prepared)
    print('Prepared actual immutable payloads and candidate packages; corresponding sources must bind this final stage before release finalization.')
    return prepared


def verify_transformations(stage: Path, platform: str):
    """Verify original packaging execution, including retained intermediate bytes.

    These records prove packaging observations, not the upstream source lineage
    of a changed binary. Final per-file source mappings remain independently
    checked by the source collector.
    """
    collector=source_inventory_api()
    transformations=meta.json_read(stage/'transformations.json',32*1024*1024)
    meta.require(transformations.get('schemaVersion')==1 and
                 transformations.get('kind')=='huhucapture-package-transformations',
                 'Unknown packaging transformation record')
    stages=transformations.get('deploymentStages',[])
    meta.require(isinstance(stages,list) and all(isinstance(item,dict) for item in stages),
                 'Invalid native deployment/debug stage records')
    meta.require([item.get('operation') for item in stages] ==
                 (['native-deployment','split-debug'] if platform=='linux' else []),
                 'Original native deployment/debug stage inventories are missing')
    previous=-1
    for item in stages:
        expected_path={'native-deployment':'linuxdeploy-output-inventory.json',
                       'split-debug':'linux-stripped-inventory.json'}[item['operation']]
        meta.require(item.get('path')==expected_path and item.get('root')=='appimage' and
                     meta.descriptor(stage/expected_path)==item.get('inventory') and
                     isinstance(item.get('commands'),list) and item['commands'] and
                     all(type(index) is int and 0<=index<len(transformations['commands']) for index in item['commands']) and
                     item['commands']==list(range(item['commands'][0],item['commands'][-1]+1)) and
                     item['commands'][0]>previous,
                     'Native deployment/debug stage identity or executed-command range changed')
        previous=item['commands'][-1]
    execution_record=transformations.get('executionRecord',{})
    meta.require(execution_record.get('path')=='executions/build-execution.json' and
                 meta.descriptor(stage/'executions/build-execution.json')==
                    {key:value for key,value in execution_record.items() if key != 'path'} and
                 collector.inventory(stage/'executions')==transformations.get('executionInventory'),
                 'Actual packaging execution records or original logs changed')
    recorded=meta.json_read(stage/'executions/build-execution.json',32*1024*1024)
    meta.require(recorded.get('schemaVersion')==1 and recorded.get('kind')=='huhucapture-build-execution' and
                 recorded.get('platform')==platform and recorded.get('commands')==transformations.get('commands') and
                 recorded.get('commandOrigins')=={} and
                 recorded.get('outputs',{}).get('deploymentStages')==stages,
                 'Packaging stage inventories differ from the original successful execution record')
    commands=recorded.get('commands',[]); executions=recorded.get('executions',[])
    meta.require(commands and len(executions)==len(commands) and
                 all(isinstance(record,dict) and record.get('commandIndex')==index and
                     record.get('arguments')==commands[index] and type(record.get('exitCode')) is int and
                     record['exitCode']==0 for index,record in enumerate(executions)),
                 'Packaging requires every original executed command to have succeeded')
    producer=recorded.get('producer',{})
    meta.require(producer.get('sha256')==transformations.get('packagerSha256'),
                 'Packaging producer differs from the original retained script')
    original=Path(producer['path']).parent.parent
    def retained_path(value):
        name=Path(value).relative_to(original).as_posix()
        path=stage/'executions'/collector.relative(name)
        meta.require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to((stage/'executions').resolve()),
                     'Original packaging evidence is missing or outside its retained directory')
        return path
    for record in [producer,*recorded.get('records',[])]:
        observed=meta.descriptor(retained_path(record['path']))
        meta.require(all(observed[key]==record.get(key) for key in ('size','sha256')),
                     'Retained original packaging input or log differs')
    for index,record in enumerate(executions):
        meta.require(meta.json_read(stage/'executions'/('execution-'+str(index)+'.json'),32*1024*1024)==record,
                     'Original per-command packaging execution record differs')
        retained_path(record['stdout']); retained_path(record['stderr'])
    return transformations,recorded


def verify_prepared(stage: Path):
    prepared=meta.json_read(stage/'prepared.json',32*1024*1024)
    meta.require(prepared.get('schemaVersion') == 1 and prepared.get('kind')=='huhucapture-prepared-packages' and
                 prepared.get('intent') in ('development','release'), 'Unknown or incomplete package preparation')
    development=prepared['intent']=='development'; build=prepared['build']
    meta.identity(build,development=development)
    expected=package_products(build['platform'])
    meta.require(prepared.get('products')==expected and prepared['deployment'].get('products')==expected,
                 'Prepared products do not cover both actual package forms')
    collector=source_inventory_api(); actual=collector.inventory(stage/'payloads')
    meta.require(actual==prepared['deployment']['files'] and
                 collector.tree_digest(actual)==prepared['deployment']['treeSha256'] and
                 meta.json_read(stage/'source-deployment-inventory.json',32*1024*1024)==actual,
                 'Prepared deployment changed after source collection')
    meta.require(meta.descriptor(stage/'transformations.json')==prepared['transformations'],
                 'Actual packaging transformation records changed')
    transformations,recorded=verify_transformations(stage,build['platform'])
    selection=prepared.get('runtimeSelection')
    meta.require(transformations.get('runtimeSelection')==selection and
                 recorded['outputs'].get('runtimeSelection')==selection and
                 (selection is None or build['platform']=='linux'),
                 'Original runtime selection differs from the frozen packaging execution')
    verify_runtime_selection(stage,selection,recorded)
    original=meta.json_read(stage/'installed-input-inventory.json',32*1024*1024)
    identity={key:value for key,value in build.items() if key != 'packagingTools'}
    meta.require(transformations.get('buildIdentity')==identity and
                 transformations.get('installedInput')==meta.descriptor(stage/'installed-input-inventory.json') and
                 recorded.get('inputs')=={'buildIdentity':identity,'installedInput':original},
                 'Original packaging input inventory or actual build identity differs')
    for key,value in {'deploymentTreeSha256':prepared['deployment']['treeSha256'],
                      'products':expected,'artifacts':[*prepared['packages'],*prepared['additionalAssets']]}.items():
        meta.require(recorded['outputs'].get(key)==value and transformations.get(key)==value,
                     'Original packaging execution differs from the frozen final '+key)
    meta.require(recorded['outputs'].get('files')==actual,
                 'Original packaging execution differs from the frozen final file inventory')
    lineage_api().validate(stage,transformations,recorded,collector)
    for product in expected:
        identity,_=meta.audit_tree(stage/'payloads'/product['root'],product['packageKind'],development=development)
        meta.require(identity=={key:value for key,value in build.items() if key != 'packagingTools'},
                     'Prepared product identity differs from its native build')
    entries=prepared['packages']; expected_kinds={item['packageKind'] for item in expected}
    meta.require(len(entries)==2 and {item['packageKind'] for item in entries}==expected_kinds,
                 'Prepared candidate package matrix is incomplete')
    for item in entries:
        meta.require(item.get('contentRoot')==item['packageKind'], 'Prepared package content root differs')
        meta.verify_artifact(stage/'artifacts',item,item['packageKind'],build['version'])
    for item in prepared['additionalAssets']:
        meta.require(Path(item['name']).name==item['name'] and meta.descriptor(stage/'artifacts'/item['name'])==item,
                     'Prepared symbols changed or escaped the output')
    meta.require({path.name for path in (stage/'artifacts').iterdir()}==
                 {item['name'] for item in [*entries,*prepared['additionalAssets']]}, 'Unexpected prepared artifact')
    return prepared


def verify_source_materials(bundle: Path, manifest: Path, build: dict, deployment: dict):
    evidence=meta.json_read(manifest,64*1024*1024)
    meta.require(isinstance(evidence,dict) and isinstance(deployment,dict), 'Corresponding-source metadata must be an object')
    for key in ('product','repository','version','sourceCommit','platform','architecture'):
        meta.require(evidence.get(key)==build[key], 'Corresponding source manifest describes a different build')
    stem=f"huhucapture-v{build['version']}-{build['platform']}-{build['architecture']}"
    meta.require(bundle.name==stem+'-sources.tar.gz' and manifest.name==stem+'-source-manifest.json',
                 'Source asset names must bind their actual platform and architecture')
    actual=meta.descriptor(bundle)
    meta.require(evidence.get('schemaVersion')==1 and evidence.get('generator')=='scripts/create-source-bundle.py' and
                 evidence.get('archive')==actual and evidence.get('complete') is True and evidence.get('gaps')==[],
                 'Corresponding-source bytes or computed completion do not match their manifest')
    required={'huhucapture','qt','gifski','localsend','ffmpeg','rust-vendor'}
    if build['platform']=='linux': required |= {'libportal','pipewire','x264','appimage-runtime'}
    meta.require(required <= {entry['name'] for entry in evidence.get('components',[])},
                 'Corresponding source material omits a shipped dependency family')
    meta.require(evidence.get('deployment')==deployment,
                 'Corresponding sources must bind the unchanged final payloads of both package forms')
    collector=source_inventory_api()
    meta.require(deployment.get('products')==package_products(build['platform']) and
                 collector.tree_digest(deployment['files'])==deployment.get('treeSha256'),
                 'Corresponding-source deployment inventory is incomplete or has changed')
    expected_inventory=evidence.get('sourceInventory',{})
    meta.require(expected_inventory.get('path')=='huhucapture-sources/source-inventory.json' and
                 meta.HEX64.fullmatch(expected_inventory.get('sha256','')) is not None,
                 'Corresponding-source archive has no bound original collector report')
    found=False; members=0; expanded=0
    with tarfile.open(bundle,'r|gz') as archive:
        for entry in archive:
            members+=1; expanded+=entry.size
            meta.require(members<=500000 and 0<=entry.size<=32*1024*1024*1024 and expanded<=64*1024*1024*1024,
                         'Corresponding-source archive exceeds the bounded inspection budget')
            if entry.name != expected_inventory['path']: continue
            meta.require(not found and entry.isfile() and 0 < entry.size <= 64*1024*1024,
                         'Corresponding-source report in the actual archive is duplicate or invalid')
            incoming=archive.extractfile(entry)
            meta.require(incoming is not None, 'Cannot read actual archived collector report')
            raw=incoming.read(64*1024*1024+1)
            meta.require(len(raw)==entry.size and hashlib.sha256(raw).hexdigest()==expected_inventory['sha256'] and
                         json.loads(raw)=={key:value for key,value in evidence.items() if key not in {'archive','sourceInventory'}},
                         'External source manifest differs from the report inside the actual archive')
            found=True
    meta.require(found, 'The corresponding-source archive does not contain its recorded collector report')
    return {'archive':actual,'manifest':meta.descriptor(manifest)}


def verify_inventory_binding(files, deployment):
    """Compare updater inventory with the collector's exact file/link format."""
    actual={entry['path']:entry for entry in files}
    source={entry['path']:entry for entry in deployment['files']}
    meta.require(len(actual)==len(files) and len(source)==len(deployment['files']) and set(actual)==set(source),
                 'Build-info and corresponding-source deployment paths differ')
    for name, entry in source.items():
        item=actual[name]
        meta.safe_relative(name)
        if entry['type']=='symlink':
            meta.require(item.get('linkTarget')==entry['target'], 'Build-info link differs from the final source-mapped link')
        else:
            meta.require(entry['type']=='file' and 'linkTarget' not in item and
                         item.get('size')==entry['size'] and item.get('sha256')==entry['sha256'],
                         'Build-info bytes differ from the final source-mapped deployment')


def finalize(args):
    stage=args.stage.resolve(); output=args.output.resolve()
    meta.require(not output.is_relative_to(stage) and not stage.is_relative_to(output),
                 'Final output must not alter the preserved preparation')
    prepared=verify_prepared(stage); build=prepared['build']
    development=prepared['intent']=='development'
    meta.require(development==args.development,
                 'A prepared development bundle cannot be promoted to release; finalization intent must match')
    verify_source(build,development)
    sources=None
    if not development:
        if getattr(args,'source_plan',None) is not None:
            meta.require(args.source_bundle is None and args.source_manifest is None and args.source_output is not None,
                         'Supply one verified source plan/output or an already collected archive/manifest pair')
            meta.require(args.source_plan.is_file() and not args.source_plan.is_symlink(),
                         'The actual producer materials have not supplied the final-stage source plan; release completion is blocked')
            run([sys.executable,ROOT/'scripts/create-source-bundle.py','collect','--plan',args.source_plan,
                 '--output',args.source_output,'--require-complete'])
            stem=f"huhucapture-v{build['version']}-{build['platform']}-{build['architecture']}"
            args.source_bundle=args.source_output/(stem+'-sources.tar.gz')
            args.source_manifest=args.source_output/(stem+'-source-manifest.json')
        meta.require(args.source_bundle is not None and args.source_manifest is not None,
                     'Release finalization requires computed-complete corresponding sources for the final stage')
        sources=verify_source_materials(args.source_bundle,args.source_manifest,build,prepared['deployment'])
    else:
        meta.require(args.source_bundle is None and args.source_manifest is None and getattr(args,'source_plan',None) is None,
                     'Development output never claims release source completeness; retain diagnostic sources separately')
    meta.require(verify_prepared(stage)==prepared, 'Preparation changed during corresponding-source collection')
    empty_output(output)
    for item in [*prepared['packages'],*prepared['additionalAssets']]:
        copy_exclusive(stage/'artifacts'/item['name'],output/item['name'])
    if sources:
        copy_exclusive(args.source_bundle,output/args.source_bundle.name)
        copy_exclusive(args.source_manifest,output/args.source_manifest.name)
    files=meta.inventory(stage/'payloads')
    meta.write_platform_metadata(output,build,files,prepared['packages'],meta.locked_dependencies(ROOT,build['platform']),
        prepared['minimumSystem'],development=development,source_materials=sources,extra_assets=prepared['additionalAssets'],
        deployment={'treeSha256':prepared['deployment']['treeSha256'],'products':prepared['products']})
    verify_prepared(stage)
    print('Finalized exact prepared bytes and hashes; no repacking, relabeling, tag, upload, installation or publication occurred.')


def package(args):
    # Existing CI convenience remains explicitly development-only. Formal
    # packaging has a required source-collection boundary between two commands.
    meta.require(args.development, 'Formal packages require prepare, complete source collection, then finalize')
    meta.require(args.source_bundle is None and args.source_manifest is None,
                 'Use the explicit prepare/finalize boundary for corresponding sources')
    args.stage=args.stage or args.output.with_name(args.output.name+'-prepared')
    prepare(args)
    finalize(args)


def release_identity(manifest):
    meta.require(isinstance(manifest,dict), 'Release package metadata must be an object')
    meta.require(manifest.get('developmentOnly') is False and manifest.get('sourceDirty') is False,
                 'Development, dirty or unspecified package metadata cannot enter a release manifest')
    version=manifest.get('version',''); commit=manifest.get('sourceCommit','')
    meta.require(type(manifest.get('schemaVersion')) is int and manifest['schemaVersion']==1 and
                 manifest.get('product')==meta.PRODUCT and manifest.get('repository')==meta.REPOSITORY and
                 isinstance(version,str) and meta.VERSION.fullmatch(version) and manifest.get('tag')=='v'+version and
                 isinstance(commit,str) and meta.HEX40.fullmatch(commit), 'Noncanonical release identity')
    return {key:manifest[key] for key in ('schemaVersion','product','repository','tag','version','sourceCommit','sourceDirty','developmentOnly')}


def verify_release_files(folder: Path, manifest: dict, *, complete: bool):
    """One verifier shared by aggregation and the sole publication command."""
    identity=release_identity(manifest); entries=manifest.get('assets')
    meta.require(isinstance(entries,list) and len(entries)==(4 if complete else 2), 'The actual release package matrix is incomplete')
    seen=set(); platforms=set(); references={}; infos={}
    for item in entries:
        kind=item['packageKind']
        meta.require(kind in meta.KINDS and kind not in seen, 'Unexpected/duplicate release package')
        platform,arch,_,_=meta.KINDS[kind]
        meta.require(item['platform']==platform and item['architecture']==arch and item.get('contentRoot')==kind and
                     isinstance(item.get('minimumSystem'),str) and 0<len(item['minimumSystem'].encode('utf-8'))<=256,
                     'Package platform, architecture, content root or system baseline differs')
        seen.add(kind); platforms.add(platform)
        actual=meta.verify_artifact(folder,item,kind,identity['version'])
        info_name=f'build-info-{platform}-{arch}.json'
        meta.require(item['buildInfo'].get('name')==info_name and meta.descriptor(folder/info_name)==item['buildInfo'],
                     'Build-info reference or bytes changed')
        if info_name not in infos:
            info=meta.json_read(folder/info_name)
            meta.require(release_identity(info)==identity and info.get('platform')==platform and info.get('architecture')==arch,
                         'Build-info describes a different release/platform')
            meta.identity(info)
            source=info.get('sourceMaterials')
            meta.require(isinstance(source,dict) and set(source)=={'archive','manifest'},
                         'Release build-info lacks complete corresponding-source references')
            for reference in source.values():
                meta.require(Path(reference['name']).name==reference['name'] and meta.descriptor(folder/reference['name'])==reference,
                             'Corresponding-source artifact changed or escaped its output')
            evidence=meta.json_read(folder/source['manifest']['name'],64*1024*1024)
            deployment=evidence.get('deployment')
            meta.require(isinstance(deployment,dict) and info.get('deployment')==
                         {key:deployment.get(key) for key in ('treeSha256','products')},
                         'Release build-info is not bound to its actual final deployment')
            meta.require(verify_source_materials(folder/source['archive']['name'],folder/source['manifest']['name'],info,deployment)==source,
                         'Corresponding-source references differ')
            verify_inventory_binding(info['files'],deployment)
            symbols=info.get('additionalAssets')
            expected_symbol=f"huhucapture-v{identity['version']}-{platform}-{arch}"+('-symbols.zip' if platform=='windows' else '-debug.tar.zst')
            meta.require(isinstance(symbols,list) and len(symbols)==1 and symbols[0].get('name')==expected_symbol,
                         'Matching native debug symbols are missing or ambiguous')
            infos[info_name]=info
        info=infos[info_name]
        matching=[entry for entry in info.get('packages',[]) if entry.get('packageKind')==kind]
        meta.require(len(matching)==1 and matching[0]=={**actual,'packageKind':kind,'contentRoot':kind},
                     'Build-info package bytes differ from the release entry')
        for reference in [actual,item['buildInfo'],*info['additionalAssets'],*info['sourceMaterials'].values()]:
            name=reference['name']
            meta.require(Path(name).name==name and meta.descriptor(folder/name)==reference,
                         'Referenced artifact changed or escaped its platform output')
            meta.require(name not in references or references[name]==reference, 'Conflicting release asset names')
            references[name]=reference
    if complete:
        meta.require(platforms=={'windows','linux'} and seen==set(meta.KINDS), 'The complete Windows/Linux package matrix is required')
    else:
        meta.require(len(platforms)==1 and seen=={kind for kind,value in meta.KINDS.items() if value[0] in platforms},
                     'A platform output must contain both real package forms')
    return identity,references


def verify_aggregate(folder: Path, *, expected_commit=None, expected_version=None):
    manifest=meta.json_read(folder/'release-manifest.json',1024*1024)
    identity,references=verify_release_files(folder,manifest,complete=True)
    meta.require(expected_commit is None or identity['sourceCommit']==expected_commit, 'Release source commit differs from the requested source')
    meta.require(expected_version is None or identity['version']==expected_version, 'Release version differs from the single source of truth')
    references['release-manifest.json']=meta.descriptor(folder/'release-manifest.json')
    expected=''.join(references[name]['sha256']+'  '+name+'\n' for name in sorted(references))
    checksums=folder/'SHA256SUMS.txt'
    meta.require(checksums.is_file() and not checksums.is_symlink() and checksums.read_bytes()==expected.encode('ascii'),
                 'Release checksums omit, add or change an exact asset')
    references[checksums.name]=meta.descriptor(checksums)
    meta.require({path.name for path in folder.iterdir()}==set(references), 'Release directory contains undeclared artifacts')
    return manifest,references


def aggregate(args):
    output=args.output.resolve(); empty_output(output)
    combined=[]; baseline=None; names=set(); seen_platforms=set()
    meta.require(len(args.inputs)==2, 'Exactly one native output from each platform is required')
    for folder in args.inputs:
        folder=folder.resolve()
        manifests=list(folder.glob('package-manifest-*.json'))
        meta.require(len(manifests)==1,'Each platform input requires one generated package manifest')
        manifest=meta.json_read(manifests[0],1024*1024)
        current,references=verify_release_files(folder,manifest,complete=False)
        if baseline is None: baseline=current
        meta.require(current==baseline,'Platforms were built from different identities')
        platform=manifest['assets'][0]['platform']
        meta.require(platform not in seen_platforms, 'Duplicate platform output')
        seen_platforms.add(platform)
        for name,reference in references.items():
            meta.require(name not in names, 'Different platform assets collided by name')
            names.add(name); copy_exclusive(folder/name,output/name)
        combined.extend(manifest['assets'])
    # developmentOnly is explicit in the canonical manifest consumed by both
    # real Qt and legacy About paths. Absence must not accidentally mean stable.
    meta.json_write(output/'release-manifest.json',{**baseline,'assets':sorted(combined,key=lambda item:item['name'])})
    checksums=[meta.sha256(path)+'  '+path.name for path in sorted(output.iterdir()) if path.is_file()]
    with (output/'SHA256SUMS.txt').open('x',encoding='ascii',newline='\n') as stream: stream.write('\n'.join(checksums)+'\n')
    verify_aggregate(output)
    print('Complete aggregate manifest generated from exact artifacts; no publishing operation was performed.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    for name in ('prepare','package'):
        pack=commands.add_parser(name,help='Preserve actual final package payloads' if name=='prepare' else 'Prepare/finalize an explicit development bundle')
        pack.add_argument('--installed',type=Path,required=True)
        pack.add_argument('--stage',type=Path,required=name=='prepare')
        if name=='package':
            pack.add_argument('--output',type=Path,required=True)
            pack.add_argument('--source-bundle',type=Path);pack.add_argument('--source-manifest',type=Path)
        pack.add_argument('--minimum-system',required=True)
        pack.add_argument('--tools',type=Path);pack.add_argument('--qmake',type=Path);pack.add_argument('--symbols',type=Path)
        pack.add_argument('--appimage-runtime-build',type=Path,
            help='Completed native runtime source-build workspace; verifies every actual source/static input and output before selecting it')
        pack.add_argument('--nsis-installer',type=Path,help='Exact reviewed NSIS 3.11 distribution, extracted without executing setup')
        pack.add_argument('--development',action='store_true',help='Explicit non-release intent; never promotable to a formal release')
    finish=commands.add_parser('finalize',help='Verify frozen payloads and complete sources, then write platform metadata without repacking')
    finish.add_argument('--stage',type=Path,required=True);finish.add_argument('--output',type=Path,required=True)
    finish.add_argument('--source-bundle',type=Path);finish.add_argument('--source-manifest',type=Path)
    finish.add_argument('--source-plan',type=Path,help='Actual producer/material plan for this frozen final deployment')
    finish.add_argument('--source-output',type=Path,help='New output directory for computed-complete corresponding sources')
    finish.add_argument('--development',action='store_true')
    combined=commands.add_parser('aggregate',help='Verify and combine both native platform outputs, without publishing')
    combined.add_argument('--input',dest='inputs',action='append',type=Path,required=True);combined.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        {'prepare':prepare,'finalize':finalize,'package':package,'aggregate':aggregate}[args.command](args)
        return 0
    except (ValueError,KeyError,TypeError,OSError,tarfile.TarError,subprocess.CalledProcessError) as error:
        print('Qt packaging failed; existing outputs retained:',error,file=sys.stderr)
        return 1


if __name__=='__main__':
    raise SystemExit(main())
