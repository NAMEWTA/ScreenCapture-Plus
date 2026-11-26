"""Verify and extract the exact Debian source package selected by an APT index.

The original Sources stanza binds the .dsc and each archive by SHA256. The .dsc
must independently name the same source/version and archive bytes. Extraction
uses dpkg-source, including Debian patches and build recipes, in a private copy.
This establishes retained source material, never an original binary build.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

PACKAGE = re.compile(r'[a-z0-9][a-z0-9+.-]+')
VERSION = re.compile(r'[0-9][A-Za-z0-9.+:~\-]*')
SHA256 = re.compile(r'[0-9a-f]{64}')
MAX_FILE = 1024 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= MAX_FILE,
            'A Debian source input is missing, empty, linked or oversized: ' + str(path))
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            value.update(data)
    return {'size': path.stat().st_size, 'sha256': value.hexdigest()}


def basename(value):
    require(isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.+~\-]*', value) and
            value not in ('.', '..'), 'Unsafe Debian source archive filename')
    return value


def control_stanzas(text, *, clear_signed=False):
    """Parse deb822 fields without evaluating data or claiming signature validity."""
    require(isinstance(text, str) and '\0' not in text, 'Invalid Debian control text')
    lines = text.splitlines()
    if lines and lines[0] == '-----BEGIN PGP SIGNED MESSAGE-----':
        require(clear_signed, 'Unexpected signed envelope in an APT source index')
        try:
            start = lines.index('') + 1
            end = lines.index('-----BEGIN PGP SIGNATURE-----', start)
            signature_end = lines.index('-----END PGP SIGNATURE-----', end)
        except ValueError as error:
            raise ValueError('Incomplete clear-signed Debian source description') from error
        require(not any(line.strip() for line in lines[signature_end + 1:]),
                'Trailing data after the Debian source signature')
        lines = [line[2:] if line.startswith('- ') else line for line in lines[start:end]]
    require(not any(line.startswith('-----BEGIN PGP') for line in lines),
            'Unexpected OpenPGP envelope in Debian control fields')
    result, stanza, current = [], {}, None
    for line in [*lines, '']:
        if not line.strip():
            if stanza:
                result.append(stanza)
                stanza, current = {}, None
            continue
        if line[0] in ' \t':
            require(current is not None, 'Orphan continuation in Debian control fields')
            stanza[current] += '\n' + line[1:]
            continue
        field, separator, value = line.partition(':')
        key = field.lower()
        require(separator and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]*', field) and key not in stanza,
                'Duplicate or invalid Debian control field')
        stanza[key], current = value.lstrip(), key
    return result


def checksum_files(stanza):
    value = stanza.get('checksums-sha256')
    require(isinstance(value, str) and value.strip(), 'Debian source input lacks Checksums-Sha256')
    result = {}
    for line in value.splitlines():
        if not line.strip():
            continue
        fields = line.split()
        require(len(fields) == 3 and SHA256.fullmatch(fields[0]) and fields[1].isdigit(),
                'Malformed Debian source SHA256 record')
        name, size = basename(fields[2]), int(fields[1])
        require(name not in result and 0 < size <= MAX_FILE, 'Duplicate or oversized Debian source archive')
        result[name] = {'sha256': fields[0], 'size': size}
    require(result, 'Debian source archive list is empty')
    return result


def selected_index(text, package, version):
    require(PACKAGE.fullmatch(package or '') and VERSION.fullmatch(version or ''),
            'Exact Debian source package/version is required')
    found = [item for item in control_stanzas(text) if item.get('package') == package and item.get('version') == version]
    require(found, 'The APT source index has no exact installed source package/version')
    files = checksum_files(found[0])
    require(all(checksum_files(item) == files for item in found),
            'APT indexes disagree about the exact source package bytes')
    require(sum(name.endswith('.dsc') for name in files) == 1,
            'The APT source index must name exactly one source description')
    return found[0], files


def validate_source(specification):
    dsc, index = Path(specification['dsc']), Path(specification['sourceIndex'])
    expected_dsc, expected_index = specification.get('sha256', ''), specification.get('sourceIndexSha256', '')
    require(SHA256.fullmatch(expected_dsc) and SHA256.fullmatch(expected_index),
            'Original Debian description and source index hashes are required')
    dsc_identity, index_identity = identity(dsc), identity(index)
    require(dsc_identity['sha256'] == expected_dsc and index_identity['sha256'] == expected_index,
            'Original Debian description or APT source index changed')
    package, version = specification['sourcePackage'], specification['sourceVersion']
    selected, indexed = selected_index(index.read_text(encoding='utf-8'), package, version)
    require(indexed.get(dsc.name) == dsc_identity,
            'Debian source description differs from its original APT index pin')
    stanzas = control_stanzas(dsc.read_text(encoding='utf-8'), clear_signed=True)
    require(len(stanzas) == 1 and stanzas[0].get('source') == package and stanzas[0].get('version') == version,
            'Debian source description names a different installed source package/version')
    description = stanzas[0]
    require(description.get('format') in ('1.0', '3.0 (native)', '3.0 (quilt)'),
            'Unsupported Debian source package format')
    archives = checksum_files(description)
    require(archives == {name: value for name, value in indexed.items() if name != dsc.name},
            'Debian source description and original APT index disagree about archive bytes')
    originals = [(dsc, dsc_identity), (index, index_identity)]
    for name, expected in sorted(archives.items()):
        path = dsc.parent / name
        require(identity(path) == expected, 'Debian source archive differs from its original SHA256: ' + name)
        originals.append((path, expected))
    return {'kind': 'debian-source', 'package': package, 'version': version, 'format': description['format'],
            'dsc': {'name': dsc.name, **dsc_identity}, 'sourceIndex': {'name': index.name, **index_identity},
            'archives': [{'name': name, **value} for name, value in sorted(archives.items())]}, originals


def extract_source(specification, destination, inventory, *, execute=None):
    provenance, originals = validate_source(specification)
    destination = Path(destination)
    require(not destination.exists(), 'Debian source extraction must use a new directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    executable = shutil.which('dpkg-source')
    require(executable is not None, 'Actual dpkg-source is required for Debian patch/application replay')
    tool = {'path': str(Path(executable).resolve()), **identity(Path(executable).resolve())}
    environment = os.environ.copy()
    environment.update(LC_ALL='C.UTF-8', DPKG_COLORS='never')
    for key in ('TAR_OPTIONS', 'GZIP', 'BZIP', 'BZIP2', 'XZ_DEFAULTS', 'XZ_OPT'):
        environment.pop(key, None)
    with tempfile.TemporaryDirectory(prefix='debian-originals-', dir=destination.parent) as temporary:
        private = Path(temporary)
        for source, expected in originals:
            if source == Path(specification['sourceIndex']):
                continue
            shutil.copyfile(source, private / source.name)
            require(identity(private / source.name) == expected, 'Original Debian source copy changed')
        arguments = [executable, '--no-copy', '--no-overwrite-dir', '--require-strong-checksums',
                     '--extract', str(private / provenance['dsc']['name']), str(destination)]
        if execute is None:
            result = subprocess.run(arguments, cwd=private, env=environment, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            require(result.returncode == 0, 'Actual dpkg-source extraction failed: ' + result.stderr[-5000:])
            stdout, stderr = result.stdout, result.stderr
        else:
            stdout, stderr = execute(arguments, cwd=private, env=environment, capture=True)
        provenance['extraction'] = {'tool': tool, 'arguments': [argument.replace(str(private), '$DSC_ROOT')
            .replace(str(destination), '$SOURCE_ROOT') for argument in arguments],
            'exitCode': 0, 'stdout': stdout, 'stderr': stderr}
    require(identity(Path(tool['path'])) == {key: tool[key] for key in ('size', 'sha256')},
            'The actual Debian source extractor changed while running')
    require(all(identity(path) == expected for path, expected in originals),
            'Original Debian source package changed during extraction')
    for name in ('debian/rules', 'debian/changelog', 'debian/copyright'):
        path = destination / name
        require(path.is_file() and not path.is_symlink() and path.stat().st_size,
                'Extracted Debian source lacks its original build recipe or notice: ' + name)
    require((destination / 'debian/changelog').read_text(encoding='utf-8').splitlines()[0]
            .startswith(provenance['package'] + ' (' + provenance['version'] + ') '),
            'Extracted Debian changelog disagrees with the exact source description')
    return provenance, inventory(destination), originals
