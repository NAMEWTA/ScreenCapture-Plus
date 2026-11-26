#!/usr/bin/env python3
"""Verify or publish one complete, immutable, same-source Qt release aggregate.

Publication is explicit and restricted to a successful canonical main workflow.
The separate P11a bridge exception retains only the old Windows five assets.
This command never clobbers assets, reuses a tag, repairs a draft, or turns
development metadata into release data.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import types

from version_source import read_version

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'NAMEWTA/huhucapture'
module_path = ROOT/'scripts/package-qt.py'
package = types.ModuleType('huhucapture_release_packages')
package.__file__ = str(module_path)
exec(compile(module_path.read_bytes(), str(module_path), 'exec'), package.__dict__)
require = package.meta.require


def command(arguments, *, payload=None):
    result = subprocess.run(arguments, cwd=ROOT, input=payload,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding='utf-8', errors='replace', timeout=1800)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {arguments[0]} {arguments[1]}\n{result.stderr}')
    return result.stdout


def api(endpoint, *, method='GET', body=None, pages=False):
    args = ['gh','api',f'repos/{REPOSITORY}/{endpoint}','--method',method]
    if pages: args += ['--paginate','--slurp']
    payload = None
    if body is not None:
        args += ['--input','-']; payload=json.dumps(body,ensure_ascii=False)
    return json.loads(command(args,payload=payload))


def declaration(manifest, expected):
    require(package.meta.HEX40.fullmatch(expected) is not None, 'An exact tested source SHA is required')
    require(manifest['sourceCommit']==expected and read_version(ROOT)==manifest['version'],
            'Release aggregate differs from the checked-out single version source')
    actual=command(['git','rev-parse','HEAD']).strip()
    require(actual==expected, 'Release checkout differs from the tested source SHA')
    require(not command(['git','status','--porcelain','--untracked-files=normal']).strip(),
            'Publication requires an unchanged clean checkout')
    value=package.meta.json_read(ROOT/'releases/publish.json',16384)
    tag=manifest['tag']
    require(set(value)=={'schemaVersion','tag','notes'} and type(value['schemaVersion']) is int and
            value['schemaVersion']==1 and value['tag']==tag and value['notes']==f'releases/{tag}.md',
            'The reviewed release declaration does not match this aggregate')
    notes=ROOT/value['notes']
    require(notes.is_file() and not notes.is_symlink() and 0<notes.stat().st_size<=1024*1024,
            'Matching release notes are missing, linked or oversized')
    body=notes.read_text(encoding='utf-8')
    require(body.strip(), 'Matching release notes must not be empty')
    return tag,body


def verify_bridge(path: Path, manifest, assets):
    evidence=package.meta.json_read(path,8*1024*1024)
    actual=evidence.get('actualCompletePackage')
    require(evidence.get('actualCompletePackageRun') is True and isinstance(actual,dict) and
            actual.get('actualAboutPassed') is True, 'Qt release requires its actual complete package and native About journey')
    for key in ('sourceCommit','version','tag'):
        require(actual.get(key)==manifest[key], 'Actual package evidence belongs to a different source or version')
    require(actual.get('manifestSha256')==assets['release-manifest.json']['sha256'],
            'Native About did not use these exact final aggregate metadata bytes')
    portable=next(item for item in manifest['assets'] if item['packageKind']=='portable-zip')
    require(actual.get('sha256')==portable['sha256'], 'Native About downloaded different package bytes')
    cases=evidence.get('cases',[])
    downloaded=[entry for entry in cases if entry.get('case')=='actual-complete-package']
    about=[entry for entry in cases if entry.get('case')=='native-about-actual-complete-package']
    require(len(downloaded)==1 and len(about)==1 and about[0].get('result')=='passed',
            'Actual complete-package or About evidence is absent/ambiguous')
    result=downloaded[0].get('result',{})
    require(result.get('stage')=='downloaded' and result.get('packageKind')=='portable-zip' and
            result.get('received')==portable['size'] and result.get('total')==portable['size'],
            'The actual native bridge did not finish this complete package')


def canonical_run(value, commit, workflow):
    repository=value.get('head_repository') or {}
    return (value.get('head_sha')==commit and value.get('head_branch')=='main' and
            value.get('path')=='.github/workflows/'+workflow and repository.get('full_name')==REPOSITORY and
            value.get('status')=='completed' and value.get('conclusion')=='success' and
            value.get('event') in ('push','workflow_dispatch'))


def verify_workflows(commit, application_run):
    require(application_run.isdecimal(), 'The successful formal application run ID is required')
    current=api(f'actions/runs/{application_run}')
    require(canonical_run(current,commit,'qt-application.yml') and current['event']=='workflow_dispatch',
            'The formal candidate did not come from a successful canonical main application dispatch')
    jobs=[]
    for page in api(f'actions/runs/{application_run}/jobs?per_page=100',pages=True): jobs += page['jobs']
    expected={'fixtures','linux','windows','legacy-mp3-linux','aggregate'}
    actual={job['name']:job for job in jobs}
    require(expected<=set(actual) and all(actual[name].get('conclusion')=='success' for name in expected),
            'Native application, installation, cross-platform media or formal aggregate job did not pass')
    checked={'qt-application.yml':int(application_run)}
    for workflow in ('qt-validation.yml','localsend-validation.yml','video-inspection-validation.yml','windows.yml'):
        runs=[]
        for page in api(f'actions/workflows/{workflow}/runs?head_sha={commit}&status=success&per_page=100',pages=True):
            runs += page['workflow_runs']
        valid=[run for run in runs if canonical_run(run,commit,workflow)]
        require(valid, 'No successful canonical same-source validation run: '+workflow)
        checked[workflow]=max(run['id'] for run in valid)
    return checked


def verify_published_bridge(version):
    value=package.meta.json_read(ROOT/'releases/bridge-publish.json',16384)
    require(isinstance(value,dict) and set(value)=={'schemaVersion','kind','tag','notes','previousRelease'} and
            type(value.get('schemaVersion')) is int and value['schemaVersion']==1 and
            value.get('kind')=='legacy-windows-bridge',
            'Qt first release requires the independently declared P11a transition')
    tag=value.get('tag')
    require(isinstance(tag,str) and tag.startswith('v') and package.meta.VERSION.fullmatch(tag[1:]) and
            tuple(map(int,tag[1:].split('.')))<tuple(map(int,version.split('.'))),
            'The published bridge version must precede this Qt release')
    bridge=api('releases/tags/'+tag)
    require(bridge.get('draft') is False and bridge.get('prerelease') is False and
            bridge.get('tag_name')==tag and bridge.get('published_at') and
            '<!-- huhucapture:legacy-windows-bridge:v1 -->' in (bridge.get('body') or ''),
            'P11a has not actually been published through its independent bridge gate')
    expected={'huhucapture.exe',f'huhucapture-{tag}-windows-x64.zip',
              f'huhucapture-{tag}-windows-x64-symbols.zip','build-info.json','SHA256SUMS.txt'}
    assets=bridge.get('assets',[])
    require(len(assets)==5 and {entry.get('name') for entry in assets}==expected and
            all(entry.get('state')=='uploaded' and type(entry.get('size')) is int and entry['size']>0 and
                isinstance(entry.get('digest'),str) and entry['digest'].startswith('sha256:') for entry in assets),
            'Published bridge no longer has its five original complete Windows assets')
    return bridge['html_url']


def publish(args, manifest, assets):
    require(os.environ.get('GITHUB_REPOSITORY')==REPOSITORY and os.environ.get('GITHUB_REF')=='refs/heads/main' and
            os.environ.get('GITHUB_EVENT_NAME')=='workflow_dispatch',
            'Publishing is restricted to the explicit canonical main release workflow')
    tag,body=declaration(manifest,args.source_commit)
    require(args.bridge_evidence is not None, 'Actual complete-package About evidence is required before any tag mutation')
    verify_bridge(args.bridge_evidence,manifest,assets)
    checked=verify_workflows(args.source_commit,args.application_run)
    bridge_release=verify_published_bridge(manifest['version'])
    existing=[]
    for page in api('releases?per_page=100',pages=True): existing += page
    require(not any(release.get('tag_name')==tag for release in existing),
            'This tag already has a release/draft; existing releases are immutable and drafts need explicit recovery')
    refs=api('git/matching-refs/tags/'+tag)
    require(not any(ref.get('ref')=='refs/tags/'+tag for ref in refs), 'The release tag already exists; tags are never reused or moved')
    require(api('git/ref/heads/main')['object']['sha']==args.source_commit,
            'Main advanced after validation; retain these candidate files and validate the current source')
    # Atomic create rejects another tag creator. No failure path deletes a ref
    # or draft whose continued ownership cannot be established automatically.
    api('git/refs',method='POST',body={'ref':'refs/tags/'+tag,'sha':args.source_commit})
    release=api('releases',method='POST',body={'tag_name':tag,'target_commitish':args.source_commit,
        'name':tag,'body':body,'draft':True,'prerelease':False,'make_latest':'false'})
    require(release.get('draft') is True and release.get('tag_name')==tag, 'New release was not created as the requested draft')
    print(f'Created unpublished draft {release["id"]}; failure leaves this draft and new tag for explicit recovery.',flush=True)
    command(['gh','release','upload',tag,*[str(args.package/name) for name in sorted(assets)],'--repo',REPOSITORY])
    uploaded=[]
    for page in api(f'releases/{release["id"]}/assets?per_page=100',pages=True): uploaded += page
    observed={item['name']:item for item in uploaded}
    require(len(observed)==len(uploaded) and set(observed)==set(assets), 'Uploaded draft asset names differ from the verified aggregate')
    for name,expected in assets.items():
        item=observed[name]
        require(item.get('state')=='uploaded' and item.get('size')==expected['size'] and
                item.get('digest')=='sha256:'+expected['sha256'], 'Uploaded draft asset does not match exact local bytes: '+name)
    package.verify_aggregate(args.package,expected_commit=args.source_commit,expected_version=manifest['version'])
    require(api('git/ref/tags/'+tag)['object']['sha']==args.source_commit and
            api('git/ref/heads/main')['object']['sha']==args.source_commit,
            'Source/tag changed during upload; the verified draft remains unpublished')
    published=api(f'releases/{release["id"]}',method='PATCH',body={'draft':False,'prerelease':False,'make_latest':'true'})
    require(published.get('draft') is False and published.get('prerelease') is False and published.get('tag_name')==tag,
            'GitHub did not return the requested published stable release')
    require(api('releases/latest').get('id')==release['id'], 'Published release was not observed as latest')
    print(json.dumps({'release':published['html_url'],'sourceCommit':args.source_commit,
                      'assetCount':len(assets),'validationRuns':checked,'bridgeRelease':bridge_release},indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('verify','publish'))
    parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--source-commit',required=True)
    parser.add_argument('--bridge-evidence',type=Path)
    parser.add_argument('--application-run',default='')
    args=parser.parse_args(); args.package=args.package.resolve()
    try:
        manifest,assets=package.verify_aggregate(args.package,expected_commit=args.source_commit,expected_version=read_version(ROOT))
        if args.bridge_evidence: verify_bridge(args.bridge_evidence,manifest,assets)
        if args.command=='publish': publish(args,manifest,assets)
        else: print(f'Verified {len(assets)} exact same-source release assets. No network mutation or publication was performed.')
        return 0
    except (ValueError,RuntimeError,OSError,KeyError,TypeError,subprocess.SubprocessError) as error:
        print('Qt release refused; existing tags/assets retained:',error,file=sys.stderr)
        return 1


if __name__=='__main__':
    raise SystemExit(main())
