"""Map final 7z blocks and pair observed ETW process lifetimes without merging reused PIDs."""
import argparse
import collections
import ctypes
from ctypes import wintypes
import datetime
import json
from pathlib import Path


def blocks(path):
    records = []
    for entry in path.read_text(encoding='utf-8-sig').replace('\r\n', '\n').split('\n\n'):
        values = dict(line.split(' = ', 1) for line in entry.splitlines() if ' = ' in line)
        if values.get('Block'):
            records.append(values)
    grouped = {}
    for item in records:
        group = grouped.setdefault(item['Block'], {'method': item['Method'], 'files': 0,
                                                  'unpackedBytes': 0, 'packedBytes': 0, 'extensions': {}})
        if group['method'] != item['Method']:
            raise ValueError('Different method descriptions within one block')
        group['files'] += 1
        group['unpackedBytes'] += int(item.get('Size') or 0)
        # 7z -slt reports packed size only on the first file of a solid block.
        group['packedBytes'] += int(item.get('Packed Size') or 0)
        suffix = Path(item['Path']).suffix.lower() or '(none)'
        group['extensions'][suffix] = group['extensions'].get(suffix, 0) + 1
    return grouped


def arguments(command):
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LocalFree.argtypes = [wintypes.HLOCAL]
    count = ctypes.c_int()
    result = shell.CommandLineToArgvW(command, ctypes.byref(count))
    if not result:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return [result[n] for n in range(count.value)]
    finally:
        kernel.LocalFree(result)


def lifetime(events):
    active, processes, unmatched = {}, [], collections.Counter()
    for line in events.read_text(encoding='utf-8').splitlines():
        event = json.loads(line)
        # Mentioning 7z in another process's argv does not make it a 7z lifetime.
        image = (event.get('ImageFileName') or '').lower()
        if image not in ('7z.exe', 'ultra7z archive optimizer.exe'):
            continue
        key = (event['UniqueProcessKey'], event['ProcessId'])
        opcode = event.get('opcodeName')
        if opcode in ('Start', 'DCStart'):
            if key in active:
                unmatched['supersededStart'] += 1
            record = {'pid': int(event['ProcessId'], 16), 'processKey': key[0], 'image': image,
                      'start': event['time'], 'startObserved': opcode == 'Start',
                      'command': event.get('CommandLine') or ''}
            active[key] = record
            processes.append(record)
        elif opcode in ('End', 'DCEnd'):
            record = active.pop(key, None)
            if record is None:
                unmatched['endWithoutStart'] += 1
                continue
            record['end'] = event['time']
            record['endObserved'] = opcode == 'End'
            record['exitStatus'] = int(event['ExitStatus'].strip(), 0)
            if record['startObserved'] and record['endObserved']:
                a, b = (datetime.datetime.fromisoformat(record[k]) for k in ('start', 'end'))
                record['seconds'] = (b - a).total_seconds()
    classes = collections.Counter()
    for record in processes:
        args = arguments(record['command']) if record['command'] else []
        record['arguments'] = args
        record['codecFlags'] = [a for a in args[1:] if a.startswith('-m')]
        record['listFiles'] = [a[1:] if a.startswith('@') else a.split('@', 1)[1]
                               for a in args[1:] if a.startswith('@') or a.lower().startswith(('-ir@', '-i@'))]
        record['archivePaths'] = [a for a in args[1:] if not a.startswith(('-', '@')) and a.lower().endswith('.7z')]
        record['verb'] = args[1].lower() if len(args) > 1 else ''
        # A target name labels a trial/final update; only final archive blocks prove the winner.
        classes[record['verb']] += 1
    return {'lifetimes': processes, 'unmatched': dict(unmatched), 'verbs': dict(classes),
            'observedStarts': sum(p['startObserved'] for p in processes),
            'completeLifetimes': sum('seconds' in p for p in processes),
            'sub10msLifetimes': sum(p.get('seconds', 1) < 0.01 for p in processes),
            'scope': 'ETW began after optimization started. DCStart/DCEnd are boundary snapshots. Event timestamps retain tracerpt timezone; only paired duration differences are used.'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ultra7z-list', type=Path, required=True)
    parser.add_argument('--ultraarch-list', type=Path, required=True)
    parser.add_argument('--events', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'schema': 1, 'ultra7zBlocks': blocks(args.ultra7z_list),
              'ultraarchBlocks': blocks(args.ultraarch_list), 'etw': lifetime(args.events)}
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps({'ultra7zBlocks': len(report['ultra7zBlocks']),
                      'ultraarchBlocks': len(report['ultraarchBlocks']),
                      **{k: v for k, v in report['etw'].items() if k != 'lifetimes'}}))


if __name__ == '__main__':
    main()
