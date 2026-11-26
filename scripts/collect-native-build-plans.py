#!/usr/bin/env python3
"""Select the existing same-build native producers for final source assembly.

This reads the original fixed native workspaces, verifies their current source
and output bytes against real producer records, and retains those records. It
does not compile, fabricate receipts, merge another vendor tree, or attribute
packaging-transformed files to earlier hashes.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import runpy
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
bundle = API['bundle']
native = runpy.run_path(str(ROOT / 'scripts/collect-native-source-materials.py'))


def native_inputs(platform, lock):
    migration = lock['qtMigration']
    rows = ([('ffmpeg', 'video-inspection-windows', 'source', 'install/Release',
              'scripts/build-windows-video-inspection.py')] if platform == 'windows' else [
        ('libportal', 'portal-validation', 'source', 'install', 'scripts/build-portal.py'),
        ('pipewire', 'pipewire-validation', 'source', 'install', 'scripts/build-pipewire.py'),
        ('x264', 'media-validation', 'x264-source', 'install', 'scripts/build-software-media.py'),
        ('ffmpeg', 'media-validation', 'ffmpeg-source', 'install', 'scripts/build-software-media.py')])
    result = []
    for component, name, source_name, prefix_name, builder in rows:
        workspace = (ROOT / '.deps' / name).resolve(strict=True)
        source = (workspace / source_name).resolve(strict=True)
        prefix = (workspace / prefix_name).resolve(strict=True)
        bundle.require(source.is_relative_to(workspace) and prefix.is_relative_to(workspace),
                       'Original native source/prefix escaped its build workspace')
        original = workspace / 'build-info.json'
        report_identity = native['snapshot'](original)
        value = bundle.read_json(original)
        bundle.require(native['snapshot'](original) == report_identity, 'Original native report changed while reading')
        pin = migration[component] if component in ('libportal', 'pipewire') else migration['softwareMedia'][component]
        selected = native['verify_receipt'](component, value, pin)
        if platform == 'windows':
            selected = value['configurations']['Release']
            bundle.require(selected.get('crt') == 'MD', 'Use only the original production Release/MD FFmpeg receipt')
            for name in ('nasm', 'make'):
                package = migration['videoInspectionWindows'][name]
                bundle.require(value[name]['sha512'] == package['sha512'] and value[name]['url'] in package['urls'],
                               'Original Windows native tool package differs from the lock')
        elif component == 'libportal':
            bundle.require(value['qtVersion'] == migration['qt']['version'], 'libportal used another Qt version')
        elif component == 'pipewire':
            bundle.require(value['version'] == pin['version'] and value['mesonOptions'] == pin['mesonOptions'],
                           'PipeWire used another version or actual profile')
        else:
            bundle.require(selected['configure'] == [str(source / 'configure'), '--prefix=' + str(prefix), *pin['configure']],
                           'Original native codec configure arguments differ from the lock')
        declared_prefix = selected['prefix'] if platform == 'windows' else value['prefix']
        bundle.require(Path(declared_prefix).is_absolute() and Path(declared_prefix).resolve(strict=True) == prefix,
                       'Original native report names another installed prefix')
        specification = selected.get('sourceBuildReceipt')
        bundle.require(specification is not None, component + ': original successful producer receipt is required')
        result.append({'id': component, 'workspace': workspace, 'source': source, 'prefix': prefix,
                       'report': original, 'reportIdentity': report_identity, 'selected': selected,
                       'receipt': specification, 'builder': ROOT / builder})
    return result


def bind_producer(component, specification, workspace, prefix, source, output, inputs, builder):
    """Retain a checked producer; the independent native collector checks its journal."""
    receipt_path = Path(specification['path'])
    bundle.require(receipt_path.is_absolute() and receipt_path.resolve(strict=True).is_relative_to(workspace),
                   'Native receipt escaped its original workspace')
    own_plan = receipt_path.parent / 'receipt-plan.json'
    bundle.require(bundle.read_json(own_plan) == specification,
                   'The original builder report and its actual receipt-plan disagree')
    receipt = bundle.read_json(receipt_path)
    bundle.require(receipt.get('staticInputs') == [], 'This fixed native profile has undeclared static inputs')
    files = []
    for entry in source['files']:
        item = {key: entry[key] for key in ('path', 'size', 'sha256')}
        if entry['type'] == 'symlink':
            item.update(mode='120000', linkTarget=entry['target'])
        else:
            item['mode'] = '100755' if entry['mode'] & 0o111 else '100644'
        files.append(item)
    inventory = output / 'families' / component / 'source-inputs.json'
    inventory.parent.mkdir(parents=True)
    API['write_new'](inventory, {'schemaVersion': 1, 'scope': 'Independently verified current source bytes',
                               'archiveFiles': files})
    runtime = []
    for item in receipt.get('outputs', []):
        relative = bundle.relative(item['path'])
        path = prefix / relative
        bundle.require(path.resolve(strict=True).is_relative_to(prefix), 'Native output escaped its original installed prefix')
        identity = API['identity'](path)
        inputs[path] = native['snapshot'](path)
        runtime.append({'path': relative, 'originalPath': str(path), **identity})
    bundle.require(runtime, 'The actual native producer recorded no outputs')
    bound = native['producer_receipt'](component, specification, workspace, prefix,
        {'sourceInventory': {'path': inventory.relative_to(output).as_posix()}}, runtime, output, inputs)
    bundle.require(bound is not None, 'The actual native producer receipt is missing')
    journals = [record for record in bound['buildRecords'] if record['path'].endswith('-build-execution.json')]
    execution = bundle.read_json(output / journals[0]['path'])
    bundle.require({key: execution['producer'][key] for key in ('size', 'sha256')} == API['identity'](builder),
                   'Actual native producer script differs from this checkout')
    bundle.require(execution.get('platform') == receipt.get('platform') and
                   execution.get('toolchain') == receipt.get('toolchain') and
                   receipt.get('environment') == {'executions': [e['environment'] for e in execution['executions']]} and
                   receipt.get('commandOrigins', {}) == execution.get('commandOrigins', {}) and
                   Path(execution['outputs']['prefix']).resolve(strict=True) == prefix,
                   'Native receipt toolchain, environment or output prefix differs from original execution')
    native['retain'](own_plan, output / 'families' / component / 'original-receipt-plan.json', inputs)
    return {'id': component, 'path': str(output / bound['receipt']['path']),
            'buildRecords': [{'path': str(output / record['path']), 'sha256': record['sha256']}
                             for record in bound['buildRecords']], 'staticFiles': {}}


def collect(output):
    output = output.resolve()
    bundle.require(output.is_relative_to(ROOT / 'artifacts') and output != ROOT / 'artifacts' and not output.exists(),
                   'Use a new fragment directory below this checkout\'s artifacts/')
    platform = 'windows' if os.name == 'nt' else 'linux'
    lock_path = ROOT / 'build/dependencies.lock.json'
    lock = bundle.read_json(lock_path)
    rows = native_inputs(platform, lock)
    output.mkdir(parents=True)
    inputs = {lock_path: native['snapshot'](lock_path)}
    plan = {'schemaVersion': 1, 'platform': platform, 'project': str(ROOT),
            'sources': [{'id': row['id'], 'directory': str(row['source'])} for row in rows],
            'buildReceipts': [], 'materials': []}
    reports = {}
    for row in rows:
        if row['report'] not in reports:
            path = output / 'materials' / (row['workspace'].name + '-build-info.json')
            bundle.require(native['retain'](row['report'], path, inputs) == row['reportIdentity'],
                           'Original build report changed before retention')
            plan['materials'].append({'id': row['workspace'].name + '-original-build', 'path': str(path)})
            reports[row['report']] = path
    with tempfile.TemporaryDirectory(prefix='checked-native-plan-', dir=output) as temporary:
        collector = bundle.Collector(plan, output, Path(temporary))
        collector.collect_project()
        for item in plan['sources']:
            collector.collect_source(item)
        for row in rows:
            plan['buildReceipts'].append(bind_producer(row['id'], row['receipt'], row['workspace'], row['prefix'],
                collector.components[row['id']], output, inputs, row['builder']))
            receipt = bundle.read_json(plan['buildReceipts'][-1]['path'])
            bundle.require(receipt['platform'] == platform, 'Native producer belongs to another platform')
            if row['id'] in ('ffmpeg', 'x264'):
                index = receipt['profileCommands'][row['id']]
                bundle.require(receipt['commands'][index] == row['selected']['configure'],
                               'Build-info configure arguments differ from the real producer command')
        collector.collect_receipts()
        allowed = {'huhucapture: actual matching build-identity.json is missing',
                   'huhucapture: matching source/build-parameter receipt is missing'}
        bundle.require(all(gap in allowed for gap in collector.gaps),
                       'Native producer fragment is incomplete: ' + '; '.join(collector.gaps))
        bundle.require(all(native['snapshot'](path) == identity for path, identity in inputs.items()),
                       'Original native build inputs changed while selecting their plans')
        for row in rows:
            component = collector.components[row['id']]
            bundle.compare_checkout(row['source'], component['files'], component['source']['commit'])
        bundle.compare_checkout(ROOT, collector.components['huhucapture']['files'], collector.identity['sourceCommit'])
        report = {'schemaVersion': 1, 'platform': platform, 'sourceCommit': collector.identity['sourceCommit'],
                  'sources': {row['id']: collector.components[row['id']]['sourceTreeSha256'] for row in rows},
                  'buildReceiptIds': sorted(collector.receipts),
                  'scope': 'Checked original native producer inputs and outputs; no complete source or deployment claim'}
    API['write_new'](output / 'selection-report.json', report)
    plan['materials'].append({'id': 'native-producer-selection', 'path': str(output / 'selection-report.json')})
    path = output / 'source-plan-fragment.json'
    API['write_new'](path, plan)
    print(path)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    try:
        collect(options.output)
        return 0
    except (ValueError, OSError, KeyError, TypeError, bundle.subprocess.SubprocessError) as error:
        print('Native producer plan selection failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
