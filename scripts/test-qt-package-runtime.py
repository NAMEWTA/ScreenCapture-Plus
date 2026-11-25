#!/usr/bin/env python3
"""Load genuine installed Qt/TLS libraries, excluding developer search paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--certificate',type=Path,required=True)
    parser.add_argument('--private-key',type=Path,required=True)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    prefix=args.prefix.resolve()
    if any(c in str(prefix) for c in ('\r','\n','\0')):raise ValueError('Runtime prefix must be one path')
    windows=sys.platform=='win32'
    receipt=prefix/('build-identity.json' if windows else 'bin/build-identity.json')
    identity=json.loads(receipt.read_text())
    environment=os.environ.copy()
    for name in list(environment):
        if name.startswith(('QT_','QML_','LD_')) or name in ('OPENSSL_CONF','OPENSSL_MODULES','SSL_CERT_FILE','SSL_CERT_DIR'):
            environment.pop(name)
    environment['QT_PLUGIN_PATH']=str(prefix/'plugins')
    if windows:
        # os.environ performs Windows' case-insensitive lookup, but copy()
        # returns a plain dict whose keys CPython normalizes to uppercase.
        system_value=os.environ.get('SystemRoot')
        if not system_value:raise ValueError('Windows did not supply its actual system directory')
        system=Path(system_value)
        if not system.is_absolute() or not (system/'System32').is_dir():
            raise ValueError('Windows system directory is not available')
        environment['PATH']=os.pathsep.join(map(str,(prefix,system/'System32',system)))
    else:
        environment['LD_LIBRARY_PATH']=str(prefix/'lib')
        environment['PATH']='/usr/bin:/bin'
    # Windows resolves app-local CRT before System32 only when this executable
    # lives beside the actual app. Add one exclusively owned probe temporarily,
    # leaving qt.conf and every product file intact; remove only that identity.
    work=prefix if windows else prefix/'bin'
    descriptor,name=tempfile.mkstemp(prefix='.huhu-qt-runtime-',suffix='.exe' if windows else '',dir=work)
    probe=Path(name);owned=os.fstat(descriptor)
    try:
        with os.fdopen(descriptor,'wb') as target,args.probe.open('rb') as source:shutil.copyfileobj(source,target,1024*1024)
        if not windows:probe.chmod(0o700)
        completed=subprocess.run([str(probe),str(prefix),str(args.certificate.resolve()),str(args.private_key.resolve()),identity['qtVersion']],
                                 cwd=work,env=environment,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
        if completed.stderr:print(completed.stderr,end='',file=sys.stderr)
        if completed.returncode:
            if completed.stdout:print(completed.stdout,end='')
            raise RuntimeError('Installed runtime probe failed with exit code '+str(completed.returncode))
        evidence=json.loads(completed.stdout)
        if evidence.get('untrustedPeerRejected') is not True:raise ValueError('No real TLS rejection evidence')
        evidence['moduleBytes']={}
        for name,location in evidence['modules'].items():
            path=Path(location);digest=hashlib.sha256()
            with path.open('rb') as source:
                while chunk:=source.read(1024*1024):digest.update(chunk)
            evidence['moduleBytes'][name]={'size':path.stat().st_size,'sha256':digest.hexdigest()}
        print(json.dumps(evidence,indent=2))
        if args.report:
            with args.report.open('x',encoding='utf-8') as output:json.dump(evidence,output,indent=2);output.write('\n')
    finally:
        current=probe.lstat()
        if (current.st_dev,current.st_ino)!=(owned.st_dev,owned.st_ino):
            raise RuntimeError('Probe filename identity changed; leaving it untouched')
        probe.unlink()
    return 0


if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,RuntimeError,subprocess.TimeoutExpired) as error:
        print('Installed Qt runtime validation failed:',error,file=sys.stderr);raise SystemExit(1)
