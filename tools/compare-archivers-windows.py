"""Run a full UltraArch comparison case with Windows job accounting and independent SHA checks."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

spec = importlib.util.spec_from_file_location('worker_smoke', Path(__file__).with_name('windows-worker-smoke.py'))
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)


class Accounting(ctypes.Structure):
    _fields_ = [(n, ctypes.c_longlong) for n in ('user', 'kernel', 'periodUser', 'periodKernel')] + [
        (n, wintypes.DWORD) for n in ('pageFaults', 'totalProcesses', 'activeProcesses', 'terminatedProcesses')]


class AccountingIo(ctypes.Structure):
    _fields_ = [('accounting', Accounting), ('io', support.IoCounters)]


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def input_identity(path):
    if path.is_file():
        return sha(path), path.stat().st_size
    files = [{'path': p.relative_to(path).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(path.rglob('*')) if p.is_file()]
    directories = sorted(p.relative_to(path).as_posix() for p in path.rglob('*') if p.is_dir())
    encoded = json.dumps({'files': files, 'directories': directories}, sort_keys=True).encode('utf-8')
    return hashlib.sha256(encoded).hexdigest(), sum(p['bytes'] for p in files)


def job_stats(job):
    query = job.kernel.QueryInformationJobObject
    query.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
    query.restype = wintypes.BOOL
    limits, counters = support.ExtendedLimits(), AccountingIo()
    for code, value in ((9, limits), (8, counters)):
        if not query(job.job, code, ctypes.byref(value), ctypes.sizeof(value), None):
            raise ctypes.WinError(ctypes.get_last_error())
    return {'peakTreeCommittedBytes': limits.peakJobMemory,
            'peakSingleProcessCommittedBytes': limits.peakProcessMemory,
            'userCpuSeconds': counters.accounting.user / 10000000,
            'kernelCpuSeconds': counters.accounting.kernel / 10000000,
            'totalProcesses': counters.accounting.totalProcesses,
            'activeProcesses': counters.accounting.activeProcesses,
            'treeReadTransferBytes': counters.io.readBytes, 'treeWriteTransferBytes': counters.io.writeBytes}


def verify(archive, tools, reference, output):
    started = time.perf_counter()
    restored = output / 'independent-restored'
    with (output / 'independent-verify.log').open('wb') as log:
        for arguments in (['t', str(archive)], ['x', '-y', '-o' + str(restored), str(archive)]):
            process = subprocess.run([str(tools / '7z.exe'), *arguments], stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            if process.returncode:
                raise RuntimeError('Independent 7-Zip test/extract failed')
    manifest = json.loads(reference.read_text(encoding='utf-8'))
    expected = {f['path']: f for f in manifest['files']}
    actual = {p.relative_to(restored).as_posix(): p for p in restored.rglob('*') if p.is_file()}
    assert set(actual) == set(expected), 'File path set changed'
    for name, p in actual.items():
        assert p.stat().st_size == expected[name]['bytes'] and sha(p) == expected[name]['sha256'], name
    dirs = {p.relative_to(restored).as_posix() for p in restored.rglob('*') if p.is_dir()}
    assert dirs == set(manifest['directories']), 'Directory set changed'
    return {'files': len(actual), 'directories': len(dirs), 'perFileSha256Matched': True,
            'seconds': time.perf_counter() - started}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--ram', type=int, default=26624)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--cpu-threads', type=int, default=6)
    parser.add_argument('--search', choices=('exhaustive', 'balanced', 'fast'), default='exhaustive')
    parser.add_argument('--strategy', choices=('whole', 'groups', 'both'), default='both')
    parser.add_argument('--cpu-limit', type=int, default=0)
    parser.add_argument('--disk-tasks', type=int, default=0)
    parser.add_argument('--solid', help='Explicit solid policy for focused group comparisons')
    parser.add_argument('--lzma-dictionary', type=int, help='Maximum requested LZMA dictionary in MiB')
    parser.add_argument('--similarity-groups', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Windows runtime required')
    exe, source, tools, output, reference = (p.resolve() for p in
        (args.exe, args.input, args.tools, args.output, args.reference))
    output.mkdir(parents=True, exist_ok=False)
    archive, report, scratch = output / 'result.7z', output / 'application-report.json', output / 'scratch'
    command = [str(exe), '--cli', '--input', str(source), '--output', str(archive), '--temp', str(scratch),
               '--profile', 'extreme', '--ram', str(args.ram), '--workers', str(args.workers), '--cpu-threads', str(args.cpu_threads),
               '--search', args.search, '--goal', 'size', '--content', 'exact', '--verifier', 'dual',
               '--tools', str(tools), '--report', str(report)]
    if args.strategy == 'whole':
        command.append('--no-grouping')
    elif args.strategy == 'groups':
        command.append('--grouping-only')
    command.extend(['--cpu-limit', str(args.cpu_limit), '--disk-tasks', str(args.disk_tasks)])
    if args.solid is not None:
        command.extend(['--solid', args.solid])
    if args.lzma_dictionary is not None:
        command.extend(['--lzma-dictionary', str(args.lzma_dictionary)])
    if args.similarity_groups:
        command.append('--similarity-groups')
    input_hash, input_bytes = input_identity(source)
    evidence = {'schema': 1, 'command': command, 'executableSha256': sha(exe),
                'inputSha256': input_hash, 'inputBytes': input_bytes,
                'logicalProcessors': os.cpu_count(),
                'backendSha256': sha(tools / '7z.exe'), 'dllSha256': sha(tools / '7z.dll'),
                'memoryLimitMiB': args.ram, 'osCache': 'Retained; no cache flush',
                'runSettings': {'ramMiB': args.ram, 'workers': args.workers,
                                'cpuThreads': args.cpu_threads, 'search': args.search,
                                'strategy': args.strategy, 'cpuLimit': args.cpu_limit,
                                'diskTasks': args.disk_tasks},
                'applicationCache': 'Unique private scratch; no candidates or capability records shared between cases',
                'timeScope': 'From process creation to exit; includes unpack, search, built-in verification and publication'}
    (output / 'run-environment.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    started = time.perf_counter()
    with (output / 'application.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        job = None
        try:
            job = support.MemoryJob(process, args.ram)
            with (output / 'job-samples.jsonl').open('w', encoding='utf-8') as samples:
                while process.poll() is None:
                    progress = {'elapsedSeconds': time.perf_counter() - started, **job_stats(job)}
                    samples.write(json.dumps(progress) + '\n')
                    samples.flush()
                    (output / 'progress.json').write_text(json.dumps(progress, indent=2), encoding='utf-8')
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
            measured = {'seconds': time.perf_counter() - started, 'exitCode': process.returncode,
                        **job_stats(job), 'jobNotifications': job.notifications()}
        except BaseException:
            process.kill()
            process.wait()
            raise
        finally:
            if job:
                job.close()
    measured.update(evidence)
    (output / 'measurement.json').write_text(json.dumps(measured, indent=2), encoding='utf-8')
    if process.returncode:
        raise RuntimeError('UltraArch failed; inspect retained report/log')
    assert input_identity(source)[0] == evidence['inputSha256'], 'Input changed'
    assert sha(exe) == evidence['executableSha256'], 'Executable changed during run'
    assert sha(tools / '7z.exe') == evidence['backendSha256'], 'Backend changed during run'
    assert sha(tools / '7z.dll') == evidence['dllSha256'], 'Decoder changed during run'
    application = json.loads(report.read_text(encoding='utf-8'))
    measured['guardedChildCpuSeconds'] = application['guardedChildCpuSeconds']
    denominator = measured['seconds'] * measured['logicalProcessors']
    measured['averageTreeCpuPercent'] = 100 * (measured['userCpuSeconds'] + measured['kernelCpuSeconds']) / denominator
    measured['averageGuardedChildCpuPercent'] = 100 * measured['guardedChildCpuSeconds'] / denominator
    measured['outputBytes'] = archive.stat().st_size
    measured['outputSha256'] = sha(archive)
    measured['independentVerification'] = verify(archive, tools, reference, output)
    (output / 'measurement.json').write_text(json.dumps(measured, indent=2), encoding='utf-8')
    print(json.dumps({k: measured[k] for k in ('seconds','outputBytes','peakTreeCommittedBytes','independentVerification')}))


if __name__ == '__main__':
    main()
