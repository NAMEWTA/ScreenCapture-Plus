#!/usr/bin/env python3
"""Bind supplied source/build materials to one actual frozen package preparation.

This does not build missing components, infer source versions, create build
receipts, or approve a release. The existing collector independently validates
every source and receipt before any final-file mapping is proposed.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import tempfile
import types

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
PACKAGE = ROOT / 'scripts/package-qt.py'
package = types.ModuleType('huhucapture_source_plan_package')
package.__file__ = str(PACKAGE)
exec(compile(PACKAGE.read_bytes(), str(PACKAGE), 'exec'), package.__dict__)
bundle = package.source_inventory_api()


def absolute(value, base):
    bundle.require(isinstance(value, str) and value, 'A material path must be nonempty')
    path = Path(value)
    return str((path if path.is_absolute() else base / path).resolve())


def retain_execution(directory, expected, archive):
    bundle.require(all(item['type']=='file' for item in expected) and bundle.inventory(directory)==expected,
                   'Original packaging execution files changed or are not regular files')
    # Empty stdout/stderr logs are real evidence too. Individual materials must
    # be nonempty, so retain the complete tree in one verified archive instead
    # of dropping empty files or weakening the material-input contract.
    actual=bundle.make_archive(directory,archive,0)
    bundle.require(bundle.inventory(directory)==expected,
                   'Original packaging execution files changed while archiving')
    return actual


def relocate_receipt(value, base):
    value = copy.deepcopy(value)
    value['path'] = absolute(value['path'], base)
    for record in value.get('buildRecords', []):
        record['path'] = absolute(record['path'], base)
    value['staticFiles'] = {name: absolute(path, base) for name, path in value.get('staticFiles', {}).items()}
    return value


def relocate_plan(value, base):
    allowed = {'schemaVersion', 'platform', 'project', 'buildIdentity', 'sources',
               'rustVendor', 'materials', 'buildReceipts'}
    bundle.require(isinstance(value, dict) and set(value) <= allowed and value.get('schemaVersion') == 1,
                   'Supply source/material plans before deployment binding; approval and complete fields are forbidden')
    value = copy.deepcopy(value)
    value['project'] = absolute(value['project'], base)
    if value.get('buildIdentity'):
        value['buildIdentity'] = absolute(value['buildIdentity'], base)
    for source in value.get('sources', []):
        for key in ('directory', 'archive', 'sourceSbom', 'dsc', 'sourceIndex'):
            if source.get(key):
                source[key] = absolute(source[key], base)
        if source.get('pinEvidence'):
            source['pinEvidence']['path'] = absolute(source['pinEvidence']['path'], base)
        for patch in source.get('patches', []):
            patch['path'] = absolute(patch['path'], base)
    if value.get('rustVendor'):
        vendor = value['rustVendor']
        for key in ('directory', 'registryCache', 'config'):
            if vendor.get(key):
                vendor[key] = absolute(vendor[key], base)
        if vendor.get('replay'):
            vendor['replay'] = {key: absolute(path, base) for key, path in vendor['replay'].items()}
        # noticeReviews and licenseTexts[*].path refer to committed source
        # contents, not paths relative to the input plan.
    for material in value.get('materials', []):
        material['path'] = absolute(material['path'], base)
    value['buildReceipts'] = [relocate_receipt(item, base) for item in value.get('buildReceipts', [])]
    return value


def merge_plans(plans):
    bundle.require(plans, 'At least one actual source/material plan is required')
    result = {'schemaVersion': 1, 'platform': plans[0]['platform'], 'project': plans[0]['project']}
    for plan in plans:
        bundle.require(plan['platform'] == result['platform'] and plan['project'] == result['project'],
                       'Source/material plans must refer to the same project checkout and platform')
        for key in ('buildIdentity', 'rustVendor'):
            if key in plan:
                bundle.require(key not in result or result[key] == plan[key], 'Conflicting source-plan input: ' + key)
                result[key] = plan[key]
        for key in ('sources', 'materials', 'buildReceipts'):
            indexed = {item['id']: item for item in result.get(key, [])}
            for item in plan.get(key, []):
                identifier = item['id']
                bundle.require(identifier not in indexed or indexed[identifier] == item,
                               'Conflicting source-plan component/material/receipt: ' + identifier)
                indexed[identifier] = item
            result[key] = [indexed[identifier] for identifier in sorted(indexed)]
    return result


def installed_notice_origins(name, candidates):
    _, boundary, notice = name.partition('/licenses/')
    if not boundary:
        return []
    if notice.startswith('project/'):
        expected = {'component': 'huhucapture', 'path': 'third_party/' + notice.removeprefix('project/')}
        return [candidate for candidate in candidates if candidate == expected]
    qt_prefix = 'collected-qt/qt/modules/'
    if notice.startswith(qt_prefix):
        component, _, path = notice.removeprefix(qt_prefix).partition('/')
        expected = {'component': component, 'path': path}
        return [candidate for candidate in candidates if candidate == expected]
    crate_prefix = 'collected-rust/rust/crates/'
    if notice.startswith(crate_prefix):
        expected = {'component': 'rust-vendor', 'path': notice.removeprefix(crate_prefix)}
        return [candidate for candidate in candidates if candidate == expected]
    rust_prefix = 'collected-rust-toolchain/rust-toolchain/rust-src/'
    if notice.startswith(rust_prefix):
        compact_path = notice.removeprefix(rust_prefix)
        result = []
        for candidate in candidates:
            if candidate.get('component') == 'rust-toolchain-source':
                path = candidate['path']
                archive_id = hashlib.sha256(path.encode('utf-8')).hexdigest()[:16]
                if compact_path == archive_id + '/' + PurePosixPath(path).name:
                    result.append(candidate)
        return result
    return []


def proposed_mapping(directory, files, components, receipts, products, supplied=None, transformations=None,
                     microsoft_runtime=None):
    """Use only verified equal bytes; ambiguity stays a gap for explicit review."""
    supplied = supplied or {}
    bundle.require(isinstance(supplied, dict) and set(supplied) <= {item['path'] for item in files},
                   'An explicit mapping names a file absent from the frozen deployment')
    source_index, output_index = {}, {}
    for identifier, component in components.items():
        for entry in component['files']:
            if entry['type'] == 'file':
                source_index.setdefault((entry['size'], entry['sha256']), []).append(
                    {'component': identifier, 'path': entry['path']})
    for identifier, receipt in receipts.items():
        for entry in receipt['outputs']:
            output_index.setdefault((entry['size'], entry['sha256']), []).append(
                {'receipt': identifier, 'output': entry['path']})
    applications = {str(PurePosixPath(item['root']) / item['executable']) for item in products}
    mapping, unresolved = {}, []
    for entry in files:
        name = entry['path']
        if name in supplied:
            mapping[name] = supplied[name]
            continue
        if entry['type'] == 'symlink':
            mapping[name] = {'linkTarget': entry['target']}
            continue
        runtime = (microsoft_runtime or {}).get(PurePosixPath(name).name)
        if runtime and name in ('installer/' + runtime['name'], 'portable-zip/' + runtime['name']):
            if all(runtime[field] == entry[field] for field in ('size', 'sha256')):
                mapping[name] = {'microsoftRuntime': runtime['name']}
            else:
                unresolved.append({'path': name, 'reason': 'Microsoft runtime differs from its verified original input',
                                   'candidateCount': 0, 'candidates': []})
            continue
        control_origins = [identifier for identifier, item in (transformations or {}).items()
                           if name in item.get('debControl', {})]
        if len(control_origins) == 1:
            mapping[name] = {'debControl': control_origins[0]}
            continue
        generator_origins = [identifier for identifier, item in (transformations or {}).items()
                             if name in item.get('linuxdeployGenerated', {})]
        if len(generator_origins) == 1:
            mapping[name] = {'linuxdeployGenerated': generator_origins[0]}
            continue
        observers = [(identifier, item) for identifier, item in (transformations or {}).items()
                     if name in item.get('observedPaths', [])]
        if observers:
            candidates = []
            for identifier, item in observers:
                bound = item['origins'].get(name)
                if bound is None:
                    continue
                bundle.require(bound['input'].get('kind') == 'observed-input' and
                               all(bound['output'][field] == entry[field] for field in ('path', 'size', 'sha256')),
                               'First observed transformation input differs from the verified final bytes')
                producer = bound['producer']
                if name in applications and not any(source['id'] == 'huhucapture'
                        for source in receipts[producer['receipt']]['sources']):
                    continue
                candidates.append({'transformation': identifier, 'output': name, 'input': producer})
            if len(candidates) == 1:
                mapping[name] = candidates[0]
            else:
                unresolved.append({'path': name, 'reason': 'first observed input has no unique continuous source build origin',
                                   'candidateCount': len(candidates), 'candidates': candidates[:8]})
            continue
        key = (entry['size'], entry['sha256'])
        candidates = output_index.get(key, [])
        if name in applications:
            candidates = [item for item in candidates if any(source['id'] == 'huhucapture'
                          for source in receipts[item['receipt']]['sources'])]
        with (directory / name).open('rb') as stream:
            prefix = stream.read(4)
        # A source tree containing a prebuilt binary does not establish its
        # build lineage. Native programs/libraries require a build receipt.
        native = prefix.startswith((b'MZ', b'\x7fELF'))
        if not candidates and not native and name not in applications:
            candidates = source_index.get(key, [])
        if len(candidates) > 1:
            exact_path = installed_notice_origins(name, candidates)
            if not exact_path:
                exact_path = [item for item in candidates if name == item.get('output', item.get('path')) or
                              name.endswith('/' + item.get('output', item.get('path')))]
            if len(exact_path) == 1:
                candidates = exact_path
        if not candidates:
            derived = []
            for identifier, transformation in (transformations or {}).items():
                bound = transformation['origins'].get(name)
                if bound is None:
                    continue
                bundle.require(all(bound['output'][field] == entry[field] for field in ('path', 'size', 'sha256')),
                               'Actual file-transform output differs from the frozen final bytes')
                original = bound['input']; original_key = original['size'], original['sha256']
                origins = output_index.get(original_key, [])
                if name in applications:
                    origins = [item for item in origins if any(source['id'] == 'huhucapture'
                               for source in receipts[item['receipt']]['sources'])]
                if not origins and not native and name not in applications:
                    origins = source_index.get(original_key, [])
                if len(origins) > 1:
                    matching = [item for item in origins if original['path'] == item.get('output', item.get('path')) or
                                original['path'].endswith('/' + item.get('output', item.get('path')))]
                    if len(matching) == 1:
                        origins = matching
                derived.extend({'transformation': identifier, 'output': name, 'input': origin} for origin in origins)
            candidates = derived
        if len(candidates) == 1:
            mapping[name] = candidates[0]
        else:
            unresolved.append({'path': name, 'reason': 'ambiguous verified origins' if candidates else
                               'no matching source/build output', 'candidateCount': len(candidates),
                               'candidates': candidates[:8]})
    bundle.require(bundle.inventory(directory) == files, 'Frozen deployment changed while proposing its source map')
    return mapping, unresolved


def write_new(path, value):
    with path.open('xb') as stream:
        stream.write(bundle.canonical(value))


def assemble(inputs, receipt_plans, stage, output, source_map=None):
    stage, output = stage.resolve(strict=True), output.resolve()
    prepared = package.verify_prepared(stage)
    bundle.require(not output.exists() and not output.is_relative_to(stage) and not stage.is_relative_to(output),
                   'Use a new sidecar output directory separate from the frozen preparation')
    output.mkdir(parents=True)
    (output / 'inputs').mkdir()
    plans, records = [], []
    for index, path in enumerate(inputs):
        path = path.resolve(strict=True)
        raw = path.read_bytes()
        snapshot = output / 'inputs' / ('plan-' + str(index) + '.json')
        snapshot.write_bytes(raw)
        bundle.require(path.read_bytes() == raw, 'Source plan changed during retention')
        plans.append(relocate_plan(bundle.read_json(snapshot), path.parent))
        records.append({'id': 'assembly-input-plan-' + str(index), 'path': str(snapshot)})
    plan = merge_plans(plans)
    for index, path in enumerate(receipt_plans):
        path = path.resolve(strict=True)
        raw = path.read_bytes()
        snapshot = output / 'inputs' / ('receipt-plan-' + str(index) + '.json')
        snapshot.write_bytes(raw)
        bundle.require(path.read_bytes() == raw, 'Producer receipt plan changed during retention')
        item = relocate_receipt(bundle.read_json(snapshot), path.parent)
        plan = merge_plans([plan, {'schemaVersion': 1, 'platform': plan['platform'], 'project': plan['project'],
                                  'buildReceipts': [item]}])
        records.append({'id': 'assembly-receipt-plan-' + str(index), 'path': str(snapshot)})
    build = prepared['build']
    bundle.require(plan['platform'] == build['platform'], 'Source inputs and actual packages use different platforms')
    if not plan.get('buildIdentity'):
        product = prepared['products'][0]
        executable = stage / 'payloads' / product['root'] / product['executable']
        plan['buildIdentity'] = str(executable.with_name('build-identity.json'))
    plan['materials'].extend(records)
    # Preserve the producer's actual deployment/objcopy boundaries even when a
    # matching original build receipt is still absent. These are evidence for
    # completing lineage, never a reason to map changed bytes to an old hash.
    transformations = bundle.read_json(stage / 'transformations.json')
    for identifier, path in [('packaging-transformations', stage / 'transformations.json'),
                             ('packaging-execution', stage / 'executions/build-execution.json'),
                             ('packaging-installed-input', stage / 'installed-input-inventory.json')]:
        plan['materials'].append({'id': identifier, 'path': str(path)})
    for index, item in enumerate(transformations.get('deploymentStages', [])):
        plan['materials'].append({'id': 'packaging-stage-' + str(index), 'path': str(stage / item['path'])})
    execution_archive=output/'packaging-execution.tar.gz'
    retain_execution(stage/'executions',transformations['executionInventory'],execution_archive)
    plan['materials'].append({'id':'packaging-execution-files','path':str(execution_archive)})
    plan['sourceTransformations']=[{'id':'packaging','stage':str(stage),'executionMaterial':'packaging-execution-files'}]
    with tempfile.TemporaryDirectory(prefix='verified-source-plan-', dir=output) as temporary:
        collector = bundle.Collector(plan, output, Path(temporary))
        collected = collector.finish()
        for key in ('sourceCommit', 'version', 'product', 'repository'):
            bundle.require(collected[key] == build[key], 'Source collection differs from the actual preparation: ' + key)
        files = prepared['deployment']['files']
        supplied = bundle.read_json(source_map.resolve(strict=True)) if source_map else None
        mapping, unresolved = proposed_mapping(stage / 'payloads', files, collector.components, collector.receipts,
                                               prepared['products'], supplied, collector.source_transformations,
                                               collector.microsoft_runtime)
        write_new(output / 'deployment-source-map.json', mapping)
        plan['deployment'] = {'directory': str(stage / 'payloads'),
                              'inventory': str(stage / 'source-deployment-inventory.json'),
                              'sourceMap': str(output / 'deployment-source-map.json'),
                              'products': prepared['products']}
        collector.gaps = [gap for gap in collector.gaps if gap !=
                          'deployment: final real installed tree, exact inventory and per-file source map are missing']
        deployment = collector.collect_deployment()
        bundle.require(deployment == prepared['deployment'], 'Collected source deployment differs from the frozen payload')
        bundle.require(package.verify_prepared(stage) == prepared, 'Package preparation changed during source-plan assembly')
        report = {'schemaVersion': 1, 'kind': 'huhucapture-final-source-plan-assembly',
                  'sourceCommit': build['sourceCommit'], 'platform': build['platform'],
                  'deploymentTreeSha256': deployment['treeSha256'], 'mappedFiles': len(mapping),
                  'totalFiles': len(files), 'unresolvedFiles': unresolved, 'gaps': sorted(set(collector.gaps)),
                  'sourceComponents': {name: item['sourceTreeSha256'] for name, item in collector.components.items()},
                  'buildReceiptIds': sorted(collector.receipts),
                  'deploymentStages': transformations.get('deploymentStages', []),
                  'transformationGaps': {name: value['unresolved'] for name, value in collector.source_transformations.items()},
                  'scope': 'Verified input assembly only; create-source-bundle --require-complete remains mandatory'}
        write_new(output / 'source-plan-report.json', report)
        plan['materials'].append({'id': 'final-source-plan-assembly', 'path': str(output / 'source-plan-report.json')})
        write_new(output / 'source-plan.json', plan)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, action='append', required=True,
                        help='Actual source/material plan; repeat for the same checkout and platform')
    parser.add_argument('--receipt-plan', type=Path, action='append', default=[],
                        help='Additional original producer receipt-plan.json, never a reconstructed receipt')
    parser.add_argument('--stage', type=Path, required=True, help='Actual package-qt prepare output')
    parser.add_argument('--output', type=Path, required=True, help='New sidecar directory for the plan, map and gaps')
    parser.add_argument('--source-map', type=Path, help='Explicit verified-origin choices for ambiguous files')
    args = parser.parse_args()
    try:
        report = assemble(args.inputs, args.receipt_plan, args.stage, args.output, args.source_map)
        print(json.dumps({key: report[key] for key in ('mappedFiles', 'totalFiles', 'gaps')}, indent=2))
        return 0
    except (ValueError, KeyError, OSError, bundle.subprocess.SubprocessError,
            bundle.tarfile.TarError, bundle.tomllib.TOMLDecodeError) as error:
        print('Source-plan assembly failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
