#!/usr/bin/env python3
"""Fixed hashed corpus, three-or-more repeats, JSON/CSV measurements. No cache flushing."""
import argparse, csv, hashlib, json, os, platform, random, shutil, statistics, struct
import subprocess, time, wave, zipfile, zlib
from pathlib import Path


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''): h.update(chunk)
    return h.hexdigest()


def corpus(root, project, large_mib):
    root.mkdir(parents=True, exist_ok=False)
    rng = random.Random(20261005)
    groups = ['source', 'media', 'precompressed', 'repetition', 'small-files', 'solid', 'mixed']
    for name in groups: (root / name).mkdir()
    for path in sorted((project / 'src').rglob('*')):
        if path.is_file() and path.suffix in ('.cpp', '.hpp'):
            target = root / 'source' / path.relative_to(project / 'src')
            target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(path, target)
    pcm = b''.join(struct.pack('<h', ((n // 16) % 256 - 128) * 100) for n in range(131072))
    with wave.open(str(root / 'media' / 'audio.wav'), 'wb') as out:
        out.setnchannels(1); out.setsampwidth(2); out.setframerate(44100); out.writeframes(pcm)
    width, height = 256, 256
    pixels = b''.join(bytes((x, y, (x + y) % 256)) for y in range(height) for x in range(width))
    bmp = b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
    bmp += struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    (root / 'media' / 'image.bmp').write_bytes(bmp + pixels)
    def png_chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    scanlines = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b'IDAT', zlib.compress(scanlines, 9)) + png_chunk(b'IEND', b'')
    (root / 'precompressed' / 'image.png').write_bytes(png)
    with zipfile.ZipFile(root / 'precompressed' / 'data.zip', 'w', zipfile.ZIP_DEFLATED) as out:
        item = zipfile.ZipInfo('random.bin', (2026, 1, 1, 0, 0, 0)); item.compress_type = zipfile.ZIP_DEFLATED
        out.writestr(item, rng.randbytes(262144))
    (root / 'repetition' / 'blocks.bin').write_bytes((rng.randbytes(4096) + b'ABCD' * 1024) * 128)
    for n in range(500): (root / 'small-files' / f'{n:04}.txt').write_text(f'entry={n}\n' + 'small payload\n' * 8, encoding='utf-8')
    block = rng.randbytes(65536)
    with (root / 'solid' / 'large.bin').open('wb') as out:
        for _ in range(large_mib * 16): out.write(block)
    for name in groups[:-1]: shutil.copytree(root / name, root / 'mixed' / name)
    # Real Windows PE corpus. It is deliberately machine-specific, with hashes captured below.
    if os.name == 'nt':
        (root / 'pe').mkdir()
        windows = Path(os.environ['WINDIR'])
        for folder, arch in [('System32', 'x64'), ('SysWOW64', 'x86')]:
            source = windows / folder / 'kernel32.dll'
            if source.is_file(): shutil.copyfile(source, root / 'pe' / f'kernel32-{arch}.dll')
    files = [{'path': p.relative_to(root).as_posix(), 'size': p.stat().st_size, 'sha256': sha(p)}
             for p in sorted(root.rglob('*')) if p.is_file()]
    manifest = {'schema': 1, 'seed': 20261005, 'largeMiB': large_mib, 'files': files}
    manifest['id'] = hashlib.sha256(json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (root.parent / (root.name + '-manifest.json')).write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return manifest


def metadata(executable, tools, output):
    info = {'os': platform.platform(), 'cpu': platform.processor(), 'logicalCpu': os.cpu_count(),
            'executable': str(executable), 'executableSha256': sha(executable),
            'disk': {'path': str(output), 'freeBytes': shutil.disk_usage(output).free},
            'tools': {p.name: sha(p) for p in sorted(tools.glob('*')) if p.is_file()} if tools else {},
            'cacheConditions': 'OS cache retained. Sequential repetitions; first pass may be colder. No cache flush.'}
    if not tools:
        for name in ('7zz', '7z', '7za'):
            resolved = shutil.which(name)
            if resolved: info['tools'][str(Path(resolved).resolve())] = sha(Path(resolved))
    if os.name == 'nt':
        script = "@{cpu=(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);ram=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory;disks=(Get-CimInstance Win32_DiskDrive | Select-Object Model,MediaType,Size)} | ConvertTo-Json -Depth 5"
        try: info['windowsHardware'] = json.loads(subprocess.check_output(['powershell.exe', '-NoProfile', '-Command', script], text=True))
        except (subprocess.SubprocessError, json.JSONDecodeError): info['windowsHardware'] = 'unavailable'
    return info


def run(args):
    project = Path(__file__).resolve().parent.parent
    executable = args.exe.resolve(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    tools = args.tools.resolve() if args.tools else None
    corpus_root = args.corpus.resolve() if args.corpus else output / 'corpus'
    if args.corpus:
        manifest_path = corpus_root.parent / (corpus_root.name + '-manifest.json')
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        for entry in manifest['files']:
            if sha(corpus_root / entry['path']) != entry['sha256']: raise RuntimeError('Corpus identity mismatch')
        actual = {p.relative_to(corpus_root).as_posix() for p in corpus_root.rglob('*') if p.is_file()}
        if actual != {e['path'] for e in manifest['files']}: raise RuntimeError('Corpus entry mismatch')
    else: manifest = corpus(corpus_root, project, args.large_mib)
    metadata_record = metadata(executable, tools, output)
    (output / 'environment.json').write_text(json.dumps(metadata_record, indent=2), encoding='utf-8')
    rows = []
    for dataset in sorted(corpus_root.iterdir()):
        if not dataset.is_dir() or (args.dataset and dataset.name not in args.dataset): continue
        for ram in args.ram:
            for workers in args.workers:
                for mode in args.mode:
                    for repeat in range(args.repeats):
                        prefix = f'{dataset.name}-{ram}-{workers}-{mode}-{repeat}'
                        target = output / (prefix + '.7z'); report = output / (prefix + '.json')
                        scratch = output / ('scratch-' + prefix)
                        command = [str(executable), '--cli', '--input', str(dataset), '--output', str(target), '--temp', str(scratch),
                                   '--ram', str(ram), '--workers', str(workers), '--search', mode, '--report', str(report), '--verifier', args.verifier]
                        if tools: command += ['--tools', str(tools)]
                        start = time.perf_counter()
                        with (output / (prefix + '.log')).open('wb') as log:
                            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                        elapsed = time.perf_counter() - start
                        result = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
                        row = {'dataset': dataset.name, 'corpusId': manifest['id'], 'ramMiB': ram, 'workers': workers,
                               'mode': mode, 'repeat': repeat, 'seconds': elapsed, 'exitCode': process.returncode,
                               'outputBytes': target.stat().st_size if target.exists() else 0,
                               'scratchPeakObservedBytes': result.get('scratchPeakObservedBytes', 0),
                               'peakCandidateMiB': max((r['peakRamMB'] for r in result.get('results', [])), default=0),
                               'compressionMainProcessReadBytes': sum(r.get('readBytes', 0) for r in result.get('results', [])),
                               'compressionMainProcessWriteBytes': sum(r.get('writeBytes', 0) for r in result.get('results', [])),
                               'ioCountersAvailable': os.name == 'nt',
                               'phaseSeconds': result.get('phaseSeconds', {}), 'stopReason': result.get('stopReason', '')}
                        rows.append(row)
                        (output / 'runs.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
                        print(prefix, process.returncode, round(elapsed, 3), flush=True)
                        if process.returncode: raise RuntimeError('Benchmark run failed; inspect retained report/log')
                        shutil.rmtree(scratch, ignore_errors=True)
    with (output / 'runs.csv').open('w', newline='', encoding='utf-8') as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]) if rows else []); writer.writeheader(); writer.writerows(rows)
    comparisons = []
    for dataset in sorted({r['dataset'] for r in rows}):
        for ram in args.ram:
            for workers in args.workers:
                grouped = {mode: [r for r in rows if r['dataset'] == dataset and r['ramMiB'] == ram and r['workers'] == workers and r['mode'] == mode] for mode in args.mode}
                if not grouped.get('exhaustive'): continue
                baseline_time = statistics.median(r['seconds'] for r in grouped['exhaustive'])
                baseline_size = min(r['outputBytes'] for r in grouped['exhaustive'])
                for mode, values in grouped.items():
                    if not values: continue
                    median = statistics.median(r['seconds'] for r in values)
                    delta = (max(r['outputBytes'] for r in values) / baseline_size - 1) * 100
                    speedup = (1 - median / baseline_time) * 100
                    comparisons.append({'dataset': dataset, 'ramMiB': ram, 'workers': workers, 'mode': mode,
                                        'medianSeconds': median, 'speedupPercent': speedup, 'worstSizeIncreasePercent': delta,
                                        'balancedTargetMet': speedup >= 30 and delta <= 1})
    (output / 'comparison.json').write_text(json.dumps(comparisons, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tools', type=Path); parser.add_argument('--corpus', type=Path)
    parser.add_argument('--repeats', type=int, default=3); parser.add_argument('--large-mib', type=int, default=16)
    parser.add_argument('--ram', nargs='+', type=int, default=[1024, 4096]); parser.add_argument('--workers', nargs='+', type=int, default=[1, 2])
    parser.add_argument('--mode', nargs='+', choices=['fast', 'balanced', 'exhaustive'], default=['balanced', 'exhaustive'])
    parser.add_argument('--verifier', choices=['disk', 'dual', 'stream'], default='disk')
    parser.add_argument('--dataset', nargs='+'); parser.add_argument('--timeout', type=int, default=1800)
    args = parser.parse_args()
    if args.repeats < 3 or args.large_mib < 1: parser.error('Require >=3 repeats and positive corpus size')
    run(args)
