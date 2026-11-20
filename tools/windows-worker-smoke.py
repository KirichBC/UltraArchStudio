"""Measure the installed Windows worker's IPC, restored-byte sink, and archive formats."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time


class IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in
                ('readOperations', 'writeOperations', 'otherOperations',
                 'readBytes', 'writeBytes', 'otherBytes')]


class BasicLimits(ctypes.Structure):
    _fields_ = [('processTime', ctypes.c_longlong), ('jobTime', ctypes.c_longlong),
                ('flags', wintypes.DWORD), ('minWorkingSet', ctypes.c_size_t),
                ('maxWorkingSet', ctypes.c_size_t), ('activeProcesses', wintypes.DWORD),
                ('affinity', ctypes.c_size_t), ('priority', wintypes.DWORD), ('scheduling', wintypes.DWORD)]


class ExtendedLimits(ctypes.Structure):
    _fields_ = [('basic', BasicLimits), ('io', IoCounters),
                ('processMemory', ctypes.c_size_t), ('jobMemory', ctypes.c_size_t),
                ('peakProcessMemory', ctypes.c_size_t), ('peakJobMemory', ctypes.c_size_t)]


class CompletionPort(ctypes.Structure):
    _fields_ = [('key', ctypes.c_void_p), ('port', wintypes.HANDLE)]


class MemoryJob:
    def __init__(self, process, mib):
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.handles = []
        create = self.kernel.CreateJobObjectW
        create.argtypes, create.restype = [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE
        make_port = self.kernel.CreateIoCompletionPort
        make_port.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.c_size_t, wintypes.DWORD]
        make_port.restype = wintypes.HANDLE
        set_info = self.kernel.SetInformationJobObject
        set_info.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        set_info.restype = wintypes.BOOL
        attach = self.kernel.AssignProcessToJobObject
        attach.argtypes, attach.restype = [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL
        try:
            self.job = create(None, None)
            if not self.job:
                raise ctypes.WinError(ctypes.get_last_error())
            self.handles.append(self.job)
            self.port = make_port(wintypes.HANDLE(-1), None, 0, 1)
            if not self.port:
                raise ctypes.WinError(ctypes.get_last_error())
            self.handles.append(self.port)
            association = CompletionPort(1, self.port)
            limits = ExtendedLimits()
            limits.basic.flags = 0x100 | 0x200 | 0x2000  # process/job memory + kill-on-close
            limits.processMemory = limits.jobMemory = mib * 1048576
            if not set_info(self.job, 7, ctypes.byref(association), ctypes.sizeof(association)) or not \
                    set_info(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or not \
                    attach(self.job, wintypes.HANDLE(int(process._handle))):
                raise ctypes.WinError(ctypes.get_last_error())
        except BaseException:
            self.close()
            raise

    def notifications(self):
        poll = self.kernel.GetQueuedCompletionStatus
        poll.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
                         ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_void_p), wintypes.DWORD]
        poll.restype = wintypes.BOOL
        messages = []
        code, key, pid = wintypes.DWORD(), ctypes.c_size_t(), ctypes.c_void_p()
        while poll(self.port, ctypes.byref(code), ctypes.byref(key), ctypes.byref(pid), 0):
            messages.append(code.value)
        return messages

    def close(self):
        close = self.kernel.CloseHandle
        close.argtypes, close.restype = [wintypes.HANDLE], wintypes.BOOL
        for handle in reversed(self.handles):
            close(handle)
        self.handles.clear()


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def invoke(worker, request, cwd, memory_mib=None):
    body = json.dumps(request, ensure_ascii=False).encode('utf-8')
    before = {p.relative_to(cwd).as_posix(): sha(p) for p in cwd.rglob('*') if p.is_file()}
    start = time.perf_counter()
    process = subprocess.Popen([str(worker)], cwd=cwd, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    job = None
    messages = []
    try:
        if memory_mib:
            # Worker is waiting for stdin; apply limits before sending decoder input.
            job = MemoryJob(process, memory_mib)
        output, error = process.communicate(b'UAV1' + struct.pack('<I', len(body)) + body, timeout=30)
        if job:
            messages = job.notifications()
    except BaseException:
        process.kill()
        process.communicate()
        raise
    finally:
        if job:
            job.close()
    elapsed = time.perf_counter() - start
    counters = IoCounters()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    query = kernel.GetProcessIoCounters
    query.argtypes = [wintypes.HANDLE, ctypes.POINTER(IoCounters)]
    query.restype = wintypes.BOOL
    if not query(wintypes.HANDLE(int(process._handle)), ctypes.byref(counters)):
        raise ctypes.WinError(ctypes.get_last_error())
    assert output[:4] == b'UAV1' and struct.unpack('<I', output[4:8])[0] == len(output) - 8
    reply = json.loads(output[8:])
    after = {p.relative_to(cwd).as_posix(): sha(p) for p in cwd.rglob('*') if p.is_file()}
    assert before == after, 'Worker created or modified files'
    assert not error, error
    return {'exitCode': process.returncode, 'seconds': elapsed, 'reply': reply,
            'io': {name: getattr(counters, name) for name, _ in counters._fields_},
            'replyBytes': len(output), 'filesystemUnchanged': True,
            'memoryLimitMiB': memory_mib, 'jobNotifications': messages}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Windows runtime required')
    worker, tools, output = args.worker.resolve(), args.tools.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'данные 日本語'
    root.mkdir()
    (root / 'empty').mkdir()
    (root / 'повтор.bin').write_bytes(b'Windows streaming SHA sink\n' * 650000)
    # Incompressible data forces multiple volumes; file identities are retained in evidence.
    (root / 'random.bin').write_bytes(os.urandom(2 * 1048576))
    manifest = {'schema': 1, 'files': [{'path': p.relative_to(root).as_posix(),
                'size': p.stat().st_size, 'sha256': sha(p)} for p in sorted(root.rglob('*'))
                if p.is_file()], 'directories': ['empty']}
    manifest_path = output / 'manifest.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
    wrong_manifest = json.loads(json.dumps(manifest))
    wrong_manifest['files'][0]['sha256'] = '0' * 64
    wrong_path = output / 'wrong-content-manifest.json'
    wrong_path.write_text(json.dumps(wrong_manifest, ensure_ascii=False), encoding='utf-8')
    wrong_dll = output / 'wrong-7z.dll'
    wrong_dll.write_bytes((tools / '7z.dll').read_bytes() + b'changed')
    password = 'секрет 日本語 \\" slash\\'
    results = []
    for kind in ('7z', 'volumes', 'sfx'):
        archive = output / ('encrypted.exe' if kind == 'sfx' else kind + '.7z')
        extra = ['-v1m'] if kind == 'volumes' else ['-sfx' + str(tools / '7z.sfx')] if kind == 'sfx' else []
        command = [str(tools / '7z.exe'), 'a', '-y', '-t7z', '-m0=lzma2', '-md=16m',
                   '-p', '-mhe=on', '-sccUTF-8', *extra, str(archive), '.']
        compressed = subprocess.run(command, cwd=root, input=(password + '\n' + password + '\n').encode('utf-8'),
                                    capture_output=True, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        (output / (kind + '-compress.log')).write_bytes(compressed.stdout + compressed.stderr)
        assert compressed.returncode == 0, compressed.stdout
        if kind == 'volumes':
            archive = Path(str(archive) + '.001')
            assert len(list(output.glob('volumes.7z.*'))) >= 3
        request = {'dll': str(tools / '7z.dll'), 'archive': str(archive),
                   'manifest': str(manifest_path), 'manifestHash': sha(manifest_path), 'password': password}
        result = invoke(worker, request, output)
        assert result['exitCode'] == 0 and result['reply']['success'], result
        assert result['reply']['bytes'] == sum(f['size'] for f in manifest['files'])
        # GetProcessIoCounters includes pipe writes; preserve the exact value, not "disk I/O".
        assert result['io']['writeBytes'] == result['replyBytes'], result
        results.append({'case': kind, **result})
        limited = invoke(worker, request, output, memory_mib=16)
        assert limited['exitCode'] != 0 and not limited['reply']['success'], limited
        assert set(limited['jobNotifications']) & {9, 10}, limited
        results.append({'case': kind + '-decoder-oom', **limited})
        for label, changes in [('wrong-password', {'password': 'wrong'}),
                               ('manifest-identity', {'manifestHash': '0' * 64}),
                               ('content-sha', {'manifest': str(wrong_path), 'manifestHash': sha(wrong_path)}),
                               ('dll-identity', {'dll': str(wrong_dll)})]:
            failed = invoke(worker, {**request, **changes}, output)
            assert failed['exitCode'] != 0 and not failed['reply']['success'], failed
            results.append({'case': kind + '-' + label, **failed})
    report = {'schema': 1, 'workerSha256': sha(worker), 'dllSha256': sha(tools / '7z.dll'),
              'cases': results, 'ioScope': 'Worker process transfer counters include IPC. File tree and hashes checked before/after each invocation.'}
    (output / 'worker-results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'passed': len(results), 'restoredBytesPerSuccess': sum(f['size'] for f in manifest['files']),
                      'workerWriteBytes': [r['io']['writeBytes'] for r in results if r['reply']['success']]}))


if __name__ == '__main__':
    main()
