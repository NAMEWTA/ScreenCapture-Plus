#!/usr/bin/env python3
"""Collect the same-run libportal/PipeWire/x264/FFmpeg source and build inputs.

Run on the original builder, after installation and before packaging transforms.
Only the collector's new outputs and temporary Git object/index directory are
written. This does not fetch, configure, compile, install, or modify a dependency.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import stat
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
specification = importlib.util.spec_from_file_location('huhucapture_runtime_materials', ROOT / 'scripts/collect-runtime-materials.py')
materials = importlib.util.module_from_spec(specification)
specification.loader.exec_module(materials)
require = materials.meta.require


def snapshot(path):
    """Empty source/configuration files are valid; special files are not."""
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode), 'Expected an original regular file: ' + str(path))
    result = {'size': before.st_size, 'sha256': materials.sdk.digest(path)}
    after = path.lstat()
    require((before.st_dev, before.st_ino, before.st_mode, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_mode, after.st_size, after.st_mtime_ns),
            'An original input changed while collecting: ' + str(path))
    return result


def retain(path, destination, inputs):
    require(not path.is_symlink(), 'An original configuration/receipt cannot be a link: ' + str(path))
    before = snapshot(path)
    require(before['size'] <= 64 * 1024 * 1024, 'Oversized native configuration or receipt')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with path.open('rb') as incoming, destination.open('xb') as outgoing:
        while chunk := incoming.read(1024 * 1024):
            outgoing.write(chunk)
    require(snapshot(path) == before and snapshot(destination) == before, 'Retained native input bytes changed')
    inputs[path] = before
    return before


def git(arguments, *, cwd, git_dir=None, index=None, conversion=None, optional=False, binary=False, input_data=None):
    # A caller's GIT_DIR/INDEX/OBJECT variables must never redirect our writes.
    # Original Git commands below only inspect HEAD, origin and conversion data.
    environment = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    environment.update(GIT_OPTIONAL_LOCKS='0', GIT_NO_REPLACE_OBJECTS='1', GIT_NO_LAZY_FETCH='1',
                       GIT_TERMINAL_PROMPT='0', LC_ALL='C.UTF-8')
    command = ['git', '-c', 'core.fsmonitor=false', '-c', 'core.untrackedCache=false', '-c', 'gc.auto=0']
    if arguments[0] == 'clone':
        # This is strictly a local object-sharing clone, with no global URL
        # rewriting or caller-supplied Git template in its owned directory.
        environment.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull)
        command += ['-c', 'init.templateDir=']
    if git_dir is not None:
        # Do not inherit arbitrary global clean filters, hooks or archive tools.
        environment.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull)
        command += ['--git-dir=' + str(git_dir), '--work-tree=' + str(cwd), '-c', 'core.bare=false']
    if index is not None:
        require(git_dir is not None and index.parent == git_dir, 'Only an owned index can be written')
        environment['GIT_INDEX_FILE'] = str(index)
    for key, value in (conversion or {}).items():
        command += ['-c', key + '=' + value]
    result = subprocess.run(command + list(map(str, arguments)), cwd=cwd, env=environment, input=input_data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    require(result.returncode == 0 or (optional and result.returncode == 1),
            'Git source inspection failed: ' + result.stderr.decode('utf-8', errors='replace').strip())
    return result.stdout if binary else result.stdout.decode('utf-8', errors='strict').strip()


def source_inventory(source):
    result, total = {}, 0
    for directory, folders, files in os.walk(source, followlinks=False):
        parent = Path(directory)
        if parent == source:
            folders[:] = [name for name in folders if name != '.git']
        for name in list(folders):
            if (parent / name).is_symlink():
                folders.remove(name); files.append(name)
        for name in sorted(files):
            path = parent / name
            relative = materials.meta.safe_relative(path.relative_to(source).as_posix())
            require('.git' not in PurePosixPath(relative).parts, 'Nested Git source directory is not collected')
            if path.is_symlink():
                target = os.readlink(path)
                require(not os.path.isabs(target) and path.resolve().is_relative_to(source),
                        'An upstream source link escapes its complete source tree')
                entry = {'linkTarget': target}
            else:
                entry = snapshot(path)
                entry['executable'] = bool(path.stat().st_mode & 0o111)
                total += entry['size']
            result[relative] = entry
            require(len(result) <= 100000 and total <= 2 * 1024**3, 'Native source inventory exceeded its budget')
    require(result, 'The original dependency checkout is empty')
    return result


def tree_files(source, owned, tree):
    result = {}
    data = git(['ls-tree', '-rz', '--full-tree', tree], cwd=source, git_dir=owned, binary=True)
    for record in data.rstrip(b'\0').split(b'\0'):
        header, name = record.split(b'\t', 1)
        mode, kind, digest = header.decode('ascii').split()
        relative = materials.meta.safe_relative(name.decode('utf-8'))
        require(mode in ('100644', '100755', '120000') and kind == 'blob',
                'A native source submodule or unsupported tree entry is not collected: ' + relative)
        require(relative not in result, 'Duplicate native Git source path')
        result[relative] = {'mode': mode, 'gitBlobSha1': digest}
    require(0 < len(result) <= 100000, 'Empty or oversized native Git tree')
    return result


def notice_name(relative):
    path = PurePosixPath(relative)
    return bool(re.match(r'(?i)^(?:licen[cs]e|copying|copyright|notice|unlicen[cs]e)(?:[._-].*)?$', path.name)) or 'LICENSES' in path.parts


def verify_archive(archive, family, expected, output):
    """Git writes the archive; verify every blob and retain original notices."""
    seen, files, notices, total = set(), [], [], 0
    with tarfile.open(archive, mode='r|') as stream:
        for member in stream:
            name = member.name.rstrip('/')
            if name == family:
                require(member.isdir(), 'Unexpected native archive root'); continue
            require(name.startswith(family + '/'), 'Native archive escaped its family root')
            relative = materials.meta.safe_relative(name[len(family) + 1:])
            if member.isdir():
                continue
            require(relative in expected and relative not in seen, 'Unexpected or duplicate native archive member')
            seen.add(relative)
            info = expected[relative]
            sha = hashlib.sha256()
            if member.issym():
                require(info['mode'] == '120000' and not os.path.isabs(member.linkname), 'Unexpected archive link')
                parts = PurePosixPath(relative).parent / member.linkname
                depth = 0
                for part in parts.parts:
                    depth += -1 if part == '..' else (0 if part == '.' else 1)
                    require(depth >= 0, 'An archived upstream link escapes the source root')
                payload = member.linkname.encode('utf-8')
                blob = hashlib.sha1(b'blob ' + str(len(payload)).encode() + b'\0' + payload).hexdigest()
                require(blob == info['gitBlobSha1'], 'Archived link differs from the fixed Git blob')
                files.append({'path': relative, **info, 'linkTarget': member.linkname})
                continue
            require(member.isfile() and info['mode'] != '120000' and member.size <= 64 * 1024 * 1024,
                    'Unexpected or oversized native archive file')
            require(bool(member.mode & 0o111) == (info['mode'] == '100755'), 'Archive changed an executable source mode')
            total += member.size
            require(total <= 2 * 1024**3, 'Native source archive exceeded its byte budget')
            blob = hashlib.sha1(b'blob ' + str(member.size).encode() + b'\0')
            is_notice = notice_name(relative)
            target = output / 'notices/native' / family / relative if is_notice else None
            if target is not None:
                target.parent.mkdir(parents=True, exist_ok=True)
            outgoing = target.open('xb') if target is not None else None
            length = 0
            try:
                incoming = stream.extractfile(member)
                require(incoming is not None, 'Unreadable source archive member')
                with incoming:
                    while chunk := incoming.read(1024 * 1024):
                        length += len(chunk); sha.update(chunk); blob.update(chunk)
                        if outgoing is not None:
                            outgoing.write(chunk)
            finally:
                if outgoing is not None:
                    outgoing.close()
            require(length == member.size and blob.hexdigest() == info['gitBlobSha1'],
                    'Archive source bytes differ from the fixed patched Git tree: ' + relative)
            digest = {'size': length, 'sha256': sha.hexdigest()}
            files.append({'path': relative, **info, **digest})
            if target is not None:
                require(snapshot(target) == digest, 'Retained upstream notice differs from its source blob')
                notices.append({'path': target.relative_to(output).as_posix(), 'sourcePath': relative, **digest})
    require(seen == set(expected), 'Git export attributes omitted source files from the native archive')
    required = {'libportal': {'COPYING'}, 'pipewire': {'COPYING', 'LICENSE'},
                'x264': {'COPYING'}, 'ffmpeg': {'LICENSE.md', 'COPYING.LGPLv2.1', 'COPYING.GPLv3'}}[family]
    require(required <= {item['sourcePath'] for item in notices}, 'Original native license texts are incomplete')
    return files, notices


def source_difference_report(family, source, source_output, expected_tree, expected, actual_tree, actual,
                             before, controls, conversion):
    """Keep bounded identity evidence when the real index comparison fails.

    Source contents are not exported here. The raw worktree blob identity can
    distinguish an unchanged upstream file from a Git conversion difference;
    neither identity nor diagnostics changes the exact-tree acceptance gate.
    """
    changed = sorted(name for name in set(expected) | set(actual) if expected.get(name) != actual.get(name))
    differences, remaining = [], 16 * 1024 * 1024
    for name in changed[:64]:
        original = before.get(name)
        item = {'path': name, 'expected': expected.get(name), 'actual': actual.get(name),
                'originalWorktree': original}
        if original is not None:
            if 'linkTarget' in original:
                payload = original['linkTarget'].encode('utf-8')
                item['worktreeGitBlobSha1'] = hashlib.sha1(
                    b'blob ' + str(len(payload)).encode() + b'\0' + payload).hexdigest()
            elif original['size'] <= remaining:
                path = source / name
                expected_input = {key: original[key] for key in ('size', 'sha256')}
                require(snapshot(path) == expected_input, 'Original source changed while diagnosing: ' + name)
                digest = hashlib.sha1(b'blob ' + str(original['size']).encode() + b'\0')
                with path.open('rb') as incoming:
                    while chunk := incoming.read(1024 * 1024):
                        digest.update(chunk)
                require(snapshot(path) == expected_input, 'Original source changed while diagnosing: ' + name)
                item['worktreeGitBlobSha1'] = digest.hexdigest()
                remaining -= original['size']
            else:
                item['worktreeGitBlobStatus'] = 'not-computed: bounded diagnostic byte budget'
        differences.append(item)
    unchanged = source_inventory(source) == before and all(snapshot(path) == digest for path, digest in controls.items())
    relative = Path('families') / family / 'source-differences.json'
    (source_output / relative).parent.mkdir(parents=True, exist_ok=True)
    materials.meta.json_write(source_output / relative, {
        'schemaVersion': 1, 'scope': 'Failed source comparison identities only; not source acceptance',
        'family': family, 'originalSource': str(source), 'gitVersion': git(['--version'], cwd=source),
        'gitCheckoutConversion': conversion, 'expectedPatchedGitTree': expected_tree,
        'actualCanonicalGitTree': actual_tree, 'differenceCount': len(changed),
        'differencesTruncated': len(changed) > len(differences), 'differences': differences,
        'originalWorktreeAndIndexUnchanged': unchanged, 'sourceAccepted': False,
    })
    return relative.as_posix(), len(changed)


def archive_source(family, source, package, output, source_output, inputs):
    require((source / '.git').is_dir() and not (source / '.git').is_symlink(),
            'Use the original builder checkout, not a substituted source archive or worktree')
    before = source_inventory(source)
    control_paths = [source / '.git' / name for name in ('HEAD', 'config', 'index')]
    controls = {path: snapshot(path) for path in control_paths}
    require(same_directory(git(['rev-parse', '--show-toplevel'], cwd=source), source) and
            git(['rev-parse', 'HEAD'], cwd=source) == package['commit'] and
            git(['remote', 'get-url', 'origin'], cwd=source) == package['repository'],
            'Original dependency checkout revision/origin differs from its locked build receipt')
    # Git for Windows commonly installs an unused LFS filter globally. Inspect
    # the attributes actually applied to these paths, not unrelated filters.
    attributes = git(['check-attr', '-z', '--stdin', 'filter', 'working-tree-encoding'], cwd=source, binary=True,
                     input_data=b''.join(name.encode('utf-8') + b'\0' for name in before)).split(b'\0')
    require(attributes[-1] == b'' and len(attributes[:-1]) % 3 == 0 and
            all(value in (b'unspecified', b'unset') for value in attributes[2:-1:3]),
            'A native source path uses a custom filter or encoding that needs independent review')
    conversion = {}
    for key in ('core.autocrlf', 'core.eol', 'core.filemode', 'core.symlinks'):
        value = git(['config', '--get', key], cwd=source, optional=True)
        if value:
            conversion[key] = value
    patch_records = []
    for patch in package.get('patches', []):
        relative = materials.meta.safe_relative(patch['path'])
        require(relative.startswith('third_party/patches/'), 'Unexpected controlled native patch location')
        path = ROOT / relative
        require(path.resolve(strict=True).is_relative_to(ROOT) and snapshot(path)['sha256'] == patch['sha256'],
                'Controlled native patch differs from the lock or escaped the project')
        retained = Path('families') / family / 'patches' / path.name
        patch_records.append({'source': relative, 'path': retained.as_posix(),
                              **retain(path, source_output / retained, inputs)})
    with tempfile.TemporaryDirectory(prefix='huhucapture-native-source-') as temporary:
        owned = Path(temporary).resolve() / 'objects.git'
        # --shared only reads existing objects; all new objects and both indexes
        # live here. No checkout/reset/clean/apply operation targets the original.
        git(['clone', '--bare', '--shared', '--no-hardlinks', '--no-tags', '--', str(source), str(owned)], cwd=Path(temporary))
        expected_index, actual_index = owned / 'expected.index', owned / 'actual.index'
        git(['read-tree', package['commit']], cwd=source, git_dir=owned, index=expected_index)
        for patch in patch_records:
            path = source_output / patch['path']
            git(['apply', '--cached', '--check', str(path)], cwd=source, git_dir=owned, index=expected_index)
            git(['apply', '--cached', str(path)], cwd=source, git_dir=owned, index=expected_index)
        expected_tree = git(['write-tree'], cwd=source, git_dir=owned, index=expected_index)
        expected = tree_files(source, owned, expected_tree)
        # Starting at the patched tree ensures even added, ignored patch paths
        # are tracked. --others WITHOUT excludes also exposes ignored extras.
        git(['read-tree', expected_tree], cwd=source, git_dir=owned, index=actual_index)
        others = git(['ls-files', '--others', '-z'], cwd=source, git_dir=owned, index=actual_index, binary=True)
        require(not others, 'Untracked files in native source (including ignored files): ' +
                others.decode('utf-8', errors='replace').replace('\0', ', ')[:2048])
        git(['add', '--renormalize', '--', '.'], cwd=source, git_dir=owned, index=actual_index, conversion=conversion)
        git(['add', '-A', '--', '.'], cwd=source, git_dir=owned, index=actual_index, conversion=conversion)
        actual_tree = git(['write-tree'], cwd=source, git_dir=owned, index=actual_index)
        if actual_tree != expected_tree:
            diagnostic, count = source_difference_report(
                family, source, source_output, expected_tree, expected, actual_tree,
                tree_files(source, owned, actual_tree), before, controls, conversion)
            raise ValueError(f'Original {family} source has changes beyond the exact ordered locked patches: '
                             f'{count} paths; see {diagnostic}')
        # Highest-precedence archive attributes in the OWNED Git directory
        # disable export omission/substitution; every resulting blob is checked.
        attributes = owned / 'info/attributes'; attributes.parent.mkdir(exist_ok=True)
        attributes.write_text('* -export-ignore -export-subst\n', encoding='utf-8', newline='\n')
        archive_name = family + '-' + package['commit'] + '-patched.tar'
        archive = source_output / archive_name
        git(['archive', '--format=tar', '--prefix=' + family + '/', '--output=' + str(archive), expected_tree],
            cwd=source, git_dir=owned)
        files, notices = verify_archive(archive, family, expected, output)
    require(source_inventory(source) == before and all(snapshot(path) == digest for path, digest in controls.items()),
            'Original dependency source or Git control files changed during collection')
    require(git(['rev-parse', 'HEAD'], cwd=source) == package['commit'], 'Original source HEAD changed during collection')
    inputs.update(controls)
    inventory_path = Path('families') / family / 'source-inputs.json'
    (source_output / inventory_path).parent.mkdir(parents=True, exist_ok=True)
    materials.meta.json_write(source_output / inventory_path, {
        'schemaVersion': 1, 'canonicalPatchedGitTree': expected_tree, 'gitCheckoutConversion': conversion,
        'worktreeFiles': [{'path': path, **info} for path, info in sorted(before.items())],
        'archiveFiles': files,
    })
    return {'repository': package['repository'], 'commit': package['commit'], 'patchedTree': expected_tree,
            'orderedPatches': patch_records, 'originalSource': str(source),
            'sourceArchive': {'path': archive_name, **snapshot(archive)},
            'sourceInventory': {'path': inventory_path.as_posix(), **snapshot(source_output / inventory_path)},
            'originalWorktreeAndIndexUnchanged': True, 'notices': notices}


def verify_receipt(family, receipt, package):
    actual = receipt['components'][family] if family in ('x264', 'ffmpeg') and 'components' in receipt else receipt
    windows = family == 'ffmpeg' and 'configurations' in receipt
    require(actual.get('source' if windows else 'commit') == package['commit'] and
            (windows or actual.get('repository') == package['repository']) and
            actual.get('patches') == package.get('patches', []),
            'Actual ' + family + ' build receipt differs from the current source/ordered patch lock')
    return actual


def same_directory(value, expected):
    return isinstance(value, str) and Path(value).is_absolute() and Path(value).resolve(strict=True) == expected


def configuration_file(build, relative, family, source_output, inputs, records):
    path = build / materials.meta.safe_relative(relative)
    require(path.resolve(strict=True).is_relative_to(build), 'Generated configuration escaped its actual build directory')
    target = Path('families') / family / 'configuration' / relative
    records.append({'sourcePath': str(path), 'path': target.as_posix(),
                    **retain(path, source_output / target, inputs)})
    return path


def meson_configuration(family, source, build, prefix, package, runtime, source_output, inputs):
    info = materials.meta.json_read(build / 'meson-info/meson-info.json')
    directories = info.get('directories', {})
    require(info.get('error') is not True and same_directory(directories.get('source'), source) and
            same_directory(directories.get('build'), build), 'Actual Meson source/build directories differ')
    options = materials.meta.json_read(build / 'meson-info/intro-buildoptions.json')
    values = {item['name']: item['value'] for item in options if item.get('machine') in (None, 'any', 'host')}
    require(same_directory(values.get('prefix'), prefix) and values.get('libdir') == 'lib',
            'Actual Meson installation prefix differs from its receipt')
    expected = package.get('mesonOptions', [
        '-Dbackend-qt6=enabled', '-Dbackend-qt5=disabled', '-Dbackend-gtk3=disabled', '-Dbackend-gtk4=disabled',
        '-Dintrospection=false', '-Dvapi=false', '-Ddocs=false', '-Dtests=false', '-Dportal-tests=false'])
    for option in expected:
        key, separator, value = option.removeprefix('-D').removeprefix('--').partition('=')
        require(separator, 'Unexpected locked Meson option')
        typed = {'false': False, 'true': True, '[]': []}.get(value, value)
        require(values.get(key) == typed, 'Actual Meson option differs from locked builder: ' + key)
    project = materials.meta.json_read(build / 'meson-info/intro-projectinfo.json')
    require(project.get('version') == package['version'], 'Actual Meson project version differs from the lock')
    installed = materials.meta.json_read(build / 'meson-info/intro-installed.json')
    require(isinstance(installed, dict), 'Expected original Meson install mapping')
    for item in runtime:
        original = Path(item['originalPath'])
        matches = [key for key, value in installed.items() if isinstance(value, str) and
                   Path(value).is_absolute() and Path(value).is_file() and Path(value).resolve(strict=True) == original]
        require(matches, 'Actual native runtime is absent from Meson install mapping: ' + item['path'])
        item['mesonInstallInputs'] = matches
    records = []
    for path in sorted((build / 'meson-info').glob('*.json')):
        configuration_file(build, path.relative_to(build).as_posix(), family, source_output, inputs, records)
    for relative in ('meson-private/cmd_line.txt', 'build.ninja'):
        configuration_file(build, relative, family, source_output, inputs, records)
    for relative in ('rules.ninja', 'config.h'):
        if (build / relative).is_file():
            configuration_file(build, relative, family, source_output, inputs, records)
    # Meson's original generated-source list identifies real inputs. Do not
    # mistake compiler parameter hints here for the exact build command; retain
    # build.ninja and the real compile_commands.json for that purpose.
    generated = set()
    for target in materials.meta.json_read(build / 'meson-info/intro-targets.json'):
        for language in target.get('target_sources', []):
            generated.update(language.get('generated_sources', []))
    retained = {item['sourcePath'] for item in records}
    for name in sorted(generated):
        path = Path(name)
        require(path.is_absolute() and path.resolve(strict=True).is_relative_to(build),
                'A generated Meson source is outside the original build tree')
        if str(path) not in retained:
            configuration_file(build, path.relative_to(build).as_posix(), family, source_output, inputs, records)
            retained.add(str(path))
    if (build / 'compile_commands.json').is_file():
        configuration_file(build, 'compile_commands.json', family, source_output, inputs, records)
    return {'kind': 'original-meson-introspection-and-build-files', 'mesonVersion': info.get('meson_version'),
            'files': records}


def make_values(path):
    text = path.read_text(encoding='utf-8', errors='strict')
    return text, {line.split('=', 1)[0]: line.split('=', 1)[1] for line in text.splitlines()
                  if re.match(r'^[A-Za-z_][A-Za-z_0-9]*=', line)}


def codec_configuration(family, source, build, prefix, arguments, source_output, inputs, *, cygpath=None, receipt=None):
    records = []
    relative = 'ffbuild/config.mak' if family == 'ffmpeg' else 'config.mak'
    configuration_file(build, relative, family, source_output, inputs, records)
    text, values = make_values(build / relative)
    def actual_path(value):
        require(isinstance(value, str) and value, 'Missing actual configure path')
        if cygpath is not None:
            value = materials.command(cygpath, '-a', '-w', value)
        path = Path(value)
        return (path if path.is_absolute() else build / path).resolve(strict=True)
    require(actual_path(values.get('SRC_PATH' if family == 'ffmpeg' else 'SRCPATH')) == source and
            actual_path(values.get('prefix')) == prefix, 'Actual configure source/prefix differs from receipt')
    if family == 'ffmpeg':
        require(shlex.split(values.get('FFMPEG_CONFIGURATION', '')) == arguments[1:],
                'Actual FFmpeg configure flags differ from the recorded build')
        required = ('config.h', 'config_components.h', 'libavutil/avconfig.h', 'libavutil/ffversion.h')
        optional = ('config.asm', 'config_components.asm', 'ffbuild/config.sh', 'ffbuild/config.fate')
        if cygpath is not None:
            require('CFLAGS=' + values.get('CFLAGS', '') == receipt.get('actualCflags') and
                    re.search(r'(?:^|\s)-MD(?:\s|$)', values.get('CFLAGS', '')) and
                    '-MDd' not in values['CFLAGS'] and '-MT' not in values['CFLAGS'],
                    'Actual Windows FFmpeg Release CRT differs from original receipt')
            configure_lines = (build / 'configure-fixed.sh').read_text(encoding='utf-8').splitlines()
            make_lines = (build / 'build-fixed.sh').read_text(encoding='utf-8').splitlines()
            require(len(configure_lines) == 3 and len(make_lines) == 4 and
                    configure_lines[0] == make_lines[0] == 'set -eu' and
                    shlex.split(configure_lines[2]) == arguments,
                    'Original Windows configure script differs from its controlled builder')
            for line in (configure_lines[1], make_lines[1]):
                change_directory = shlex.split(line)
                require(len(change_directory) == 2 and change_directory[0] == 'cd' and
                        actual_path(change_directory[1]) == build, 'Original native script changes a different directory')
            compile_command, install_command = shlex.split(make_lines[2]), shlex.split(make_lines[3])
            require(len(compile_command) == len(install_command) == 2 and
                    compile_command[0] == install_command[0] and install_command[1] == 'install' and
                    re.fullmatch(r'-j(?:[1-9]|[1-5][0-9]|6[0-4])', compile_command[1]) and
                    actual_path(compile_command[0]) == build.parent / 'tools/make/usr/bin/make.exe',
                    'Original native build script differs from the controlled private Make invocation')
            required += ('configure-fixed.sh', 'build-fixed.sh')
    else:
        require('default: lib-shared' in text.splitlines() and 'default: cli' not in text.splitlines(),
                'Actual x264 build is not the controlled shared-library profile')
        required, optional = ('config.h', 'x264_config.h', 'x264.pc'), ()
    for relative in required:
        configuration_file(build, relative, family, source_output, inputs, records)
    for relative in optional:
        if (build / relative).is_file():
            configuration_file(build, relative, family, source_output, inputs, records)
    return {'kind': 'original-configure-output', 'arguments': arguments, 'files': records,
            'configLogStatus': 'not-copied: FFmpeg configure dumps the shell environment into config.log'}


def runtime_family(relative, target):
    if target == 'windows':
        return 'ffmpeg' if re.fullmatch(r'(?:avcodec-63|avformat-63|avutil-61|swscale-10)\.dll', relative) else None
    if re.fullmatch(r'lib/libportal(?:-qt6)?\.so(?:\.[0-9]+)*', relative):
        return 'libportal'
    if (re.fullmatch(r'lib/libpipewire-0\.3\.so(?:\.[0-9]+)*', relative) or
            relative.startswith(('lib/pipewire-0.3/', 'lib/spa-0.2/')) or relative == 'share/pipewire/client.conf'):
        return 'pipewire'
    if re.fullmatch(r'lib/libx264\.so(?:\.[0-9]+)*', relative):
        return 'x264'
    if re.fullmatch(r'lib/lib(?:avcodec|avformat|avutil|swscale|swresample)\.so(?:\.[0-9]+)*', relative):
        return 'ffmpeg'
    return None


def runtime_inputs(installed, files, family, prefix, target, inputs):
    records = []
    for item in files:
        relative = item['path']
        if runtime_family(relative, target) != family:
            continue
        original = prefix / (Path('bin') / relative if target == 'windows' else Path(relative))
        original = original.resolve(strict=True)
        require(original.is_relative_to(prefix) and original.is_file(), 'Native runtime input escaped its build prefix')
        expected = {key: item[key] for key in ('size', 'sha256')}
        require(snapshot(original) == expected and materials.unchanged_digest(installed / relative) == expected,
                'Installed native runtime differs from its original prefix: ' + relative)
        if relative != 'share/pipewire/client.conf':
            require(materials.meta.machine(original) == target, 'Native runtime input has the wrong actual architecture')
        inputs[original] = expected
        records.append({'path': relative, 'originalPath': str(original), **expected})
    require(records, 'No actual installed runtime belongs to ' + family)
    return records


def collect(options):
    target = platform.system().lower()
    require(target in ('linux', 'windows') and platform.machine().lower() in ('amd64', 'x86_64'),
            'Run on the original Linux amd64 or Windows x64 builder')
    installed = materials.real_directory(options.installed)
    lock_path = ROOT / 'build/dependencies.lock.json'
    lock_digest = snapshot(lock_path)
    lock = materials.meta.json_read(lock_path)['qtMigration']
    marker = materials.meta.json_read(installed / ('installation.json' if target == 'windows' else 'bin/installation.json'))
    identity, files = materials.meta.audit_tree(installed, marker.get('packageKind'), development=True)
    require(identity['platform'] == target and identity['dependencyLockSha256'] == lock_digest['sha256'],
            'Installed product was not built against this exact native dependency lock')
    if target == 'linux':
        portal, pipewire, media = (materials.real_directory(path) for path in
                                   (options.portal_workspace, options.pipewire_workspace, options.media_workspace))
        families = [
            ('libportal', portal, portal / 'source', portal / 'build', portal / 'install', lock['libportal'], 'scripts/build-portal.py'),
            ('pipewire', pipewire, pipewire / 'source', pipewire / 'build', pipewire / 'install', lock['pipewire'], 'scripts/build-pipewire.py'),
            ('x264', media, media / 'x264-source', media / 'x264-build', media / 'install', lock['softwareMedia']['x264'], 'scripts/build-software-media.py'),
            ('ffmpeg', media, media / 'ffmpeg-source', media / 'ffmpeg-build', media / 'install', lock['softwareMedia']['ffmpeg'], 'scripts/build-software-media.py'),
        ]
    else:
        media = materials.real_directory(options.windows_media_workspace)
        families = [('ffmpeg', media, media / 'source', media / 'build-Release', media / 'install/Release',
                     lock['softwareMedia']['ffmpeg'], 'scripts/build-windows-video-inspection.py')]
    originals = [installed] + [row[1] for row in families]
    outputs = [options.output.resolve(), options.source_output.resolve()]
    require(not outputs[0].is_relative_to(outputs[1]) and not outputs[1].is_relative_to(outputs[0]),
            'Keep runtime evidence and full sources in separate empty directories')
    for path in outputs:
        require(all(not path.is_relative_to(original) and not original.is_relative_to(path) for original in originals),
                'Material outputs overlap an original installation or dependency workspace')
    output, source_output = materials.new_output(options.output), materials.new_output(options.source_output)
    inputs, records = {lock_path: lock_digest}, []
    report = {'schemaVersion': 1, 'scope': 'Original same-run native source, controlled patches, configuration and runtime bytes',
              'platform': target, 'buildIdentity': identity, 'nativeSourceCollectionComplete': False,
              'runtimeInputBindingComplete': False, 'correspondingSourceComplete': False,
              'reproducibleRebuildPerformed': False, 'families': records, 'failures': [],
              'bindingLimit': 'Current prefix/runtime byte equality and original builder records; Linux receipts have no build-time output hashes',
              'outstandingFamilies': ['project', 'qt-sdk-source-binding', 'qt-sdk-icu', 'rust-and-toolchain',
                                      'system-source-packages', 'appimage-runtime', 'build-tool-source-closure']}
    try:
        retain(lock_path, source_output / 'build/dependencies.lock.json', inputs)
        for family, workspace, source, build, prefix, package, builder in families:
            source, build, prefix = (materials.real_directory(path) for path in (source, build, prefix))
            require(all(path.is_relative_to(workspace) for path in (source, build, prefix)), 'Native workspace input escaped')
            receipt_path = workspace / 'build-info.json'
            receipt = materials.meta.json_read(receipt_path)
            actual = verify_receipt(family, receipt, package)
            record = {'name': family, 'collected': False,
                      'configuredLicense': (lock['videoInspectionWindows']['configuredLicense'] if target == 'windows' else
                                            package.get('configuredLicense', package.get('license', package.get('clientLicense'))))}
            records.append(record)
            receipt_name = Path('families') / family / 'build-info.json'
            record['originalBuildReceipt'] = {'path': receipt_name.as_posix(), **retain(receipt_path, source_output / receipt_name, inputs)}
            builder_name = Path('families') / family / 'builder.py'
            record['builder'] = {'sourcePath': builder, 'path': builder_name.as_posix(),
                                 **retain(ROOT / builder, source_output / builder_name, inputs)}
            runtime = runtime_inputs(installed, files, family, prefix, target, inputs)
            record['runtimeFiles'] = runtime
            cygpath, selected = None, actual
            if target == 'windows':
                selected = receipt['configurations']['Release']
                require(same_directory(selected.get('prefix'), prefix) and selected.get('crt') == 'MD',
                        'Production native FFmpeg requires the actual Release/MD build')
                cygpath = materials.real_directory(options.msys) / 'usr/bin/cygpath.exe'
                inputs[cygpath] = snapshot(cygpath)
                arguments = selected['configure']
                profile = lock['videoInspectionWindows']
                require(len(arguments) == 2 + len(profile['configure']) + 4 and
                        arguments[2:2 + len(profile['configure'])] == profile['configure'] and
                        arguments[-4:-1] == ['--extra-cflags=-MD', '--extra-cxxflags=-MD', '--extra-ldflags=-debug -opt:ref -opt:icf'] and
                        arguments[-1].startswith('--x86asmexe=') and arguments[1].startswith('--prefix='),
                        'Actual Windows codec arguments differ from the locked inspection profile')
                require(Path(materials.command(cygpath, '-a', '-w', arguments[0])).resolve(strict=True) == source / 'configure' and
                        Path(materials.command(cygpath, '-a', '-w', arguments[1].removeprefix('--prefix='))).resolve(strict=True) == prefix,
                        'Windows receipt points at a different original configure source or prefix')
                assembler = Path(materials.command(cygpath, '-a', '-w', arguments[-1].removeprefix('--x86asmexe='))).resolve(strict=True)
                require(assembler.name.lower() == 'nasm.exe' and assembler.is_relative_to(workspace / 'tools/nasm'),
                        'Original Windows assembler is not in its private restored NASM tree')
                inputs[assembler] = snapshot(assembler)
                record['pathTranslator'] = {'path': str(cygpath), **inputs[cygpath]}
                record['assembler'] = {'path': str(assembler), **inputs[assembler]}
                expected_files = {f'bin/{name}-{major}.dll' for name, major in
                                  [('avcodec', 63), ('avformat', 63), ('avutil', 61), ('swscale', 10)]}
                expected_files |= {'bin/' + name + '.lib' for name in ('avcodec', 'avformat', 'avutil', 'swscale')}
                actual_files = {Path(item['path']).as_posix(): item['sha256'] for item in selected['files']}
                require(set(actual_files) == expected_files and len(selected['files']) == len(expected_files),
                        'Windows original DLL/import-library receipt is incomplete')
                for relative, digest in actual_files.items():
                    path = prefix / relative
                    inputs[path] = snapshot(path)
                    require(inputs[path]['sha256'] == digest, 'Windows native output differs from its build-time hash')
                for name in ('nasm', 'make'):
                    require(receipt[name]['sha512'] == profile[name]['sha512'] and receipt[name]['url'] in profile[name]['urls'],
                            'Original Windows native build tool package differs from the lock')
                record['buildTimeOutputHashesVerified'] = True
            else:
                require(same_directory(receipt.get('prefix'), prefix), 'Linux original native build prefix differs')
                record['buildTimeOutputHashesVerified'] = False
                if family == 'libportal':
                    require(receipt.get('qtVersion') == lock['qt']['version'], 'libportal receipt used a different Qt SDK version')
                elif family == 'pipewire':
                    require(receipt.get('version') == package['version'] and receipt.get('mesonOptions') == package['mesonOptions'],
                            'PipeWire original build options/version differ from lock')
                else:
                    arguments = [str(source / 'configure'), '--prefix=' + str(prefix), *package['configure']]
                    require(actual.get('configure') == arguments, 'Original codec configure arguments differ from lock')
            if family in ('libportal', 'pipewire'):
                record['configuration'] = meson_configuration(family, source, build, prefix, package, runtime, source_output, inputs)
            else:
                record['configuration'] = codec_configuration(family, source, build, prefix, arguments, source_output, inputs,
                                                               cygpath=cygpath, receipt=selected)
            record['source'] = archive_source(family, source, package, output, source_output, inputs)
            record['collected'] = True
        require(all(snapshot(path) == digest for path, digest in inputs.items()), 'Original native inputs changed during collection')
        materials.meta.verify_inventory(installed, files)
        report['nativeSourceCollectionComplete'] = True
        report['runtimeInputBindingComplete'] = True
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError, tarfile.TarError) as error:
        report['failures'].append(str(error))
        raise
    finally:
        materials.meta.json_write(output / 'native-materials.json', report)
        materials.meta.json_write(source_output / 'native-sources.json', report)
    print('Collected actual native dependency source/configuration and runtime bytes; full corresponding-source closure remains incomplete.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installed', type=Path, required=True)
    parser.add_argument('--portal-workspace', type=Path, default=ROOT / '.deps/portal-validation')
    parser.add_argument('--pipewire-workspace', type=Path, default=ROOT / '.deps/pipewire-validation')
    parser.add_argument('--media-workspace', type=Path, default=ROOT / '.deps/media-validation')
    parser.add_argument('--windows-media-workspace', type=Path, default=ROOT / '.deps/video-inspection-windows')
    parser.add_argument('--msys', type=Path, default=Path('C:/msys64'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-output', type=Path, required=True)
    try:
        collect(parser.parse_args())
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError, tarfile.TarError) as error:
        print('Native material collection failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
