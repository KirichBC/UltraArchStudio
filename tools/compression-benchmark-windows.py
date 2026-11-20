"""Serial fixed-input ablations, three-repeat corpus and background acceptance.

Every successful case gets independent path/size/SHA-256 verification and Windows
tree accounting. Resume skips only cases verified against the same binary/input.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


compare = module('archiver_comparison', 'compare-archivers-windows.py')
corpus_tools = module('corpus_tools', 'benchmark.py')


def run(args):
    if os.name != 'nt':
        raise RuntimeError('Native Windows required')
    args.output.mkdir(parents=True, exist_ok=True)
    cases = []
    if args.suite in ('fixed', 'all'):
        if not args.input or not args.reference:
            raise ValueError('Fixed suite requires --input and --reference')
        for strategy, search, repeats in [('groups', 'exhaustive', 1), ('whole', 'exhaustive', 1),
                                           ('both', 'balanced', args.repeats), ('both', 'exhaustive', args.repeats)]:
            for repeat in range(repeats):
                cases.append({'name': f'fixed-{strategy}-{search}-{repeat}', 'dataset': 'fixed',
                              'source': args.input, 'reference': args.reference,
                              'strategy': strategy, 'search': search, 'cpuLimit': 0, 'diskTasks': 0})
    if args.suite in ('corpus', 'background', 'all'):
        root = args.output / 'corpus'
        if not root.exists():
            if not (args.project / 'src').is_dir():
                raise ValueError('--project must contain src for a source corpus')
            corpus_tools.corpus(root, args.project, 4)
        manifest = json.loads((args.output / 'corpus-manifest.json').read_text(encoding='utf-8'))
        actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
        assert actual == {f['path'] for f in manifest['files']}, 'Corpus entry set changed'
        for entry in manifest['files']:
            assert compare.sha(root / entry['path']) == entry['sha256'], 'Corpus identity changed'
        for dataset in sorted(p for p in root.iterdir() if p.is_dir()):
            reference = args.output / (dataset.name + '-reference.json')
            files = [{'path': p.relative_to(dataset).as_posix(), 'bytes': p.stat().st_size,
                      'sha256': compare.sha(p)} for p in sorted(dataset.rglob('*')) if p.is_file()]
            directories = sorted(p.relative_to(dataset).as_posix() for p in dataset.rglob('*') if p.is_dir())
            reference.write_text(json.dumps({'files': files, 'directories': directories}, indent=2), encoding='utf-8')
            variants = []
            if args.suite in ('corpus', 'all'):
                variants += [('exhaustive', 0, 0), ('balanced', 0, 0)]
            if args.suite in ('background', 'all') and dataset.name in ('mixed', 'solid'):
                variants += [('balanced', 25, 1)]
            for search, cap, disk in variants:
                for repeat in range(args.repeats):
                    cases.append({'name': f'{dataset.name}-{search}-cpu{cap}-{repeat}',
                                  'dataset': dataset.name, 'source': dataset, 'reference': reference,
                                  'strategy': 'both', 'search': search, 'cpuLimit': cap, 'diskTasks': disk})
    rows = []
    for case in cases:
        output = args.output / case['name']
        measured = output / 'measurement.json'
        expected_input, _ = compare.input_identity(case['source'])
        if measured.exists():
            result = json.loads(measured.read_text(encoding='utf-8'))
            assert result['executableSha256'] == compare.sha(args.exe), 'Binary changed; choose new output directory'
            assert result['backendSha256'] == compare.sha(args.tools / '7z.exe'), 'Backend changed'
            assert result['dllSha256'] == compare.sha(args.tools / '7z.dll'), 'Decoder changed'
            assert result['inputSha256'] == expected_input, 'Input changed; choose new output directory'
            expected_settings = {'ramMiB': args.ram, 'workers': args.workers,
                                 'cpuThreads': args.cpu_threads, 'search': case['search'],
                                 'strategy': case['strategy'], 'cpuLimit': case['cpuLimit'],
                                 'diskTasks': case['diskTasks']}
            assert result.get('runSettings') == expected_settings, 'Run settings changed'
            assert result.get('independentVerification', {}).get('perFileSha256Matched'), 'Incomplete retained case'
            assert compare.sha(output / 'result.7z') == result['outputSha256'], 'Retained output changed'
        else:
            command = [sys.executable, str(Path(__file__).with_name('compare-archivers-windows.py')),
                       '--exe', str(args.exe), '--input', str(case['source']), '--tools', str(args.tools),
                       '--output', str(output), '--reference', str(case['reference']), '--ram', str(args.ram),
                       '--workers', str(args.workers), '--cpu-threads', str(args.cpu_threads),
                       '--strategy', case['strategy'], '--search', case['search'],
                       '--cpu-limit', str(case['cpuLimit']), '--disk-tasks', str(case['diskTasks'])]
            (args.output / 'active-case.json').write_text(json.dumps({'name': case['name'], 'command': command}, indent=2), encoding='utf-8')
            subprocess.run(command, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            result = json.loads(measured.read_text(encoding='utf-8'))
        row = {k: case[k] for k in ('name', 'dataset', 'strategy', 'search', 'cpuLimit', 'diskTasks')}
        row.update({k: result[k] for k in ('seconds', 'outputBytes', 'peakTreeCommittedBytes',
                                         'userCpuSeconds', 'kernelCpuSeconds', 'treeReadTransferBytes',
                                         'treeWriteTransferBytes', 'outputSha256',
                                         'averageTreeCpuPercent', 'averageGuardedChildCpuPercent')})
        application = json.loads((output / 'application-report.json').read_text(encoding='utf-8'))
        row.update({'phaseSeconds': application['phaseSeconds'],
                    'scratchPeakObservedBytes': application['scratchPeakObservedBytes'],
                    'generatedCandidates': application['generatedCandidates'],
                    'scheduledCandidates': application['scheduledCandidates'],
                    'guardedChildCpuSeconds': result['guardedChildCpuSeconds'],
                    'totalProcesses': result['totalProcesses'],
                    'independentVerificationSeconds': result['independentVerification']['seconds']})
        rows.append(row)
        (args.output / 'runs.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
        print(case['name'], result['outputBytes'], round(result['seconds'], 3), flush=True)
    summary = []
    for key in sorted({(r['dataset'], r['strategy'], r['search'], r['cpuLimit']) for r in rows}):
        matches = [r for r in rows if (r['dataset'], r['strategy'], r['search'], r['cpuLimit']) == key]
        summary.append({'dataset': key[0], 'strategy': key[1], 'search': key[2], 'cpuLimit': key[3],
                        'repeats': len(matches), 'medianSeconds': statistics.median(r['seconds'] for r in matches),
                        'minOutputBytes': min(r['outputBytes'] for r in matches),
                        'maxOutputBytes': max(r['outputBytes'] for r in matches),
                        'medianAverageTreeCpuPercent': statistics.median(r['averageTreeCpuPercent'] for r in matches),
                        'medianAverageGuardedChildCpuPercent': statistics.median(r['averageGuardedChildCpuPercent'] for r in matches),
                        'medianPeakTreeCommittedBytes': statistics.median(r['peakTreeCommittedBytes'] for r in matches)})
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    comparisons = []
    for full in summary:
        if full['strategy'] != 'both' or full['search'] != 'exhaustive' or full['cpuLimit']:
            continue
        adaptive = next((r for r in summary if r['dataset'] == full['dataset'] and
                         r['strategy'] == 'both' and r['search'] == 'balanced' and
                         not r['cpuLimit']), None)
        if not adaptive:
            continue
        seconds_saved = 100 * (1 - adaptive['medianSeconds'] / full['medianSeconds'])
        size_increase = 100 * (adaptive['maxOutputBytes'] / full['minOutputBytes'] - 1)
        comparisons.append({'dataset': full['dataset'], 'savedMedianAppTimePercent': seconds_saved,
                            'worstSizeIncreasePercent': size_increase,
                            'timeTargetPercent': 30, 'sizeTolerancePercent': 1,
                            'sizeTargetMet': size_increase <= 1,
                            'timeTargetMet': seconds_saved >= 30,
                            'fullRepeats': full['repeats'], 'adaptiveRepeats': adaptive['repeats']})
    external = None
    if args.external_reference_bytes is not None:
        fixed = [r for r in rows if r['dataset'] == 'fixed']
        external = {'bytes': args.external_reference_bytes,
                    'scope': 'Saved external reference; no external time/resource repeats run by this tool',
                    'cases': [{'name': r['name'], 'bytes': r['outputBytes'],
                               'differenceBytes': r['outputBytes'] - args.external_reference_bytes,
                               'strictlySmaller': r['outputBytes'] < args.external_reference_bytes}
                              for r in fixed]}
    (args.output / 'comparison.json').write_text(json.dumps(
        {'adaptive': comparisons, 'externalReference': external}, indent=2), encoding='utf-8')
    (args.output / 'active-case.json').write_text(json.dumps({'state': 'Completed', 'cases': len(rows)}), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('exe', 'tools', 'output', 'project'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--suite', choices=('fixed', 'corpus', 'background', 'all'), default='all')
    parser.add_argument('--ram', type=int, default=26624)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--cpu-threads', type=int, default=8)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--external-reference-bytes', type=int)
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error('Require at least three repeats')
    if args.external_reference_bytes is not None and args.external_reference_bytes <= 0:
        parser.error('External reference size must be positive')
    for name in ('exe', 'tools', 'output', 'project', 'input', 'reference'):
        value = getattr(args, name)
        if value:
            setattr(args, name, value.resolve())
    run(args)
