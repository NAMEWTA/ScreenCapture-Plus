#!/usr/bin/env python3
"""Actual producer command/source/output contracts; no product build claim."""
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
receipts = runpy.run_path(str(ROOT / 'build/package/source_receipts.py'))
bundle = receipts['bundle']
source_plan = runpy.run_path(str(ROOT / 'scripts/assemble-source-plan.py'))


class SourceReceiptsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='actual-build-receipt-')
        self.root = Path(self.temporary.name).resolve(strict=True)
        self.source = self.root / 'source'
        self.source.mkdir()
        for args in (('init', '--quiet'), ('config', 'user.name', 'Source receipt contract'),
                     ('config', 'user.email', 'source-receipt@example.invalid'), ('config', 'core.autocrlf', 'false')):
            bundle.git(self.source, *args)
        (self.source / 'source.c').write_bytes(b'#include <stdio.h>\nint main(void) { puts("actual receipt fixture"); return 0; }\n')
        (self.source / 'LICENSE').write_bytes(b'Contract fixture notice\n')
        bundle.git(self.source, 'add', '.')
        bundle.git(self.source, 'commit', '--quiet', '-m', 'Actual producer source fixture')
        self.pin = {'repository': 'https://example.invalid/fixture.git',
                    'commit': bundle.git(self.source, 'rev-parse', 'HEAD').decode().strip()}
        self.prefix = self.root / 'prefix'
        self.prefix.mkdir()
        (self.prefix / 'previous-producer.txt').write_bytes(b'Existing prefix data is not this build output\n')

    def tearDown(self):
        self.temporary.cleanup()

    def producer(self, name='receipt'):
        return receipts['NativeBuild'](self.root / name, 'linux' if os.name != 'nt' else 'windows',
                                       __file__, 'fixture', self.source, self.pin, self.prefix)

    @unittest.skipUnless(os.name == 'nt', 'Requires the actual Windows Visual Studio v143 release runtime')
    def test_actual_microsoft_runtime_remains_bound_to_its_app_local_bytes(self):
        fixture = self.root / 'msvc'
        fixture.mkdir()
        (fixture / 'CMakeLists.txt').write_text(r'''cmake_minimum_required(VERSION 3.25)
project(original_msvc_runtime LANGUAGES CXX)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
string(JOIN "\n" runtime_inputs ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
file(WRITE "${CMAKE_BINARY_DIR}/msvc-runtime-inputs.txt" "${CMAKE_GENERATOR_INSTANCE}\n${runtime_inputs}\n")
install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION . CONFIGURATIONS Release)
''', encoding='utf-8')
        build = fixture / 'build'
        subprocess.run(['cmake', '-S', str(fixture), '-B', str(build), '-G', 'Visual Studio 17 2022',
                        '-A', 'x64', '-T', 'v143'], check=True, capture_output=True)
        subprocess.run(['cmake', '--install', str(build), '--config', 'Release', '--prefix', str(self.prefix)],
                       check=True, capture_output=True)
        meta = bundle.packaging_metadata()
        helper = runpy.run_path(str(ROOT / 'build/package/windows_redistributables.py'))
        selection = build / 'msvc-runtime-inputs.txt'
        report = helper['report'](selection, meta, self.prefix)
        self.assertEqual(report['kind'], 'microsoft-vc143-release-runtime')
        stage = self.root / 'collector'; stage.mkdir()
        collector = bundle.Collector({}, self.root, stage)
        report_path = self.root / '1-msvc-runtime-inputs.json'
        receipts['write_new'](report_path, report)
        selection_record = self.root / '0-msvc-runtime-inputs.txt'
        shutil.copyfile(selection, selection_record)
        retained = []
        for identifier, record in (('selection', selection_record), ('report', report_path)):
            collector.retain(identifier, str(record))
            retained.append(collector.materials[identifier])
        collector.collect_microsoft_runtime({key: collector.materials[key]['sha256'] for key in ('selection', 'report')}, retained)

        payload = self.root / 'payload'
        filename = 'msvcp140.dll'
        for folder in ('installer', 'portable-zip', 'installer/plugins'):
            destination = payload / folder / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.prefix / filename, destination)
        with (payload / 'portable-zip' / filename).open('ab') as stream:
            stream.write(b'changed after installation')
        mapping, unresolved = source_plan['proposed_mapping'](
            payload, bundle.inventory(payload), {}, {}, [], microsoft_runtime=collector.microsoft_runtime)
        self.assertEqual(mapping, {'installer/' + filename: {'microsoftRuntime': filename}})
        self.assertEqual({item['path'] for item in unresolved},
                         {'installer/plugins/' + filename, 'portable-zip/' + filename})
        with (self.prefix / filename).open('ab') as stream:
            stream.write(b'changed installed bytes')
        with self.assertRaisesRegex(ValueError, 'differs from the original Microsoft file'):
            helper['report'](selection, meta, self.prefix)

    @unittest.skipIf(os.name == 'nt', 'Actual cc/ELF compilation is a Linux producer gate')
    def test_real_compile_binds_checked_source_actual_commands_and_only_new_outputs(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.fail('The actual producer output test requires the host C compiler')
        producer = self.producer()
        executable = self.prefix / 'probe'
        command = [compiler, str(self.source / 'source.c'), '-o', str(executable)]
        producer.run(command, cwd=self.root)
        stdout, stderr = producer.run([str(executable)], cwd=self.root, capture=True)
        self.assertEqual(stdout.strip(), 'actual receipt fixture')
        self.assertEqual(stderr, '')
        plan = producer.finish()
        receipt = bundle.read_json(Path(plan['path']))
        self.assertEqual(receipt['commands'][0], command)
        self.assertEqual([item['path'] for item in receipt['outputs']], ['probe'])
        self.assertEqual(receipt['outputs'][0]['sha256'], bundle.digest(executable))
        execution = bundle.read_json(producer.directory / 'build-execution.json')
        self.assertEqual([item['exitCode'] for item in execution['executions']], [0, 0])
        self.assertEqual(execution['inputs']['sources'][0]['sourceTreeSha256'], producer.source_hash)
        execution_files = bundle.inventory(producer.directory)
        self.assertTrue(any(item.get('size') == 0 for item in execution_files))
        archive = self.root / 'original-execution.tar.gz'
        source_plan['retain_execution'](producer.directory, execution_files, archive)
        extracted = bundle.extract_tar(archive, self.root / 'original-execution', 'huhucapture-sources')
        self.assertEqual(extracted, execution_files)
        # Pass the actual produced receipt through the independent consumer.
        stage = self.root / 'collector'; stage.mkdir()
        collector = bundle.Collector({'buildReceipts': [plan]}, self.root, stage)
        collector.platform = 'linux'; collector.lock = {'qtMigration': {}}
        collector.components = {'fixture': {'sourceTreeSha256': producer.source_hash}}
        collector.collect_receipts()
        self.assertEqual(collector.gaps, [])
        self.assertEqual(collector.receipts['fixture']['outputs'], receipt['outputs'])
        # A final-stage assembler can bind these actual output bytes to the
        # verified producer, but cannot turn a copied binary into raw source.
        payload = self.root / 'payload'; payload.mkdir()
        shutil.copy2(executable, payload / 'probe')
        shutil.copy2(self.source / 'LICENSE', payload / 'LICENSE')
        files = bundle.inventory(payload)
        components = {'fixture': {'files': producer.files}}
        mapping, unresolved = source_plan['proposed_mapping'](
            payload, files, components, collector.receipts, [])
        self.assertEqual(unresolved, [])
        self.assertEqual(mapping['probe'], {'receipt': 'fixture', 'output': 'probe'})
        self.assertEqual(mapping['LICENSE'], {'component': 'fixture', 'path': 'LICENSE'})
        prebuilt = {'fixture': {'files': files}}
        mapping, unresolved = source_plan['proposed_mapping'](payload, files, prebuilt, {}, [])
        self.assertNotIn('probe', mapping)
        self.assertEqual([item['path'] for item in unresolved], ['probe'])
        duplicate = {**collector.receipts, 'different-origin': collector.receipts['fixture']}
        mapping, unresolved = source_plan['proposed_mapping'](payload, files, components, duplicate, [])
        self.assertNotIn('probe', mapping)
        self.assertEqual(unresolved[0]['candidateCount'], 2)
        with (payload / 'probe').open('ab') as stream:
            stream.write(b'changed frozen payload')
        with self.assertRaisesRegex(ValueError, 'deployment changed'):
            source_plan['proposed_mapping'](payload, files, components, collector.receipts, [])
        report = {'platform': 'linux', 'buildComplete': True, 'sourceModules': [
            {'id': 'fixture', 'sourceDirectory': str(self.source), 'sourceBuildReceipt': plan}]}
        report_path = self.root / 'build-info.json'
        receipts['write_new'](report_path, report)
        fragment_path = receipts['write_module_plan'](self.root, report, 'linux')
        fragment = bundle.read_json(fragment_path)
        self.assertEqual(fragment['buildReceipts'], [plan])
        self.assertEqual(fragment['sources'], [{'id': 'fixture', 'directory': str(self.source)}])
        with self.assertRaisesRegex(ValueError, 'unchanged successful'):
            receipts['write_module_plan'](self.root, {**report, 'buildComplete': False}, 'linux')
        selector = runpy.run_path(str(ROOT / 'scripts/collect-native-build-plans.py'))
        selected_output = self.root / 'selected-native-plan'; selected_output.mkdir()
        selected = selector['bind_producer']('fixture', plan, self.root, self.prefix,
            {'files': producer.files}, selected_output, {}, Path(__file__).resolve())
        self.assertEqual(bundle.read_json(selected['path']), receipt)
        self.assertTrue(all(Path(item['path']).is_relative_to(selected_output) for item in selected['buildRecords']))
        # Mutating an actual linked output rejects selection, even though the
        # original successful receipt and journal are still intact.
        original_binary = executable.read_bytes()
        with executable.open('ab') as stream:
            stream.write(b'changed before final source-plan selection')
        rejected_output = self.root / 'rejected-native-plan'; rejected_output.mkdir()
        with self.assertRaisesRegex(ValueError, 'actual producer-time output identity'):
            selector['bind_producer']('fixture', plan, self.root, self.prefix,
                {'files': producer.files}, rejected_output, {}, Path(__file__).resolve())
        executable.write_bytes(original_binary)
        native = runpy.run_path(str(ROOT / 'scripts/collect-native-source-materials.py'))
        native_output = self.root / 'native-materials'; native_output.mkdir()
        # Same real source identities expressed in the native archive collector's
        # format, which has already checked each archive blob at this boundary.
        original = [{'path': item['path'], 'mode': '100755' if item['mode'] & 0o111 else '100644',
                     'size': item['size'], 'sha256': item['sha256']} for item in producer.files]
        (native_output / 'source-inputs.json').write_bytes(bundle.canonical({'archiveFiles': original}))
        source_record = {'sourceInventory': {'path': 'source-inputs.json'}}
        runtime = [{'path': 'probe', 'originalPath': str(executable), **receipts['identity'](executable)}]
        bound = native['producer_receipt']('fixture', plan, self.root, self.prefix, source_record, runtime, native_output, {})
        self.assertEqual(bound['runtimeOutputsVerified'], 1)
        self.assertEqual(bound['sourceTreeSha256'], producer.source_hash)
        changed_output = self.root / 'changed-native-materials'; changed_output.mkdir()
        shutil.copyfile(native_output / 'source-inputs.json', changed_output / 'source-inputs.json')
        with executable.open('ab') as stream:
            stream.write(b'changed after producer')
        runtime[0].update(receipts['identity'](executable))
        with self.assertRaisesRegex(ValueError, 'actual producer-time output identity'):
            native['producer_receipt']('fixture', plan, self.root, self.prefix, source_record, runtime, changed_output, {})

    def test_actual_failed_command_keeps_exit_and_logs_without_successful_receipt(self):
        producer = self.producer()
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            producer.run([sys.executable, '-c', 'import sys; print("actual failure fixture"); sys.exit(23)'], cwd=self.root)
        self.assertEqual(failure.exception.returncode, 23)
        record = bundle.read_json(producer.directory / 'execution-0.json')
        self.assertEqual(record['exitCode'], 23)
        self.assertIn('actual failure fixture', Path(record['stdout']).read_text())
        with self.assertRaisesRegex(ValueError, 'Unsuccessful commands'):
            producer.raw_record({}, {})
        self.assertFalse((producer.directory / 'source-build-receipt.json').exists())

    def test_actual_child_loader_search_path_is_retained_without_normalization(self):
        producer = self.producer()
        environment = os.environ.copy()
        environment['PATH'] = os.pathsep.join([str(self.root / 'module-build/bin'),
                                              str(self.prefix / 'bin'), environment.get('PATH', '')])
        expected = environment['PATH']
        stdout, stderr = producer.run([sys.executable, '-c', 'import os,sys; sys.stdout.write(os.environ["PATH"])'],
                                      cwd=self.root, env=environment, capture=True)
        self.assertEqual(stdout, expected)
        self.assertEqual(stderr, '')
        record = bundle.read_json(producer.directory / 'execution-0.json')
        self.assertEqual(record['environment']['PATH'], stdout)
        self.assertEqual(record['exitCode'], 0)
        self.assertEqual(environment['PATH'], expected)

    @unittest.skipIf(os.name == 'nt', 'The actual cc/objcopy source ancestry chain runs on Linux')
    def test_real_compilation_copy_objcopy_chain_retains_original_receipt_and_rejects_unrecorded_bytes(self):
        for tool in ('cc', 'objcopy'):
            self.assertIsNotNone(shutil.which(tool), 'The real transformation chain requires ' + tool)
        producer = self.producer()
        original = self.prefix / 'probe'
        producer.run([shutil.which('cc'), '-g', str(self.source / 'source.c'), '-o', str(original)], cwd=self.root)
        plan = producer.finish()
        collection = self.root / 'collected'; collection.mkdir()
        collector = bundle.Collector({'buildReceipts': [plan]}, self.root, collection)
        collector.platform = 'linux'; collector.lock = {'qtMigration': {}}
        collector.components = {'fixture': {'sourceTreeSha256': producer.source_hash}}
        collector.collect_receipts()
        original_receipts = json.loads(json.dumps(collector.receipts))
        stage = self.root / 'transform-stage'; (stage / 'payloads').mkdir(parents=True)
        package = source_plan['package']; transforms = package.lineage_api()
        execution = receipts['ExecutionLog'](stage / 'executions', 'linux', package.__file__)
        recorder = transforms.Recorder(stage, self.prefix, execution, bundle)
        payload = stage / 'payloads/appimage/usr'
        recorder.copy_tree(self.prefix, payload)
        executable = payload / 'probe'
        def run(arguments):
            return execution.run(arguments, cwd=stage)
        debug = stage / 'work/debug/probe.debug'; debug.parent.mkdir(parents=True)
        debug_index = run([shutil.which('objcopy'), '--only-keep-debug', executable, debug])
        recorder.mutation(executable, [shutil.which('objcopy'), '--strip-debug', executable], run,
                          operation='objcopy-strip-debug')
        recorder.mutation(executable, [shutil.which('objcopy'), '--add-gnu-debuglink=' + str(debug), executable], run,
                          operation='objcopy-add-debuglink', debugCommandIndex=debug_index,
                          debugInput={'path': str(debug), **transforms.file_identity(bundle, debug)})
        stdout, _ = execution.run([executable], cwd=stage, capture=True)
        self.assertEqual(stdout.strip(), 'actual receipt fixture')
        self.assertNotEqual(bundle.digest(original), bundle.digest(executable))
        # A newly copied ELF has no original installed input chain. A real
        # subsequent objcopy call cannot invent that missing source boundary.
        new_library = payload / 'new-library'
        shutil.copyfile(executable, new_library)
        recorder.mutation(new_library, [shutil.which('objcopy'), '--strip-debug', new_library], run,
                          operation='objcopy-strip-debug')
        (payload / 'probe-alias').symlink_to('probe')
        files = bundle.inventory(stage / 'payloads')
        descriptor = recorder.finish({}, files)
        raw = execution.raw_record({'installedInput': recorder.original}, {'files': files, 'fileTransformations': descriptor})
        journal = {'buildIdentity': {}, 'fileTransformations': descriptor}
        recorded = bundle.read_json(raw)
        bound = transforms.validate(stage, journal, recorded, bundle)
        target = 'appimage/usr/probe'
        self.assertEqual(bound['origins'][target]['input']['sha256'], bundle.digest(original))
        self.assertNotIn('appimage/usr/new-library', bound['origins'])
        mapping, unresolved = source_plan['proposed_mapping'](stage / 'payloads', files, {}, collector.receipts, [],
                                                            transformations={'packaging': bound})
        self.assertEqual(mapping[target], {'transformation': 'packaging', 'output': target,
                                          'input': {'receipt': 'fixture', 'output': 'probe'}})
        self.assertIn('appimage/usr/new-library', {item['path'] for item in unresolved})
        collector.source_transformations = {'packaging': bound}
        final_entry = next(item for item in files if item['path'] == target)
        ancestry, root_input = collector.original_mapping(mapping[target], final_entry)
        self.assertEqual(ancestry, {'receipt': 'fixture', 'output': 'probe'})
        self.assertEqual(root_input['sha256'], bundle.digest(original))
        self.assertEqual(collector.receipts, original_receipts, 'Transformations must preserve all original source/static receipts')
        # The journal keeps its real original absolute paths. Revalidation must
        # read the retained files at the new location, including debug inputs,
        # when the old stage and original installed prefix no longer exist.
        relocated = self.root / 'relocated-stage'
        stage.rename(relocated)
        self.prefix.rename(self.root / 'original-prefix-moved')
        self.assertFalse(stage.exists())
        self.assertFalse(self.prefix.exists())
        stage = relocated
        executable = stage / 'payloads' / target
        self.assertEqual(transforms.validate(stage, journal, recorded, bundle), bound)
        self.assertEqual((executable.parent / 'probe-alias').resolve(strict=True), executable)
        failed = json.loads(json.dumps(recorded)); failed['executions'][0]['exitCode'] = 23
        with self.assertRaisesRegex(ValueError, 'successful execution'):
            transforms.validate(stage, journal, failed, bundle)
        proof = stage / 'file-transformations.json'; saved = proof.read_bytes()
        proof.write_bytes(saved + b'\n')
        with self.assertRaisesRegex(ValueError, 'missing or changed'):
            transforms.validate(stage, journal, recorded, bundle)
        proof.write_bytes(saved)
        missing_step = json.loads(saved)
        missing_step['events'] = [event for event in missing_step['events'] if not (
            event['operation'] == 'objcopy-strip-debug' and event['after']['path'] == target)]
        proof.write_bytes(bundle.canonical(missing_step))
        changed_pointer = {'path': 'file-transformations.json', **bundle.descriptor(proof)}
        missing_record = json.loads(json.dumps(recorded))
        missing_record['outputs']['fileTransformations'] = changed_pointer
        incomplete = transforms.validate(stage, {**journal, 'fileTransformations': changed_pointer}, missing_record, bundle)
        self.assertNotIn(target, incomplete['origins'], 'A copy and a final hash cannot replace the missing actual transformation')
        proof.write_bytes(saved)
        with executable.open('ab') as stream:
            stream.write(b'change after actual transformation')
        with self.assertRaisesRegex(ValueError, 'final output bytes'):
            transforms.validate(stage, journal, recorded, bundle)

    def test_source_plan_relocates_material_paths_without_rewriting_source_license_names(self):
        plan = {'schemaVersion': 1, 'platform': 'linux', 'project': 'source',
                'sources': [{'id': 'fixture', 'directory': 'actual checkout',
                             'pinEvidence': {'path': 'original.json', 'sha256': 'a' * 64, 'pointer': '/commit'}},
                            {'id': 'system-fixture', 'kind': 'debian-source', 'dsc': 'source/fixture.dsc',
                             'sourceIndex': 'source/Sources.txt'}],
                'rustVendor': {'directory': 'vendor', 'registryCache': 'registry', 'config': 'vendor.toml',
                               'noticeReviews': 'third_party/rust-notices/upstream.json',
                               'licenseTexts': {'example@1.0': {'component': 'fixture', 'path': 'LICENSE'}},
                               'replay': {'cargo': 'tools/cargo', 'rustc': 'tools/rustc', 'cache': 'cache'}},
                'buildReceipts': [{'id': 'fixture', 'path': 'receipt.json',
                                  'buildRecords': [{'path': 'build.log', 'sha256': 'b' * 64}],
                                  'staticFiles': {'aggregate': 'lib/aggregate.a'}}]}
        relocated = source_plan['relocate_plan'](plan, self.root)
        self.assertEqual(relocated['sources'][0]['directory'], str(self.root / 'actual checkout'))
        self.assertEqual(relocated['sources'][1]['dsc'], str(self.root / 'source/fixture.dsc'))
        self.assertEqual(relocated['sources'][1]['sourceIndex'], str(self.root / 'source/Sources.txt'))
        self.assertEqual(relocated['rustVendor']['replay']['cache'], str(self.root / 'cache'))
        self.assertEqual(relocated['rustVendor']['licenseTexts'], plan['rustVendor']['licenseTexts'])
        self.assertEqual(relocated['rustVendor']['noticeReviews'], plan['rustVendor']['noticeReviews'])
        self.assertEqual(relocated['buildReceipts'][0]['staticFiles']['aggregate'], str(self.root / 'lib/aggregate.a'))
        self.assertEqual(plan['project'], 'source')
        self.assertEqual(len(source_plan['merge_plans']([relocated, relocated])['buildReceipts']), 1)
        with self.assertRaisesRegex(ValueError, 'same project checkout and platform'):
            source_plan['merge_plans']([relocated, {**relocated, 'platform': 'windows'}])
        with self.assertRaisesRegex(ValueError, 'approval and complete'):
            source_plan['relocate_plan']({**plan, 'complete': True}, self.root)

    def recorded_linuxdeploy(self, *, break_chain=False):
        pin = bundle.read_json(ROOT / 'build/package/dependencies.lock.json')['tools']['linuxdeploy']
        configured = os.environ.get('HUHUCAPTURE_TEST_LINUXDEPLOY')
        linuxdeploy = Path(configured) if configured else ROOT / 'artifacts/packaging-tools/linuxdeploy' / pin['name']
        if not linuxdeploy.is_file() and not configured:
            self.skipTest('The exact locked linuxdeploy image is required; set HUHUCAPTURE_TEST_LINUXDEPLOY')
        self.assertEqual(receipts['identity'](linuxdeploy), {key: pin[key] for key in ('size', 'sha256')})
        for tool in ('cc', 'objcopy', 'readelf'):
            self.assertIsNotNone(shutil.which(tool), 'The actual ELF chain requires ' + tool)
        linuxdeploy = linuxdeploy.resolve(strict=True)
        producer = self.producer()
        original = self.prefix / 'probe'
        compile_command = [shutil.which('cc'), '-g', '-Wl,-rpath,/original-receipt-rpath',
                           str(self.source / 'source.c'), '-o', str(original)]
        producer.run(compile_command, cwd=self.root)
        plan = producer.finish()
        collected = self.root / 'observed-collected'; collected.mkdir()
        collector = bundle.Collector({'buildReceipts': [plan]}, self.root, collected)
        collector.platform = 'linux'; collector.lock = {'qtMigration': {}}
        collector.components = {'fixture': {'sourceTreeSha256': producer.source_hash}}
        collector.collect_receipts()
        self.assertEqual(collector.gaps, [])
        stage = self.root / 'observed-stage'; (stage / 'payloads').mkdir(parents=True)
        (stage / 'work').mkdir()
        installed = self.root / 'empty-installed'; installed.mkdir()
        package = source_plan['package']; transforms = package.lineage_api()
        execution = receipts['ExecutionLog'](stage / 'executions', 'linux', package.__file__)
        recorder = transforms.Recorder(stage, installed, execution, bundle)
        appdir = stage / 'payloads/appimage'
        executable = appdir / 'usr/bin/probe'
        # An owned real linuxdeploy plugin adds read-only observations. The
        # negative case really replaces the file between recorded operations;
        # it never manufactures a copy event or a successful receipt.
        plugins = self.root / 'plugins'; plugins.mkdir()
        plugin = plugins / 'linuxdeploy-plugin-observe'
        text = '#!/bin/sh\nset -eu\nif [ "${1-}" = "--plugin-api-version" ]; then printf "0\\n"; exit 0; fi\n'
        if break_chain:
            text += shlex.join([shutil.which('cp'), str(original), str(executable)]) + '\n'
        text += '"$PATCHELF" --print-rpath ' + shlex.quote(str(executable)) + '\n'
        if break_chain:
            text += '"$PATCHELF" --set-rpath /later-observed-rpath ' + shlex.quote(str(executable)) + '\n'
            text += '"$PATCHELF" --print-rpath ' + shlex.quote(str(executable)) + '\n'
        plugin.write_text(text, encoding='utf-8', newline='\n'); plugin.chmod(0o755)
        execution.retain_record(plugin)
        environment = {**os.environ, 'APPIMAGE_EXTRACT_AND_RUN': '1', 'NO_STRIP': '1',
                       'PATH': str(plugins) + os.pathsep + os.environ.get('PATH', '')}
        first = len(execution.commands)
        environment = recorder.patchelf_environment(linuxdeploy, environment, execution.run)
        execution.run([linuxdeploy, '--appdir', appdir, '--executable', original, '--plugin', 'observe'],
                      cwd=stage, env=environment)
        native_commands = list(range(first, len(execution.commands)))
        recorder.collect_patchelf()
        recorder.mutation(executable, [shutil.which('objcopy'), '--strip-debug', executable],
                          lambda arguments: execution.run(arguments, cwd=stage), operation='objcopy-strip-debug')
        stdout, _ = execution.run([executable], cwd=stage, capture=True)
        self.assertEqual(stdout.strip(), 'actual receipt fixture')
        dynamic = subprocess.check_output([shutil.which('readelf'), '-d', str(executable)], text=True)
        self.assertNotIn('/original-receipt-rpath', dynamic)
        self.assertNotEqual(bundle.digest(original), bundle.digest(executable))
        files = bundle.inventory(stage / 'payloads')
        descriptor = recorder.finish({}, files)
        raw = execution.raw_record({'installedInput': recorder.original},
                                   {'files': files, 'fileTransformations': descriptor})
        journal = {'buildIdentity': {}, 'fileTransformations': descriptor,
                   'deploymentStages': [{'operation': 'native-deployment', 'commands': native_commands}]}
        recorded = bundle.read_json(raw)
        observed = transforms.validate(stage, journal, recorded, bundle)
        return {'stage': stage, 'files': files, 'journal': journal, 'recorded': recorded, 'observed': observed,
                'transforms': transforms, 'collector': collector, 'producer': producer, 'plan': plan,
                'original': original, 'target': 'appimage/usr/bin/probe', 'compileCommand': compile_command}

    @unittest.skipIf(os.name == 'nt', 'Actual linuxdeploy/patchelf/ELF capture runs on Linux')
    def test_first_observed_linuxdeploy_input_requires_one_original_producer_and_every_real_step(self):
        result = self.recorded_linuxdeploy()
        stage, files, observed = result['stage'], result['files'], result['observed']
        transforms, collector, target = result['transforms'], result['collector'], result['target']
        original_receipts = json.loads(json.dumps(collector.receipts))
        self.assertEqual(observed['origins'], {}, 'The deployed file was never an installed input')
        first = observed['observedOrigins'][target]['input']
        self.assertEqual(first['kind'], 'observed-input')
        self.assertEqual(first['sha256'], bundle.digest(result['original']))
        self.assertEqual(first['sequence'], 0)
        bound = transforms.bind_observed_inputs(observed, collector.receipts)
        expected = {'transformation': 'packaging', 'output': target,
                    'input': {'receipt': 'fixture', 'output': 'probe'}}
        mapping, unresolved = source_plan['proposed_mapping'](stage / 'payloads', files, {}, collector.receipts, [],
                                                            transformations={'packaging': bound})
        self.assertEqual(mapping[target], expected)
        self.assertNotIn(target, {entry['path'] for entry in unresolved})
        collector.source_transformations = {'packaging': bound}
        final = next(entry for entry in files if entry['path'] == target)
        ancestry, root_input = collector.original_mapping(expected, final)
        self.assertEqual(ancestry, expected['input'])
        self.assertEqual(root_input['sha256'], bundle.digest(result['original']))
        self.assertEqual(collector.original_mapping(expected['input'], final), (None, None),
                         'An explicit direct mapping must not bypass the observed boundary')
        self.assertEqual(collector.receipts, original_receipts)
        # Binding cannot invent a producer, or preserve a stale earlier binding.
        missing = transforms.bind_observed_inputs(bound, {})
        self.assertNotIn(target, missing['origins'])
        mapping, unresolved = source_plan['proposed_mapping'](stage / 'payloads', files, {}, {}, [],
                                                            transformations={'packaging': missing})
        self.assertNotIn(target, mapping)
        self.assertIn(target, {entry['path'] for entry in unresolved})
        # A second real compile produces identical bytes under a different
        # output name. Basename agreement cannot disambiguate original builds.
        second_prefix = self.root / 'second-prefix'; second_prefix.mkdir()
        other = receipts['NativeBuild'](self.root / 'second-receipt', 'linux', __file__,
                                       'second-fixture', self.source, self.pin, second_prefix)
        command = result['compileCommand'][:-1] + [str(second_prefix / 'different-output-name')]
        other.run(command, cwd=self.root)
        other_plan = other.finish()
        self.assertEqual(bundle.digest(second_prefix / 'different-output-name'), bundle.digest(result['original']))
        second_stage = self.root / 'two-producers'; second_stage.mkdir()
        two = bundle.Collector({'buildReceipts': [result['plan'], other_plan]}, self.root, second_stage)
        two.platform = 'linux'; two.lock = {'qtMigration': {}}
        two.components = {'fixture': {'sourceTreeSha256': result['producer'].source_hash},
                          'second-fixture': {'sourceTreeSha256': other.source_hash}}
        two.collect_receipts(); self.assertEqual(two.gaps, [])
        ambiguous = transforms.bind_observed_inputs(observed, two.receipts)
        self.assertNotIn(target, ambiguous['origins'])
        mapping, _ = source_plan['proposed_mapping'](stage / 'payloads', files, {}, two.receipts, [],
                                                    transformations={'packaging': ambiguous})
        self.assertNotIn(target, mapping)
        two.source_transformations = {'packaging': ambiguous}
        self.assertEqual(two.original_mapping(expected, final), (None, None))
        # These deliberately changed consumers are negative test inputs. The
        # retained actual nested commands and logs are never rewritten.
        proof_path = stage / 'file-transformations.json'; saved = proof_path.read_bytes()
        proof = json.loads(saved)
        operations = [event['operation'] for event in proof['events']]
        self.assertEqual(operations[:2], ['patchelf-set-rpath', 'patchelf-print-rpath'])
        def validate_changed(changed):
            proof_path.write_bytes(bundle.canonical(changed))
            pointer = {'path': 'file-transformations.json', **bundle.descriptor(proof_path)}
            record = json.loads(json.dumps(result['recorded']))
            record['outputs']['fileTransformations'] = pointer
            return transforms.validate(stage, {**result['journal'], 'fileTransformations': pointer}, record, bundle)
        try:
            changed = json.loads(saved)
            changed['events'] = [event for event in changed['events'] if event['operation'] != 'patchelf-print-rpath']
            with self.assertRaisesRegex(ValueError, 'observation was omitted'):
                validate_changed(changed)
            changed = json.loads(saved); changed['events'][0], changed['events'][1] = changed['events'][1], changed['events'][0]
            with self.assertRaisesRegex(ValueError, 'complete ordered original'):
                validate_changed(changed)
            changed = json.loads(saved); changed['events'][0]['before']['sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'complete ordered original'):
                validate_changed(changed)
            changed = json.loads(saved); changed['events'] = changed['events'][:-1]
            incomplete = validate_changed(changed)
            self.assertNotIn(target, transforms.bind_observed_inputs(incomplete, collector.receipts)['origins'])
            changed = json.loads(saved); changed['schemaVersion'] = 1
            changed['events'] = [event for event in changed['events'] if event['operation'] != 'patchelf-print-rpath']
            legacy = validate_changed(changed)
            self.assertEqual(legacy['observedPaths'], [], 'Older evidence cannot acquire a new observed root')
            self.assertNotIn(target, legacy['origins'])
        finally:
            proof_path.write_bytes(saved)
        changed_range = json.loads(json.dumps(result['journal']))
        changed_range['deploymentStages'][0]['commands'] = [0]
        with self.assertRaisesRegex(ValueError, 'successful locked-image deployment command'):
            transforms.validate(stage, changed_range, result['recorded'], bundle)

    @unittest.skipIf(os.name == 'nt', 'Actual linuxdeploy/patchelf/ELF capture runs on Linux')
    def test_observed_input_cannot_restart_after_an_actual_unrecorded_replacement(self):
        result = self.recorded_linuxdeploy(break_chain=True)
        observed, target, transforms = result['observed'], result['target'], result['transforms']
        self.assertIn(target, observed['observedPaths'])
        self.assertNotIn(target, observed['observedOrigins'])
        records = [bundle.read_json(path) for path in sorted((result['stage'] / 'executions/patchelf').glob('*.json'))]
        self.assertEqual([row['operation'] for row in records],
                         ['patchelf-set-rpath', 'patchelf-print-rpath', 'patchelf-set-rpath', 'patchelf-print-rpath'])
        self.assertEqual(records[1]['before']['sha256'], bundle.digest(result['original']))
        self.assertEqual(records[2]['before']['sha256'], bundle.digest(result['original']))
        self.assertNotEqual(records[0]['after'], records[1]['before'])
        bound = transforms.bind_observed_inputs(observed, result['collector'].receipts)
        self.assertNotIn(target, bound['origins'], 'Matching a producer again must not repair a broken earlier chain')
        mapping, unresolved = source_plan['proposed_mapping'](result['stage'] / 'payloads', result['files'], {},
            result['collector'].receipts, [], transformations={'packaging': bound})
        self.assertNotIn(target, mapping)
        self.assertIn(target, {entry['path'] for entry in unresolved})

    def test_changed_original_source_cannot_receive_a_build_receipt(self):
        producer = self.producer()
        producer.run([sys.executable, '-c', 'from pathlib import Path; Path("made.txt").write_bytes(b"actual output")'], cwd=self.prefix)
        (self.source / 'source.c').write_bytes(b'int changed_after_build;\n')
        with self.assertRaisesRegex(ValueError, 'unrecorded source change'):
            producer.finish()
        self.assertFalse((producer.directory / 'source-build-receipt.json').exists())

    @unittest.skipIf(os.name == 'nt', 'The controlled POSIX shell contract runs on the Linux host')
    def test_controlled_script_binds_only_successful_unchanged_executed_arguments(self):
        shell = shutil.which('sh')
        if not shell:
            self.fail('The real controlled-script contract requires a POSIX shell')
        producer = self.producer()
        commands = [[sys.executable, '-c', 'from pathlib import Path; Path("output with space.txt").write_bytes(b"actual shell output")']]
        script = self.root / 'fixed script.sh'
        expected = 'set -eu\ncd ' + shlex.quote(str(self.prefix)) + '\n' + shlex.join(commands[0]) + '\n'
        script.write_text(expected, encoding='utf-8', newline='\n')
        indices = producer.run_fixed_script([shell, str(script)], script, commands,
                    build_directory=self.root, posix_directory=str(self.prefix), env=os.environ)
        plan = producer.finish()
        receipt = bundle.read_json(Path(plan['path']))
        self.assertEqual(receipt['commands'][indices[0]], commands[0])
        self.assertEqual(receipt['commandOrigins'][str(indices[0])]['line'], 3)
        self.assertEqual((self.prefix / 'output with space.txt').read_bytes(), b'actual shell output')
        other = self.producer('invalid-script')
        with script.open('a') as stream:
            stream.write('printf unrecorded\n')
        with self.assertRaisesRegex(ValueError, 'script differs'):
            other.run_fixed_script([shell, str(script)], script, commands,
                        build_directory=self.root, posix_directory=str(self.prefix), env=os.environ)
        self.assertEqual(other.executions, [])


if __name__ == '__main__':
    unittest.main()
