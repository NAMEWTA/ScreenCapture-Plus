"""Replay the fixed musl-cross-make patch recipe and verify its complete output.

Only these three original archives use GNU patch's upstream matching semantics.
Every patch byte, pristine tree and complete patched tree remains exact.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

PROFILE = 'musl-cross-make-cowpatch-v1'
REPOSITORY = 'https://github.com/richfelker/musl-cross-make.git'
COMMIT = '5e8042e63de29f8473fc3b0b831a587ba284e1a8'
RECIPE = {
    'profile': PROFILE,
    'makefile': {'upstreamPath':'Makefile', 'blob':'d8b8d35e0ba41608f577122f857ec8abb752964a'},
    'cowpatch': {'path':'third_party/appimage/toolchain-patches/cowpatch.sh', 'upstreamPath':'cowpatch.sh',
                'blob':'c79c9d9a27c9180aa16dff14c80461c67b3027dd'},
    'arguments': ['-p1', '-N', '-t', '-F2'],
}
ARCHIVES = {
    'appimage-musl': ('musl-1.2.6', 'd585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a'),
    'appimage-gcc': ('gcc-9.4.0', 'c95da32f440378d7751dd95533186f7fc05ceb4fb65eb5b85234e6299eb9838e'),
    'appimage-binutils': ('binutils-2.44', '0cdd76777a0dfd3dd3a63f215f030208ddb91c2361d2bcc02acec0f1c16b6a2e'),
}
SHELL = 'set -eu\nexec sh "$1" -p1 -N -t -F2 < "$2"\n'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), 'Original patch input/tool must be a regular file')
    with path.open('rb') as stream:
        return {'size':path.stat().st_size, 'sha256':hashlib.file_digest(stream,'sha256').hexdigest()}


def blob(data):
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()


def validate_profile(component, pin, project):
    value = pin.get('patchApplication')
    require(component in ARCHIVES and isinstance(value,dict) and
            set(value)=={'profile','pristineTreeSha256','patchedTreeSha256'} and value['profile']==PROFILE and
            all(re.fullmatch('[0-9a-f]{64}',value[key]) for key in ('pristineTreeSha256','patchedTreeSha256')),
            'Unknown or incomplete original toolchain patch profile')
    prefix, archive_sha = ARCHIVES[component]
    require(pin.get('archivePrefix')==prefix and pin.get('sourceArchive',{}).get('sha256')==archive_sha,
            'The original patch recipe is limited to its reviewed exact source archive')
    provenance = json.loads((Path(project)/'third_party/appimage/toolchain-patches/upstream.json').read_text(encoding='utf-8'))
    require(provenance['repository']==REPOSITORY and provenance['commit']==COMMIT and provenance['application']==RECIPE,
            'The original canonical patch recipe provenance changed')
    expected = [item for item in provenance['patches'] if item['path'].startswith(
        'third_party/appimage/toolchain-patches/'+prefix+'/')]
    require(expected and [item['path'] for item in pin.get('patches',[])]==[item['path'] for item in expected] and
            [item['path'] for item in expected]==sorted(item['path'] for item in expected),
            'The complete original patch set or canonical concatenation order changed')
    cowpatch = Path(project)/RECIPE['cowpatch']['path']
    require(cowpatch.is_file() and not cowpatch.is_symlink() and blob(cowpatch.read_bytes())==RECIPE['cowpatch']['blob'],
            'The original copy-on-write patch script differs from its exact Git blob')
    return value, expected, cowpatch


def apply(component, destination, patches, pin, project, api, *, recorder=None, environment=None):
    value, expected, cowpatch = validate_profile(component,pin,project)
    require(len(patches)==len(expected), 'An original toolchain patch is missing')
    contents=[]
    for path, specification, original in zip(patches,pin['patches'],expected):
        data=Path(path).read_bytes()
        require(hashlib.sha256(data).hexdigest()==specification['sha256'] and blob(data)==original['blob'],
                'An original toolchain patch byte changed')
        contents.append(data)
    destination=Path(destination).resolve(strict=True)
    before=api['inventory'](destination)
    require(api['tree_digest'](before)==value['pristineTreeSha256'],
            'The complete original toolchain tree differs before patching')
    selected=dict(os.environ if environment is None else environment)
    for key in ('POSIXLY_CORRECT','VERSION_CONTROL','SIMPLE_BACKUP_SUFFIX','QUOTING_STYLE','PATCH_GET',
                'BASH_ENV','ENV','SHELL','LD_PRELOAD','LD_LIBRARY_PATH'):
        selected.pop(key,None)
    selected.update(PATH=os.defpath,LC_ALL='C')
    patch_program=shutil.which('patch',path=selected['PATH'])
    shell_program=shutil.which('sh',path=selected['PATH'])
    require(patch_program and shell_program, 'The original runtime patch recipe requires real GNU patch and sh')
    tools={name:{'path':str(Path(path).resolve()),**identity(Path(path).resolve())}
           for name,path in (('patch',patch_program),('sh',shell_program))}
    observations=[]
    def run(arguments):
        arguments=list(map(str,arguments))
        if recorder is None:
            result=subprocess.run(arguments,cwd=destination,env=selected,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=False)
            stdout,stderr=result.stdout.decode('utf-8','replace'),result.stderr.decode('utf-8','replace')
            observations.append({'arguments':arguments,'exitCode':result.returncode,'stdout':stdout,'stderr':stderr})
            if result.returncode:
                raise ValueError('Original GNU patch recipe failed: '+stdout+stderr)
        else:
            index=len(recorder.commands)
            stdout,stderr=recorder.run(arguments,cwd=destination,env=selected,capture=True)
            observations.append({'arguments':arguments,'exitCode':0,'stdout':stdout,'stderr':stderr,
                                 'executionCommandIndex':index})
        return stdout,stderr
    version,_=run([patch_program,'--version'])
    require(version.startswith('GNU patch '), 'This source recipe requires GNU patch, not another patch implementation')
    with tempfile.TemporaryDirectory(prefix='original-toolchain-patches-',dir=destination.parent) as temporary:
        temporary=Path(temporary)
        combined=temporary/'original-combined.patch';combined.write_bytes(b''.join(contents))
        original_cow=temporary/'cowpatch.sh';original_cow.write_bytes(cowpatch.read_bytes())
        inputs={'patchSet':identity(combined),'cowpatch':identity(original_cow)}
        if recorder is not None:
            recorder.retain_record(Path(__file__).resolve())
            original_cow=Path(recorder.retain_record(original_cow)['path'])
            combined=Path(recorder.retain_record(combined)['path'])
            recorder.observe_tool(patch_program,selected)
        run([shell_program,'-c',SHELL,'huhucapture-original-cowpatch',original_cow,combined])
        require(inputs=={'patchSet':identity(combined),'cowpatch':identity(original_cow)},
                'Original patch recipe inputs changed during execution')
    after=api['inventory'](destination)
    require(api['tree_digest'](after)==value['patchedTreeSha256'],
            'The complete patched toolchain tree differs, including copy-on-write/backup files')
    require(all(identity(Path(item['path']))=={key:item[key] for key in ('size','sha256')} for item in tools.values()),
            'The actual patch tool changed during execution')
    return {'schemaVersion':1,'kind':'huhucapture-original-toolchain-patch-replay',
            'component':component,'profile':value,'recipe':RECIPE,'inputs':inputs,'tools':tools,'executions':observations,
            'environment':{'PATH':selected['PATH'],'LC_ALL':selected['LC_ALL']},
            'pristineFiles':len(before),'patchedFiles':len(after),
            'backupFiles':[entry for entry in after if entry['path'].endswith(('.orig','.rej'))],
            'scope':'Original combined upstream patch recipe on a complete independent source copy; every resulting source/link/backup byte is pinned'}


def verify_replay(component, pin, project, replay, files, api, execution, source_directory):
    """Bind the source manifest to the original successful executed commands."""
    value, expected, cowpatch = validate_profile(component,pin,project)
    combined=b''.join((Path(project)/item['path']).read_bytes() for item in expected)
    require(replay.get('schemaVersion')==1 and replay.get('kind')=='huhucapture-original-toolchain-patch-replay' and
            replay.get('component')==component and replay.get('profile')==value and replay.get('recipe')==RECIPE and
            replay.get('inputs')=={'patchSet':{'size':len(combined),'sha256':hashlib.sha256(combined).hexdigest()},
                                  'cowpatch':identity(cowpatch)} and
            api['tree_digest'](files)==value['patchedTreeSha256'] and replay.get('patchedFiles')==len(files) and
            replay.get('backupFiles')==[entry for entry in files if entry['path'].endswith(('.orig','.rej'))],
            'The original patch replay differs from its exact complete source output')
    observed=replay.get('executions',[])
    require(len(observed)==2 and observed[0]['arguments'][1:]==['--version'] and
            observed[0]['stdout'].startswith('GNU patch ') and
            len(observed[1]['arguments'])==6 and
            observed[1]['arguments'][1:4]==['-c',SHELL,'huhucapture-original-cowpatch'] and
            replay.get('environment')=={'PATH':os.defpath,'LC_ALL':'C'},
            'The original GNU patch recipe or tool version evidence is missing')
    for item, tool in zip(observed,('patch','sh')):
        index=item.get('executionCommandIndex')
        require(type(index) is int and 0 <= index < len(execution['executions']),
                'The patch replay has no original execution command')
        actual=execution['executions'][index]
        require(item['exitCode']==actual['exitCode']==0 and item['arguments']==actual['arguments'] and
                actual['cwd']==str(Path(source_directory).resolve(strict=True)) and
                actual['tool']==replay['tools'][tool] and
                all(actual['environment'].get(key)==value for key,value in replay['environment'].items()) and
                all(item[key]==Path(actual[key]).read_text(encoding='utf-8',errors='replace')
                    for key in ('stdout','stderr')),
                'The patch replay differs from the actual original command and complete output')
    retained={item['path']:item for item in execution['records']}
    for argument,kind in zip(observed[1]['arguments'][4:],('cowpatch','patchSet')):
        require(argument in retained and identity(Path(argument))==replay['inputs'][kind] and
                all(retained[argument][key]==replay['inputs'][kind][key] for key in ('size','sha256')),
                'The actual patch command did not use its unchanged retained input bytes')
