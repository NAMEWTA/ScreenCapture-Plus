#!/usr/bin/env python3
"""Restore exact reviewed native packagers and provenance, without executing them."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from urllib.request import Request, urlopen

ROOT=Path(__file__).resolve().parents[1]


def download(url, destination, digest, limit, exact=None):
    if not url.startswith('https://') or not re.fullmatch('[0-9a-f]{64}',digest or ''):
        raise ValueError('Every download needs a fixed HTTPS URL and reviewed SHA256')
    hashed=hashlib.sha256();length=0
    with urlopen(Request(url,headers={'User-Agent':'huhucapture-fixed-packagers'}),timeout=90) as response, destination.open('xb') as output:
        while chunk:=response.read(1024*1024):
            length+=len(chunk)
            if length>limit:raise ValueError('Asset exceeded its locked size budget')
            output.write(chunk);hashed.update(chunk)
    if hashed.hexdigest()!=digest or (exact is not None and length!=exact):
        raise ValueError('Downloaded bytes do not match the reviewed lock: '+destination.name)
    return {'path':destination.name,'url':url,'sha256':digest,'size':length}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--platform',choices=['linux','windows','all'],default='windows' if sys.platform=='win32' else 'linux')
    parser.add_argument('--qt-translations',type=Path,help='Optional real matching Qt SDK translations directory to preserve')
    parser.add_argument('--qt-translations-archive',type=Path,help='Optional original Qt SDK translation archive to preserve')
    parser.add_argument('--qt-translations-sha256',help='Reviewed original archive SHA256; required when preserving an archive')
    args=parser.parse_args()
    try:
        output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
        if any(output.iterdir()):raise ValueError('Use an empty output; previous files are retained')
        lock=json.loads((ROOT/'build/package/dependencies.lock.json').read_text())
        records=[]
        if args.platform in ('linux','all'):
            for key,spec in lock['tools'].items():
                if spec.get('bootstrapOnly'):raise ValueError('Bootstrap-only tool is not approved for restoration: '+key)
                folder=output/key;folder.mkdir()
                binary=download(spec['url'],folder/spec['name'],spec['sha256'],spec['size'],spec['size'])
                (folder/spec['name']).chmod(0o755)
                related=[]
                for source_key,name in [('sourceArchive','source.tar.gz'),('licenseRecord',spec['licenseFile'])]:
                    source=spec[source_key]
                    related.append(download(source['url'],folder/name,source['sha256'],source['size'],source['size']))
                records.append({'name':key,'repository':spec['repository'],'commit':spec['commit'],'tag':spec['tag'],
                                'binary':binary,'materials':related})
        if args.platform in ('windows','all'):
            spec=lock['windowsTools']['nsis'];folder=output/'nsis';folder.mkdir()
            actual=download(spec['url'],folder/spec['name'],spec['sha256'],spec['maximumBytes'])
            records.append({'name':'nsis','version':spec['version'],'binary':actual,'digestSource':spec['digestSource'],
                            'source':spec['source'],'license':'Retain license texts inside the exact NSIS distribution'})
        translations=None
        qt_version=json.loads((ROOT/'build/dependencies.lock.json').read_text())['qtMigration']['qt']['version']
        if args.qt_translations:
            source=args.qt_translations.resolve()
            if not source.is_dir():raise ValueError('Matching Qt translation directory is absent')
            files=sorted(source.glob('*.qm'))
            if not files:raise ValueError('Qt translation directory contains no actual compiled catalogs')
            destination=output/'qt-translations';destination.mkdir()
            translation_files=[]
            for path in files:
                if path.is_symlink() or not path.is_file():raise ValueError('Translation source must be regular files')
                shutil.copyfile(path,destination/path.name)
                translation_files.append({'name':path.name,'size':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
            translations={'qtVersion':qt_version,'files':translation_files}
        if args.qt_translations_archive:
            path=args.qt_translations_archive
            if not re.fullmatch('[0-9a-f]{64}',args.qt_translations_sha256 or ''):raise ValueError('Original translation archive needs its reviewed SHA256')
            digest=hashlib.sha256(path.read_bytes()).hexdigest()
            if digest!=args.qt_translations_sha256:raise ValueError('Original Qt translation archive changed')
            folder=output/'qt-translations-source';folder.mkdir()
            shutil.copyfile(path,folder/path.name)
            if translations is None:translations={'qtVersion':qt_version}
            translations['archive']={'name':path.name,'size':path.stat().st_size,'sha256':digest}
        (output/'manifest.json').write_text(json.dumps({'schemaVersion':1,'tools':records,'qtTranslations':translations},indent=2)+'\n')
        entries=[]
        for path in sorted(output.rglob('*')):
            if path.is_file():entries.append(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.relative_to(output).as_posix())
        (output/'SHA256SUMS.txt').write_text('\n'.join(entries)+'\n')
        print('Verified fixed packaging inputs restored; no packager or installer was executed.')
        return 0
    except (ValueError,OSError) as error:
        print('Packaging input restoration failed; partial files retained for inspection:',error,file=sys.stderr)
        return 1


if __name__=='__main__':raise SystemExit(main())
