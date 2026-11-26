"""Exact input and output checks for the separately source-built type-2 runtime.

The old official asset remains a different, independently pinned input. A build
report cannot substitute a library name for an archive, CRT object or producer.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import runpy
import struct

ROOT = Path(__file__).resolve().parents[2]
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
PATCHES = runpy.run_path(str(ROOT / 'build/package/toolchain_patches.py'))
HEADERS = runpy.run_path(str(ROOT / 'build/package/linux_header_paths.py'))
CHECKOUT = runpy.run_path(str(ROOT / 'build/package/source_checkout.py'))
bundle = API['bundle']
SOURCE_IDS = {'appimage-toolchain-build', 'appimage-musl', 'appimage-gcc', 'appimage-binutils',
              'appimage-gmp', 'appimage-mpc', 'appimage-mpfr', 'appimage-linux-headers',
              'appimage-fuse', 'appimage-squashfuse', 'appimage-zstd', 'appimage-zlib', 'appimage-mimalloc'}
STATIC_IDS = {'appimage-musl', 'appimage-gcc', 'appimage-fuse', 'appimage-squashfuse',
              'appimage-zstd', 'appimage-zlib', 'appimage-mimalloc'}
PRODUCERS = ('scripts/build-appimage-runtime.py', 'build/package/appimage_runtime.py',
             'build/package/source_receipts.py', 'scripts/create-source-bundle.py',
             'build/package/toolchain_patches.py', 'third_party/appimage/toolchain-patches/cowpatch.sh',
             'third_party/appimage/toolchain-patches/upstream.json', 'build/package/linux_header_paths.py',
             'build/package/source_checkout.py', 'build/package/toolchain-eh-probe.cpp')
TOOLCHAIN_FILES = ('libgcc.a', 'libgcc_eh.a', 'crtbeginS.o', 'crtendS.o',
                   'rcrt1.o', 'crti.o', 'crtn.o', 'libc.a')
EH_COMPILE_FLAGS = ['-std=c++11', '-O2', '-fPIE', '-fexceptions', '-fno-inline']


def profile():
    lock_path = ROOT / 'build/package/dependencies.lock.json'
    lock = bundle.read_json(lock_path)
    value = lock.get('runtimeSourceBuild', {})
    bundle.require(value.get('schemaVersion') == 1 and value.get('target') == 'x86_64-linux-musl' and
                   set(value.get('sources', {})) == SOURCE_IDS,
                   'Review the exact complete Linux AppImage source/toolchain input set')
    runtime = lock['tools']['appimageRuntime']
    bundle.require(runtime['repository'] == 'https://github.com/AppImage/type2-runtime' and
                   runtime['commit'] == 'dd6cebedcbddde9c82f89b011e8e1d40b6e43868',
                   'The reviewed original type-2 runtime revision changed')
    for name, pin in value['sources'].items():
        HEADERS['source_profile'](name, pin)
        if 'sourceArchive' in pin:
            archive = pin['sourceArchive']
            bundle.require(archive['url'].startswith('https://') and bundle.HEX64.fullmatch(archive['sha256']) and
                           0 < archive['maximumBytes'] <= 256 * 1024**2,
                           'An original source archive lacks an exact bounded HTTPS pin: ' + name)
            bundle.relative(pin['archivePrefix'])
        else:
            bundle.require(pin['repository'].startswith('https://github.com/') and
                           pin['repository'].endswith('.git') and bundle.HEX40.fullmatch(pin['commit']),
                           'An original Git source lacks its official exact commit: ' + name)
            if name != 'appimage-toolchain-build':
                bundle.require(pin.get('tag') and bundle.HEX40.fullmatch(pin.get('tagObject', '')),
                               'The original library release tag is missing: ' + name)
        for patch in pin.get('patches', []):
            path = ROOT / bundle.relative(patch['path'])
            bundle.require(bundle.digest(path) == patch['sha256'], 'A reviewed upstream patch changed')
        if name in PATCHES['ARCHIVES']:
            PATCHES['validate_profile'](name, pin, ROOT)
        else:
            bundle.require('patchApplication' not in pin, 'Unexpected source patch application profile')
    configuration = value['toolchainConfiguration']
    bundle.require(configuration['common'] == ['--disable-nls'] and
                   configuration['musl'] == ['--disable-shared', 'CFLAGS=-fPIC'] and
                   configuration['gcc'] == ['--disable-libquadmath', '--disable-decimal-float',
                       '--disable-libitm', '--disable-fixed-point', '--disable-lto', '--without-isl', '--disable-shared',
                       'CFLAGS_FOR_TARGET=-g -O2 -fPIC',
                       'CXXFLAGS_FOR_TARGET=-g -O2 -fPIC'],
                   'The reviewed source-built static musl toolchain configuration changed')
    bundle.require(value['runtimeLinkProfile'] == ['-std=gnu99', '-Os', '-D_FILE_OFFSET_BITS=64', '-fPIE',
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-static',
                       '-Wall', '-Werror', '-static-pie'], 'The reviewed static PIE runtime link profile changed')
    return lock_path, lock, value


def file_identity(path):
    return API['identity'](Path(path))


def source_inventory(component, pin, directory):
    return HEADERS['source_inventory'](component, pin, directory, bundle.__dict__)


def verify_checkout_policy(checkout, retained):
    policy = CHECKOUT['verify_policy'](checkout)
    bundle.require(all(retained.get(key) == value for key, value in policy.items()) and
                   file_identity(checked_record(retained['retainedPolicy'])) ==
                       {key:policy[key] for key in ('size','sha256')} and
                   file_identity(checked_record(retained['retainedHelper'])) ==
                       file_identity(ROOT / 'build/package/source_checkout.py'),
                   'The original runtime Git checkout policy or retained implementing code changed')


def producer_identity():
    return {name: file_identity(ROOT / name) for name in PRODUCERS}


def compiler_file_result(name, reported, prefix, installed):
    """Only GCC's exact not-found result for the optional split EH archive is absent."""
    bundle.require(name in TOOLCHAIN_FILES, 'Unknown compiler support query')
    if name == 'libgcc_eh.a' and reported == name:
        return None
    path = Path(reported)
    bundle.require(path.is_absolute(), 'Compiler support query did not resolve an owned installed file: ' + name)
    path = path.resolve(strict=True)
    prefix = Path(prefix).resolve(strict=True)
    bundle.require(path.is_relative_to(prefix), 'Compiler support query escaped the new target toolchain: ' + name)
    relative = path.relative_to(prefix).as_posix()
    expected = installed.get(relative)
    bundle.require(expected is not None and file_identity(path) ==
                   {key: expected[key] for key in ('size', 'sha256')},
                   'Compiler support query has no matching actual installation output: ' + name)
    return {'path': str(path), **file_identity(path), 'producerOutput': relative, 'component': expected['component']}


def toolchain_probe_inputs(map_path, object_path, prefix, installed):
    """Inventory every actual LOAD, without predicting the unwinder's archive name."""
    map_path, object_path = Path(map_path), Path(object_path).resolve(strict=True)
    prefix = Path(prefix).resolve(strict=True)
    text = map_path.read_text(encoding='utf-8')
    records = []
    for path in link_loads(text):
        identity = file_identity(path)
        if path == object_path:
            record = {'kind': 'probe-object', 'path': str(path), **identity}
        else:
            bundle.require(path.is_relative_to(prefix), 'Toolchain probe linked an input outside its actual new installation')
            relative = path.relative_to(prefix).as_posix()
            owner = installed.get(relative)
            bundle.require(owner is not None and identity == {key: owner[key] for key in ('size', 'sha256')},
                           'Toolchain probe input has no matching actual installation output')
            record = {'kind': 'installed', 'path': str(path), **identity,
                      'producerOutput': relative, 'component': owner['component']}
            if path.suffix == '.a':
                record['members'] = archive_members(path)
                record['membersMentionedInMap'] = archive_member_mentions(text, path)
                bundle.require(set(record['membersMentionedInMap']) <= {entry['name'] for entry in record['members']},
                               'Toolchain probe map names a member absent from its actual archive')
        records.append(record)
    bundle.require(any(item['kind'] == 'probe-object' for item in records), 'Toolchain probe map omitted its actual object')
    return records


def verify_toolchain_probe(record, execution, prefix, installed):
    probe = bundle.read_json(checked_record(record))
    prefix = Path(prefix).resolve(strict=True)
    bundle.require(probe.get('schemaVersion') == 1 and probe.get('kind') == 'huhucapture-static-toolchain-eh-probe',
                   'The source-built compiler has no actual static exception probe')
    source, obj, binary, map_path = (checked_record(probe[key]) for key in ('source', 'object', 'binary', 'map'))
    directory = source.parent
    bundle.require(all(path.parent == directory for path in (obj, binary, map_path)) and
                   file_identity(source) == file_identity(ROOT / 'build/package/toolchain-eh-probe.cpp'),
                   'Toolchain exception probe source or output locations changed')
    compiler = prefix / 'bin/x86_64-linux-musl-gcc'
    cxx = prefix / 'bin/x86_64-linux-musl-g++'
    def actual_program(path):
        path = Path(path).resolve(strict=True)
        identity = file_identity(path)
        if path != binary:
            bundle.require(path.is_relative_to(prefix), 'Toolchain probe executed a program outside its new installation')
            owner = installed.get(path.relative_to(prefix).as_posix())
            bundle.require(owner is not None and identity == {key: owner[key] for key in ('size', 'sha256')},
                           'Toolchain probe program differs from its actual installation output')
        return {'path': str(path), **identity}
    observed = execution['executions']
    queries = probe.get('queries', [])
    bundle.require([item.get('requested') for item in queries] == list(TOOLCHAIN_FILES),
                   'Original compiler support queries are incomplete')
    for item in queries:
        index = item['commandIndex']
        bundle.require(type(index) is int and 0 <= index < len(observed), 'A compiler query has no actual execution')
        actual = observed[index]
        bundle.require(actual['exitCode'] == 0 and actual['arguments'] ==
                       [str(compiler), '-print-file-name=' + item['requested']] and
                       actual['tool'] == actual_program(compiler) and
                       Path(actual['stdout']).read_text(encoding='utf-8').strip() == item['reported'] and
                       item['file'] == compiler_file_result(item['requested'], item['reported'], prefix, installed),
                       'A compiler support query differs from its original executed result')
    expected = {
        'compile': [str(cxx), *EH_COMPILE_FLAGS, '-c', str(source), '-o', str(obj)],
        'link': [str(cxx), '-static-pie', str(obj), '-Wl,-Map=' + str(map_path), '-Wl,-t', '-o', str(binary)],
        'run': [str(binary)],
    }
    bundle.require(set(probe.get('commands', {})) == set(expected), 'Toolchain probe command evidence is incomplete')
    indices = [item['commandIndex'] for item in queries] + [probe['commands'][name] for name in ('compile', 'link', 'run')]
    bundle.require(all(type(index) is int for index in indices) and indices == sorted(set(indices)),
                   'Toolchain probe queries, compilation, linking and execution are not in their actual order')
    for name, arguments in expected.items():
        index = probe['commands'][name]
        bundle.require(type(index) is int and 0 <= index < len(observed), 'A toolchain probe command has no original execution')
        actual = observed[index]
        bundle.require(actual['exitCode'] == 0 and actual['arguments'] == arguments and actual['cwd'] == str(directory) and
                       actual['tool'] == actual_program(arguments[0]),
                       'Toolchain exception compile/link/run differs from its actual successful command')
    inspect_elf(binary, marked=False)
    actual_inputs = toolchain_probe_inputs(map_path, obj, prefix, installed)
    bundle.require(probe.get('inputs') == actual_inputs, 'Toolchain probe does not preserve every actual static link input')
    included = {item['path'] for item in actual_inputs}
    query_files = {item['requested']: item['file'] for item in queries}
    bundle.require(all(query_files[name]['path'] in included for name in ('libgcc.a', 'libc.a')),
                   'Toolchain exception probe omitted actual target compiler/libc support')
    return probe


def archive_members(path):
    """Read actual regular archive member bytes, including GNU/BSD long names."""
    data = Path(path).read_bytes()
    bundle.require(data[:8] == b'!<arch>\n', 'Static input is not a complete regular ar archive: ' + str(path))
    position, strings, members = 8, b'', []
    occurrences = {}
    while position < len(data):
        header = data[position:position + 60]
        bundle.require(len(header) == 60 and header[58:] == b'`\n', 'Malformed static archive member header')
        try:
            size = int(header[48:58].decode('ascii').strip())
            raw_name = header[:16].decode('ascii').strip()
        except (UnicodeError, ValueError) as error:
            raise ValueError('Malformed static archive member identity') from error
        start = position + 60
        payload = data[start:start + size]
        bundle.require(0 <= size <= len(data) and len(payload) == size, 'Truncated static archive member')
        position = start + size + (size % 2)
        bundle.require(position <= len(data), 'Truncated static archive padding')
        if raw_name == '//':
            strings = payload
            continue
        if raw_name in ('/', '/SYM64/'):
            continue
        if raw_name.startswith('#1/'):
            length = int(raw_name[3:])
            bundle.require(0 < length <= size, 'Invalid BSD archive member name')
            name = payload[:length].rstrip(b'\0').decode('utf-8')
            payload = payload[length:]
        elif raw_name.startswith('/'):
            offset = int(raw_name[1:])
            end = strings.find(b'/\n', offset)
            bundle.require(0 <= offset < len(strings) and end >= offset, 'Invalid GNU archive long-name reference')
            name = strings[offset:end].decode('utf-8')
        else:
            name = raw_name.removesuffix('/')
        bundle.relative(name)
        occurrences[name] = occurrences.get(name, 0) + 1
        members.append({'name': name, 'occurrence': occurrences[name], 'size': len(payload),
                        'sha256': hashlib.sha256(payload).hexdigest()})
    bundle.require(position == len(data), 'Static archive contains unaccounted trailing bytes')
    return members


def link_loads(map_text):
    names = re.findall(r'^LOAD (.+)$', map_text, re.MULTILINE)
    bundle.require(names, 'The actual GNU link map contains no LOAD input records')
    paths = []
    for name in names:
        path = Path(name)
        bundle.require(path.is_absolute() and path.suffix in ('.a', '.o') and
                       not any(character.isspace() for character in name),
                       'The runtime link map contains an uncontrolled or non-static input: ' + name)
        resolved = path.resolve(strict=True)
        bundle.require(resolved.is_file() and not path.is_symlink(), 'A static linker input is missing or linked')
        if resolved not in paths:
            paths.append(resolved)
    return paths


def archive_member_mentions(map_text, path):
    # GCC commonly spells its own paths as bin/../lib/gcc/.... Preserve those
    # actual LOAD spellings when matching member observations after resolving
    # the input's installation identity; never lose them through normalization.
    path = Path(path).resolve(strict=True)
    aliases = [name for name in re.findall(r'^LOAD (.+)$', map_text, re.MULTILINE)
               if Path(name).resolve(strict=True) == path]
    return sorted({member for name in aliases
                   for member in re.findall(re.escape(name) + r'\(([^)\r\n]+)\)', map_text)})


def inspect_elf(path, *, marked=True):
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(64)
    bundle.require(len(header) == 64 and header[:6] == b'\x7fELF\x02\x01' and
                   struct.unpack_from('<HH', header, 16) == (3, 62),
                   'The actual runtime must be an amd64 little-endian ELF static PIE')
    if marked:
        bundle.require(header[8:11] == b'AI\x02', 'The actual type-2 runtime magic is missing')
    program_offset, program_size, count = struct.unpack_from('<Q', header, 32)[0], *struct.unpack_from('<HH', header, 54)
    bundle.require(program_size == 56 and 0 < count <= 128, 'Unsupported ELF program-header table')
    with path.open('rb') as stream:
        stream.seek(program_offset)
        table = stream.read(program_size * count)
    bundle.require(len(table) == program_size * count, 'Truncated ELF program-header table')
    dynamic = []
    for offset in range(0, len(table), program_size):
        kind = struct.unpack_from('<I', table, offset)[0]
        bundle.require(kind != 3, 'Source runtime still has a dynamic interpreter')
        if kind == 2:
            file_offset, size = struct.unpack_from('<Q', table, offset + 8)[0], struct.unpack_from('<Q', table, offset + 32)[0]
            bundle.require(size <= 1024 * 1024 and size % 16 == 0, 'Invalid ELF dynamic table')
            with path.open('rb') as stream:
                stream.seek(file_offset)
                section = stream.read(size)
            bundle.require(len(section) == size, 'Truncated ELF dynamic table')
            dynamic.extend(struct.unpack_from('<QQ', section, position) for position in range(0, size, 16))
    bundle.require(not any(tag == 1 for tag, _ in dynamic), 'Source runtime has an unrecorded dynamic dependency')
    return file_identity(path)


def runtime_embedding(runtime, image):
    """Bind the appended runtime, permitting only appimagetool's MD5 field.

    The fixed appimagetool source always writes the first 16 bytes of
    .digest_md5. This is a recorded packaging transformation, not a second
    accepted runtime hash or a claim about the cryptographic value of MD5.
    """
    runtime, image = Path(runtime), Path(image)
    inspect_elf(runtime)
    before = runtime.read_bytes()
    with image.open('rb') as stream:
        after = stream.read(len(before))
    bundle.require(len(after) == len(before) and image.stat().st_size > len(before),
                   'The AppImage has no complete source runtime and appended payload')
    section_offset = struct.unpack_from('<Q', before, 40)[0]
    entry_size, count, string_index = struct.unpack_from('<HHH', before, 58)
    bundle.require(entry_size == 64 and 0 < string_index < count <= 4096 and
                   section_offset + count * entry_size <= len(before), 'Invalid runtime ELF section table')
    table = [struct.unpack_from('<IIQQQQIIQQ', before, section_offset + index * entry_size) for index in range(count)]
    strings = table[string_index]
    bundle.require(strings[4] + strings[5] <= len(before), 'Runtime ELF section names escape the actual input')
    names = before[strings[4]:strings[4] + strings[5]]
    matching = []
    for section in table:
        name_index = section[0]
        end = names.find(b'\0', name_index)
        bundle.require(name_index < len(names) and end >= name_index, 'Invalid runtime ELF section name')
        if names[name_index:end] == b'.digest_md5':
            matching.append(section)
    bundle.require(len(matching) == 1, 'The actual runtime needs one unambiguous .digest_md5 section')
    section = matching[0]
    offset = section[4]
    bundle.require(section[1] == 1 and section[5] >= 16 and 0 < offset <= len(before) - 16,
                   'Runtime MD5 field is not a complete file-backed ELF section')
    bundle.require(before[:offset] == after[:offset] and before[offset + 16:] == after[offset + 16:],
                   'Appimagetool changed source runtime bytes outside its original 16-byte MD5 field')
    return {'runtime': file_identity(runtime), 'embeddedRuntimeSha256': hashlib.sha256(after).hexdigest(),
            'image': file_identity(image), 'changes': [{'section': '.digest_md5', 'offset': offset, 'size': 16,
                'beforeHex': before[offset:offset + 16].hex(), 'afterHex': after[offset:offset + 16].hex()}],
            'meaning': 'Only the fixed appimagetool MD5 embedding differs; every other original runtime byte matches'}


def checked_record(record):
    original = Path(record['path'])
    bundle.require(original.is_absolute() and not original.is_symlink(), 'Build evidence path is relative or linked')
    path = original.resolve(strict=True)
    bundle.require(file_identity(path) == {key: record[key] for key in ('size', 'sha256')},
                   'Original runtime build evidence changed: ' + str(path))
    return path


def verify_execution(record):
    value = bundle.read_json(checked_record(record))
    commands, executions = value.get('commands'), value.get('executions')
    bundle.require(value.get('kind') == 'huhucapture-build-execution' and value.get('platform') == 'linux' and
                   value.get('commandOrigins') == {} and commands and len(commands) == len(executions) and
                   all(command['commandIndex'] == index and command['arguments'] == commands[index] and
                       type(command['exitCode']) is int and command['exitCode'] == 0
                       for index, command in enumerate(executions)),
                   'Runtime producer lacks every original successful executed command')
    retained = {item['inputPath']: item for item in value['records']}
    for item in value['records']:
        checked_record(item)
    for command in executions:
        for key in ('stdout', 'stderr'):
            path = Path(command[key])
            bundle.require(path.is_file() and not path.is_symlink(), 'Original runtime command log is missing or linked')
            if path.stat().st_size:
                bundle.require(command[key] in retained and file_identity(path) == file_identity(checked_record(retained[command[key]])),
                               'Original runtime command output differs from its retained bytes')
    return value


def validate_report(directory, report):
    """Validate actual evidence before the builder persists a success report."""
    directory = Path(directory).resolve(strict=True)
    lock_path, lock, specification = profile()
    bundle.require(report.get('schemaVersion') == 1 and report.get('kind') == 'huhucapture-built-appimage-runtime' and
                   report.get('platform') == 'linux' and report.get('architecture') == 'amd64' and
                   report.get('buildComplete') is True and report.get('staticClosureComplete') is True and
                   report.get('correspondingSourceComplete') is False and report.get('failures') == [],
                   'The AppImage runtime has no completed real static-source build')
    bundle.require(report.get('packageLockSha256') == bundle.digest(lock_path) and
                   report.get('producerFiles') == producer_identity() and
                   report.get('runtimeCommit') == lock['tools']['appimageRuntime']['commit'] and
                   report.get('sourcePins') == specification['sources'],
                   'The AppImage runtime was produced by another lock, source set or builder')
    inputs = report['sourceInputs']
    bundle.require(set(inputs) == SOURCE_IDS | {'appimage-runtime'} and
                   all(bundle.HEX64.fullmatch(value.get('sourceTreeSha256', '')) for value in inputs.values()),
                   'The original runtime/toolchain source manifests are incomplete')
    for name, value in inputs.items():
        original = bundle.read_json(checked_record(value['manifest']))
        pin = specification['sources'].get(name) or {**lock['tools']['appimageRuntime'],
            'archivePrefix': 'type2-runtime-' + lock['tools']['appimageRuntime']['commit']}
        bundle.require(original['id'] == name and original['pin'] == pin and
                       original['sourceTreeSha256'] == value['sourceTreeSha256'] and
                       bundle.tree_digest(original['files']) == value['sourceTreeSha256'],
                       'An actual source input manifest changed')
        bundle.require(source_inventory(name, pin, directory / 'sources' / name) == original['files'],
                       'The original runtime source input bytes changed')
        if 'sourceArchive' not in pin:
            checkout = directory / 'checkouts' / name
            verify_checkout_policy(checkout, original['sourceCheckout'])
            bundle.compare_checkout(checkout, original['files'], pin['commit'])
    receipts = {}
    executions, receipt_records = {}, {}
    for item in report['receipts']:
        identifier = item['id']
        bundle.require(identifier not in receipts, 'Duplicate runtime producer receipt')
        receipt = bundle.read_json(checked_record(item['receipt']))
        bundle.require(receipt.get('kind') == 'huhucapture-source-build-receipt' and receipt.get('platform') == 'linux',
                       'A runtime producer is not an original Linux source receipt')
        for source in receipt['sources']:
            bundle.require(source['sourceTreeSha256'] == inputs[source['id']]['sourceTreeSha256'],
                           'A runtime producer used another original source tree')
        for record in item['buildRecords']:
            checked_record(record)
        execution = verify_execution(item['execution'])
        bundle.require(execution['commands'] == receipt['commands'] and execution['toolchain'] == receipt['toolchain'] and
                       execution['inputs']['sources'] == receipt['sources'] and
                       receipt['environment'] == {'executions': [entry['environment'] for entry in execution['executions']]} and
                       receipt['outputs'] == [{key: entry[key] for key in ('path', 'size', 'sha256')}
                           for entry in execution['outputs']['files'] if entry['type'] == 'file' and entry['size']],
                       'A runtime producer lacks successful actual command evidence')
        receipts[identifier] = receipt
        executions[identifier], receipt_records[identifier] = execution, item
    bundle.require(set(receipts) == (STATIC_IDS - {'appimage-musl', 'appimage-gcc'}) | {'appimage-toolchain', 'appimage-runtime'} and
                   {source['id'] for receipt in receipts.values() for source in receipt['sources']} == set(inputs),
                   'The exact complete runtime/toolchain producer set is missing')
    control = verify_execution(report['controlExecution'])
    bundle.require(control['inputs']['sources'] == inputs and control['outputs'] ==
                   {'runtime': report['runtime'], 'linkClosure': report['linkClosure']},
                   'Runtime restoration and final build evidence disagree')
    for name in PATCHES['ARCHIVES']:
        original = bundle.read_json(checked_record(inputs[name]['manifest']))
        replay = bundle.read_json(checked_record(original['patchReplay']))
        PATCHES['verify_replay'](name, specification['sources'][name], ROOT, replay,
                                 original['files'], bundle.__dict__, control, directory / 'sources' / name)
    installation_owners = {}
    phases = executions['appimage-toolchain']['outputs']['installationPhases']
    expected_phases = [('appimage-musl', 'install-musl'), ('appimage-gcc', 'install-gcc'),
                       ('appimage-binutils', 'install-binutils'), ('appimage-linux-headers', 'install-kernel-headers')]
    bundle.require(len(phases) == len(expected_phases), 'The toolchain installation phases are incomplete')
    for phase, (component, target) in zip(phases, expected_phases):
        old = {entry['path']: entry for entry in phase['before']}
        changed = [entry for entry in phase['after'] if entry != old.get(entry['path'])]
        bundle.require(phase['component'] == component and phase['changed'] == changed and
                       executions['appimage-toolchain']['commands'][phase['commandIndex']][-1] == target,
                       'Toolchain output ownership differs from its actual installation commands')
        installation_owners.update({entry['path']: component for entry in changed if entry['type'] == 'file'})
    bundle.require(receipts['appimage-toolchain']['outputOwners'] == installation_owners,
                   'Compiler support/CRT ownership differs from actual source installation')
    installed = {entry['path']: {**entry, 'component': installation_owners[entry['path']]}
                 for entry in receipts['appimage-toolchain']['outputs']}
    verify_toolchain_probe(receipt_records['appimage-toolchain']['toolchainProbe'],
                           executions['appimage-toolchain'], receipt_records['appimage-toolchain']['prefix'], installed)
    closure_path = checked_record(report['linkClosure'])
    closure = bundle.read_json(closure_path)
    map_path = checked_record(closure['map'])
    map_text = map_path.read_text(encoding='utf-8')
    loads = {str(path) for path in link_loads(map_text)}
    actual_inputs = closure['inputs']
    bundle.require(len({item['id'] for item in actual_inputs}) == len(actual_inputs) == len(loads) and
                   {item['path'] for item in actual_inputs} == loads and
                   STATIC_IDS <= {item['component'] for item in actual_inputs},
                   'The complete actual link map, archives or compiler CRT sources are missing')
    for item in actual_inputs:
        path = checked_record(item)
        source = item['component']
        bundle.require(item['sourceTreeSha256'] == inputs[source]['sourceTreeSha256'],
                       'A linked input refers to another source tree')
        producer = receipts[item['producerReceipt']]
        bundle.require(any(entry['id'] == source for entry in producer['sources']) and
                       (Path(receipt_records[item['producerReceipt']]['prefix']) / item['producerOutput']).resolve() == path and
                       any(entry['path'] == item['producerOutput'] and all(entry[key] == item[key] for key in ('size', 'sha256'))
                           for entry in producer['outputs']),
                       'A static archive or CRT object has no matching actual source producer output')
        if item['producerReceipt'] == 'appimage-toolchain':
            bundle.require(installation_owners[item['producerOutput']] == source,
                           'A compiler support input was assigned to another original source producer')
        if path.suffix == '.a':
            bundle.require(item.get('members') == archive_members(path), 'Actual static archive member bytes changed')
            bundle.require(item.get('membersMentionedInMap') == archive_member_mentions(map_text, path),
                           'Static archive member observations differ from the actual link map')
    runtime_receipt = receipts['appimage-runtime']
    static = [{key: item[key] for key in ('id', 'component', 'sourceTreeSha256', 'size', 'sha256')} for item in actual_inputs]
    bundle.require(runtime_receipt['staticInputs'] == static, 'Runtime receipt and actual link-input closure disagree')
    runtime = checked_record(report['runtime'])
    notice = checked_record(report['notice'])
    bundle.require(runtime.is_relative_to(directory) and notice.is_relative_to(directory) and
                   inspect_elf(runtime) == file_identity(runtime) and
                   all(any(entry['path'] == str(path.relative_to(directory / 'install')) and
                               all(entry[key] == file_identity(path)[key] for key in ('size', 'sha256'))
                           for entry in runtime_receipt['outputs']) for path in (runtime, notice)),
                   'The selected runtime/notice is not the unchanged actual producer output')
    probe = bundle.read_json(checked_record(report['probe']))
    bundle.require(probe.get('extractionMatched') is True and probe.get('executionMatched') is True and
                   probe.get('runtimeSha256') == file_identity(runtime)['sha256'],
                   'The actual newly built runtime has no matching successful extraction/execution probe')
    image = checked_record(probe['appImage'])
    bundle.require(probe['runtimeEmbedding'] == runtime_embedding(runtime, image),
                   'Native runtime probe used another embedded runtime or an unrecorded packaging transformation')
    commands = executions['appimage-runtime']['commands']
    original = executions['appimage-runtime']['executions']
    build_index, extract_index, run_index = (probe[key] for key in ('buildCommandIndex', 'extractionCommandIndex', 'executionCommandIndex'))
    bundle.require(all(type(index) is int and 0 <= index < len(commands) for index in (build_index, extract_index, run_index)) and
                   build_index < extract_index < run_index and commands[extract_index] == [str(image), '--appimage-extract'] and
                   commands[run_index] == [str(image)] and
                   commands[build_index][1:] == ['--runtime-file', str(runtime), '--comp', 'zstd', '--mksquashfs-opt',
                       '-processors', '--mksquashfs-opt', '1', str(directory / 'probe/AppDir'), str(image)] and
                   all(original[build_index]['tool'][key] == lock['tools']['appimagetool'][key] for key in ('size', 'sha256')) and
                   original[run_index]['environment']['APPIMAGE_EXTRACT_AND_RUN'] == '1',
                   'Native runtime probe lacks its actual pinned packager/extraction/execution commands')
    output = checked_record(probe['stdout'])
    retained = executions['appimage-runtime']['records']
    bundle.require(output.read_text(encoding='utf-8').strip() == 'huhucapture-runtime-source-probe' and
                   any(item['inputPath'] == original[run_index]['stdout'] and Path(item['path']) == output for item in retained),
                   'Native AppRun success is not present in the unchanged original command output')
    payload = bundle.inventory(directory / 'probe/AppDir')
    bundle.require(bundle.tree_digest(payload) == probe['payloadTreeSha256'] and
                   payload == bundle.inventory(directory / 'probe/extract/squashfs-root'),
                   'Native runtime extraction evidence differs from the original complete payload')
    return report, runtime, notice


def verify_build(directory):
    """Only an unchanged successful report with complete real evidence is usable."""
    directory = Path(directory).resolve(strict=True)
    return validate_report(directory, bundle.read_json(directory / 'build-info.json'))
