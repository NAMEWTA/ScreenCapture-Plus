"""Retain Ubuntu's signed release and the full indexes that pin the actual inputs."""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlsplit


LISTS = Path('/var/lib/apt/lists')
KEYRING = Path('/usr/share/keyrings/ubuntu-archive-keyring.gpg')
APT_HELPER = Path('/usr/lib/apt/apt-helper')
INDEX_KEY = re.compile(r'(main|restricted|universe|multiverse)/(binary-amd64/Packages|source/Sources)')


def release_hashes(path, debian):
    records = debian.control_stanzas(path.read_text(encoding='utf-8'), clear_signed=True)
    debian.require(len(records) == 1 and records[0].get('origin') == 'Ubuntu', 'Expected an Ubuntu Release index')
    result = {}
    for line in records[0].get('sha256', '').splitlines():
        if not line.strip():
            continue
        fields = line.split()
        debian.require(len(fields) == 3 and debian.SHA256.fullmatch(fields[0]) and fields[1].isdigit(),
                       'Invalid SHA256 entry in the authenticated Ubuntu Release')
        checksum, size, key = fields
        debian.require(key not in result, 'Duplicate SHA256 entry in the authenticated Ubuntu Release')
        result[key] = {'size': int(size), 'sha256': checksum}
    debian.require(result, 'The authenticated Ubuntu Release contains no SHA256 index pins')
    return result


def selected_stanzas(index, wanted, debian):
    stanzas, block = {}, []
    with index.open(encoding='utf-8') as stream:
        for line in stream:
            if line != '\n':
                block.append(line)
                continue
            retain_stanza(block, wanted, stanzas, debian)
            block.clear()
    retain_stanza(block, wanted, stanzas, debian)
    return stanzas


def retain_stanza(block, wanted, stanzas, debian):
    package = next((line.removeprefix('Package: ').strip() for line in block if line.startswith('Package: ')), None)
    if package not in wanted:
        return
    content, = debian.control_stanzas(''.join(block))
    key = package, content.get('version')
    stanzas.setdefault(key, []).append(content)


def collect(output, execute, debian, binary_pins, source_pins, material):
    output.mkdir()
    raw, _ = execute(['apt-get', 'indextargets'], cwd=output, capture=True)
    targets = debian.control_stanzas(raw)
    wanted_binaries = {name for name, version in binary_pins}
    wanted_sources = {name for name, version in source_pins}
    bindings = {'binaries': {}, 'sources': {}}
    releases, keyring = {}, output / KEYRING.name
    shutil.copyfile(KEYRING, keyring)
    material('ubuntu-archive-keyring', keyring)
    for target in targets:
        key = target.get('metakey', '')
        if target.get('origin') != 'Ubuntu' or not INDEX_KEY.fullmatch(key):
            continue
        url = urlsplit(target['uri'])
        debian.require(url.scheme in ('http', 'https') and not url.username and not url.password,
                       'Unexpected Ubuntu APT index URI')
        original = Path(target['filename'])
        suffix = '_' + key.replace('/', '_')
        match = re.fullmatch(r'(.+)' + re.escape(suffix) + r'(?:\.[a-z0-9]+)?', original.name)
        debian.require(match and original.parent == LISTS and not original.is_symlink(),
                       'The reported Ubuntu index is outside the original local APT lists')
        if not original.is_file():
            # A target can be advertised without a downloaded index, including
            # empty pockets/packages. End-of-collection checks report any
            # actual required binary/source identity without a verified pin.
            continue
        release_name = match[1] + '_InRelease'
        if release_name not in releases:
            source = LISTS / release_name
            directory = output / str(len(releases)); directory.mkdir()
            release = directory / 'InRelease'
            pin = debian.identity(source)
            shutil.copyfile(source, release)
            debian.require(debian.identity(release) == pin, 'Original Ubuntu InRelease changed during retention')
            stdout, _ = execute(['gpgv', '--keyring', keyring, '--status-fd', '1', release], cwd=directory, capture=True)
            debian.require(any(line.startswith('[GNUPG:] VALIDSIG ') for line in stdout.splitlines()),
                           'Ubuntu InRelease has no valid signature from the archive keyring')
            signature = directory / 'signature-status.txt'
            signature.write_text(stdout, encoding='utf-8')
            identifier = 'ubuntu-release-' + directory.name
            releases[release_name] = directory, release_hashes(release, debian), identifier
            material(identifier, release)
            material(identifier + '-signature', signature)
        directory, hashes, release_id = releases[release_name]
        debian.require(key in hashes and 0 < hashes[key]['size'] <= debian.MAX_FILE,
                       'Ubuntu Release does not pin the complete observed APT index')
        index = directory / key.replace('/', '_')
        if index.exists():
            continue
        copy_index(original, index, hashes[key], debian)
        is_binary = key.endswith('/Packages')
        pins = binary_pins if is_binary else source_pins
        category = 'binaries' if is_binary else 'sources'
        index_id = release_id + '-' + key.replace('/', '-').lower()
        matches = selected_stanzas(index, wanted_binaries if is_binary else wanted_sources, debian)
        matched = False
        for package, records in matches.items():
            if package not in pins:
                continue
            expected = pins[package]
            if is_binary:
                valid = [record for record in records if record.get('architecture') == 'amd64' and
                         record.get('sha256') == expected['sha256'] and record.get('size') == str(expected['size'])]
            else:
                valid = [record for record in records if debian.checksum_files(record) == expected]
            if valid:
                matched = True
                bindings[category].setdefault('='.join(package), []).append({'release': release_id, 'index': index_id,
                    'indexKey': key, 'indexIdentity': hashes[key], 'uri': target['uri']})
        if matched:
            material(index_id, index)
        else:
            index.unlink()
    return bindings


def copy_index(source, output, expected, debian):
    checksum = hashlib.sha256()
    size = 0
    with subprocess.Popen([APT_HELPER, 'cat-file', source], stdout=subprocess.PIPE) as decoder, output.open('xb') as target:
        try:
            while block := decoder.stdout.read(1024 * 1024):
                size += len(block)
                debian.require(size <= expected['size'], 'Decoded Ubuntu APT index exceeds its authenticated size')
                target.write(block)
                checksum.update(block)
        finally:
            decoder.stdout.close()
    debian.require(decoder.returncode == 0 and {'size': size, 'sha256': checksum.hexdigest()} == expected,
                   'Original Ubuntu APT index does not match the authenticated Release SHA256')
