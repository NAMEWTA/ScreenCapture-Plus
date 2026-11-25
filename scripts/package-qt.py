#!/usr/bin/env python3
"""Package the installed unified Qt product using native NSIS/CPack/linuxdeploy.

This script never publishes, tags, changes VERSION, executes installers, or
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
import zipfile
from version_source import read_version

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location('huhucapture_package_metadata', ROOT/'build/package/metadata.py')
meta = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(meta)


def run(arguments, *, cwd=None, env=None, capture=False):
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
    destination.write_text(json.dumps({'schemaVersion':1, 'product':'huhucapture', 'platform':platform,
                                      'architecture':architecture, 'packageKind':kind}, indent=2)+'\n')


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
        install.extend(['    SetOutPath "'+output_dir+'"', '    File "/oname='+nsi_escape(relative.name)+'" "'+nsi_escape(prefix/relative)+'"'])
        remove.append('    Delete "$INSTDIR\\'+nsi_escape(str(relative).replace('/', '\\'))+'"')
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
    shutil.copytree(installed, appdir/'usr', symlinks=True)
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
    run(arguments, env=environment)
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
        debug = destination/relative.with_name(relative.name+'.debug'); debug.parent.mkdir(parents=True,exist_ok=True)
        run([objcopy,'--only-keep-debug',path,debug])
        run([objcopy,'--strip-debug',path])
        run([objcopy,'--add-gnu-debuglink='+str(debug),path])
        records.append({'path':relative.as_posix(),'buildId':identity.group(1), 'runtimeSha256':meta.sha256(path),
                        'debug':debug.relative_to(destination).as_posix(), 'debugSha256':meta.sha256(debug)})
    meta.require(any(item['path'] == 'bin/huhucapture' for item in records), 'Missing matching app debug symbols')
    meta.json_write(destination/'build-ids.json', {'schemaVersion':1,'files':records})


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


def linux_deb(appdir: Path, output: Path, workspace: Path, version: str):
    # CPack otherwise falls back to only explicit Depends when shlibdeps is
    # absent. This pipeline requires the actual ELF dependency calculation.
    tool('dpkg-shlibdeps'); tool('file')
    root = workspace/'deb-root'; (root/'opt').mkdir(parents=True)
    shutil.copytree(appdir/'usr', root/'opt/huhucapture', symlinks=True)
    marker(root/'opt/huhucapture','deb')
    (root/'usr/bin').mkdir(parents=True)
    shutil.copy2(ROOT/'build/package/deb-launcher',root/'usr/bin/huhucapture')
    (root/'usr/bin/huhucapture').chmod(0o755)
    for folder in ('applications','icons'):
        shutil.copytree(root/'opt/huhucapture/share'/folder,root/'usr/share'/folder,symlinks=True)
    config = workspace/'CPackDEBConfig.cmake'
    settings = {
        'CPACK_GENERATOR':'DEB', 'CPACK_PACKAGE_NAME':'huhucapture', 'CPACK_PACKAGE_VERSION':version,
        'CPACK_PACKAGE_VENDOR':'NAMEWTA', 'CPACK_PACKAGE_CONTACT':'https://github.com/NAMEWTA/huhucapture/issues',
        'CPACK_PACKAGE_DESCRIPTION_SUMMARY':'Screen capture, annotation, recording and LAN sharing with Qt 6',
        'CPACK_PACKAGE_DESCRIPTION':'Complete Qt 6 desktop application for screen capture, annotation, recording, scrolling capture and LAN sharing.',
        'CPACK_PACKAGE_FILE_NAME':output.stem, 'CPACK_DEBIAN_FILE_NAME':output.name,
        'CPACK_PACKAGE_DIRECTORY':str(output.parent), 'CPACK_PACKAGING_INSTALL_PREFIX':'/',
        'CPACK_DEBIAN_PACKAGE_ARCHITECTURE':'amd64', 'CPACK_DEBIAN_PACKAGE_SECTION':'graphics',
        'CPACK_DEBIAN_PACKAGE_SHLIBDEPS':'ON', 'CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS':str(root/'opt/huhucapture/lib'),
        'CPACK_DEBIAN_PACKAGE_DEPENDS':'xdg-desktop-portal, ca-certificates', 'CPACK_DEBIAN_PACKAGE_RECOMMENDS':'pipewire',
        'CPACK_DEBIAN_PACKAGE_HOMEPAGE':'https://github.com/NAMEWTA/huhucapture',
        'CPACK_DEBIAN_COMPRESSION_TYPE':'xz', 'CPACK_SET_DESTDIR':'OFF',
    }
    config.write_text('\n'.join('set('+key+' '+cmake_quote(value)+')' for key,value in settings.items())+
                      '\nset(CPACK_INSTALLED_DIRECTORIES '+cmake_quote(root)+' "/")\n')
    run([tool('cpack'),'--config',config,'-G','DEB'],cwd=workspace)
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
    return root


def source_materials(args, output: Path, build: dict):
    if args.source_bundle is None or args.source_manifest is None:
        meta.require(args.development, 'Release packaging requires the corresponding source bundle and its matching manifest')
        return None
    evidence = meta.json_read(args.source_manifest)
    for key in ('product','repository','version','sourceCommit'):
        meta.require(evidence.get(key) == build[key], 'Corresponding source manifest describes a different build')
    actual = meta.descriptor(args.source_bundle)
    meta.require(evidence.get('archive') == actual and evidence.get('complete') is True,
                 'Corresponding-source archive bytes or completion status do not match their manifest')
    required = {'huhucapture','qt','gifski','localsend','ffmpeg','rust-vendor'}
    if build['platform'] == 'linux': required |= {'libportal','pipewire','x264','appimage-runtime'}
    meta.require(required <= {entry['name'] for entry in evidence.get('components',[])}, 'Corresponding source material omits a shipped dependency family')
    copy_exclusive(args.source_bundle,output/actual['name'])
    copy_exclusive(args.source_manifest,output/args.source_manifest.name)
    return {'archive':actual,'manifest':meta.descriptor(output/args.source_manifest.name)}


def package(args):
    installed = args.installed.resolve(); output = args.output.resolve()
    meta.require(installed.is_dir(), 'The actual CMake-installed tree is required')
    output.mkdir(parents=True, exist_ok=True)
    meta.require(not any(output.iterdir()), 'Use an empty output directory; existing results are retained')
    platform = 'windows' if sys.platform == 'win32' else 'linux'
    kind = 'portable-zip' if platform == 'windows' else 'appimage'
    build, _ = meta.audit_tree(installed,kind,development=args.development)
    verify_source(build,args.development)
    meta.require(build['platform'] == platform, 'Package on the actual target platform; cross-host fake assets are forbidden')
    sources = source_materials(args,output,build)
    version = build['version']; arch=build['architecture']; dependencies=meta.locked_dependencies(ROOT,platform)
    all_files, packages, extra_assets = [], [], []
    with tempfile.TemporaryDirectory(prefix='huhucapture-package-') as temporary:
        workspace = Path(temporary)
        if platform == 'windows':
            portable=workspace/'portable-zip'; shutil.copytree(installed,portable,symlinks=True); marker(portable,'portable-zip')
            installer=workspace/'installer'; shutil.copytree(installed,installer,symlinks=True); marker(installer,'installer')
            meta.audit_tree(portable,'portable-zip',development=args.development)
            meta.audit_tree(installer,'installer',development=args.development)
            zip_path=output/meta.asset_name(version,'portable-zip'); zip_tree(portable,zip_path)
            installer_path=output/meta.asset_name(version,'installer')
            compiler=windows_installer(installer,installer_path,workspace,version,args.nsis_installer)
            verify_windows_symbols(args.symbols,portable/'huhucapture.exe',build)
            symbols=output/f'huhucapture-v{version}-windows-x64-symbols.zip'; zip_tree(args.symbols,symbols)
            extra_assets.append(meta.descriptor(symbols))
            for item, tree, artifact in [('portable-zip',portable,zip_path),('installer',installer,installer_path)]:
                all_files += meta.inventory(tree,item+'/')
                packages.append({**meta.descriptor(artifact),'packageKind':item,'contentRoot':item})
            build['packagingTools']=[compiler]
        else:
            meta.require(args.tools is not None and args.qmake is not None, 'Linux requires verified pinned packaging tools and the matching Qt qmake')
            paths,provenance=verify_tools(args.tools,workspace/'tools')
            qmake=args.qmake.resolve()
            meta.require(run([qmake,'-query','QT_VERSION'],capture=True).strip() == build['qtVersion'], 'Packager Qt SDK does not match the actual binary')
            appdir=workspace/'appimage'; environment=prepare_appdir(installed,appdir,paths,qmake)
            symbols=workspace/'debug'; split_debug(appdir/'usr',symbols)
            symbol_path=output/f'huhucapture-v{version}-linux-amd64-debug.tar.zst';tar_zstd(symbols,symbol_path,workspace)
            extra_assets.append(meta.descriptor(symbol_path))
            deb_path=output/meta.asset_name(version,'deb'); deb=linux_deb(appdir,deb_path,workspace,version)
            image_path=output/meta.asset_name(version,'appimage')
            run([paths['appimagetool'],'--runtime-file',paths['appimageRuntime'],'--comp','zstd','--mksquashfs-opt','-processors','--mksquashfs-opt','1',appdir,image_path],env=environment)
            meta.require(meta.machine(image_path) == 'linux', 'AppImage has the wrong outer runtime architecture')
            with image_path.open('rb') as stream: stream.seek(8); meta.require(stream.read(3)==b'AI\x02','AppImage type-2 magic is missing')
            extracted=workspace/'appimage-verified'; extracted.mkdir()
            run([image_path,'--appimage-extract'],cwd=extracted,env=environment)
            extracted=extracted/'squashfs-root'
            meta.require(meta.inventory(appdir) == meta.inventory(extracted), 'AppImage extraction differs from the actual staged payload')
            meta.audit_tree(extracted/'usr','appimage',development=args.development)
            for item,tree,artifact in [('deb',deb,deb_path),('appimage',appdir,image_path)]:
                all_files += meta.inventory(tree,item+'/')
                packages.append({**meta.descriptor(artifact),'packageKind':item,'contentRoot':item})
            build['packagingTools']=provenance
        meta.write_platform_metadata(output,build,all_files,packages,dependencies,args.minimum_system,
                                     development=args.development,source_materials=sources,extra_assets=extra_assets)
    print('Actual package files and hashes written. No tag, upload, installation or release was performed.')


def aggregate(args):
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    meta.require(not any(output.iterdir()), 'Aggregate output must be empty')
    combined=[]; baseline=None; seen=set(); platforms=set()
    for folder in args.inputs:
        manifests=list(folder.glob('package-manifest-*.json'))
        meta.require(len(manifests)==1,'Each platform input requires one generated package manifest')
        manifest=meta.json_read(manifests[0],1024*1024)
        meta.require(manifest.get('developmentOnly') is False and manifest.get('sourceDirty') is False,
                     'Development or dirty package metadata cannot enter a release manifest')
        current={key:manifest[key] for key in ('schemaVersion','product','repository','tag','version','sourceCommit','sourceDirty')}
        if baseline is None: baseline=current
        meta.require(current==baseline and current['product']==meta.PRODUCT and current['repository']==meta.REPOSITORY,'Platforms were built from different identities')
        for item in manifest['assets']:
            kind=item['packageKind']; platform,arch,_,budget=meta.KINDS[kind]
            meta.require(kind not in seen and item['platform']==platform and item['architecture']==arch and item['name']==meta.asset_name(current['version'],kind),'Unexpected/duplicate release package')
            seen.add(kind); platforms.add(platform)
            actual=meta.verify_artifact(folder,item,kind,current['version'])
            info_name=item['buildInfo']['name'];meta.require(info_name==f'build-info-{platform}-{arch}.json','Unexpected build-info reference')
            meta.require(meta.descriptor(folder/info_name)==item['buildInfo'],'Build-info changed before aggregation')
            info=meta.json_read(folder/info_name)
            meta.require({key:info[key] for key in current}==current and info.get('developmentOnly') is False and
                         info.get('sourceDirty') is False and info.get('sourceMaterials'),
                         'Build-info lacks matching clean release identity/corresponding source')
            references=[actual,item['buildInfo'],*info.get('additionalAssets',[]),*info['sourceMaterials'].values()]
            for reference in references:
                meta.require(Path(reference['name']).name==reference['name'] and meta.descriptor(folder/reference['name'])==reference,'Referenced artifact changed or escaped its platform output')
                destination=output/reference['name']
                if destination.exists(): meta.require(meta.descriptor(destination)==reference,'Different platform assets collided by name')
                else: copy_exclusive(folder/reference['name'],destination)
            combined.append(item)
    meta.require(platforms=={'windows','linux'} and seen==set(meta.KINDS),'The complete Windows/Linux package matrix is required')
    meta.json_write(output/'release-manifest.json',{**baseline,'assets':sorted(combined,key=lambda item:item['name'])})
    checksums=[meta.sha256(path)+'  '+path.name for path in sorted(output.iterdir()) if path.is_file()]
    with (output/'SHA256SUMS.txt').open('x',encoding='ascii',newline='\n') as stream: stream.write('\n'.join(checksums)+'\n')
    print('Complete aggregate manifest generated from exact artifacts; no publishing operation was performed.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    pack=commands.add_parser('package',help='Package one actual native CMake install tree')
    pack.add_argument('--installed',type=Path,required=True);pack.add_argument('--output',type=Path,required=True)
    pack.add_argument('--minimum-system',required=True)
    pack.add_argument('--tools',type=Path);pack.add_argument('--qmake',type=Path);pack.add_argument('--symbols',type=Path)
    pack.add_argument('--nsis-installer',type=Path,help='Exact reviewed NSIS 3.11 distribution, extracted without executing setup')
    pack.add_argument('--source-bundle',type=Path);pack.add_argument('--source-manifest',type=Path)
    pack.add_argument('--development',action='store_true',help='Explicit non-release bundle; dirty sources permitted and release aggregation forbidden')
    final=commands.add_parser('aggregate',help='Verify and combine both native platform outputs, without publishing')
    final.add_argument('--input',dest='inputs',action='append',type=Path,required=True);final.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        package(args) if args.command=='package' else aggregate(args)
        return 0
    except (ValueError,OSError,subprocess.CalledProcessError) as error:
        print('Qt packaging failed; existing outputs retained:',error,file=sys.stderr)
        return 1


if __name__=='__main__':
    raise SystemExit(main())
