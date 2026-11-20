"""Account an explicitly selected idle Ultra7z GUI and its children until a stop marker.

Does not operate the GUI. Closing the monitor leaves the application alive.
"""
import argparse
import ctypes
from ctypes import wintypes
import importlib.util
import json
from pathlib import Path
import time

spec = importlib.util.spec_from_file_location('comparison', Path(__file__).with_name('compare-archivers-windows.py'))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)
support = comparison.support


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ram', type=int, default=26624)
    args = parser.parse_args()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    handle = kernel.OpenProcess(0x100 | 0x1000 | 0x1, False, args.pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    size = wintypes.DWORD(32768)
    name = ctypes.create_unicode_buffer(size.value)
    if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)):
        kernel.CloseHandle(handle)
        raise ctypes.WinError(ctypes.get_last_error())
    if Path(name.value).resolve() != args.exe.resolve():
        kernel.CloseHandle(handle)
        raise RuntimeError('Selected PID does not match the expected test executable')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    job = None
    # Build a job without kill-on-close, so monitor errors cannot terminate the GUI.
    class AccountJob:
        close = support.MemoryJob.close
    job = AccountJob()
    job.kernel, job.handles = kernel, []
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    try:
        job.job = kernel.CreateJobObjectW(None, None)
        if not job.job:
            raise ctypes.WinError(ctypes.get_last_error())
        job.handles.append(job.job)
        limits = support.ExtendedLimits()
        limits.basic.flags = 0x100 | 0x200
        limits.processMemory = limits.jobMemory = args.ram * 1048576
        if not kernel.SetInformationJobObject(job.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or not kernel.AssignProcessToJobObject(job.job, handle):
            raise ctypes.WinError(ctypes.get_last_error())
        environment = {'pid': args.pid, 'executable': name.value,
                       'executableSha256': comparison.sha(args.exe), 'memoryLimitMiB': args.ram,
                       'scope': 'GUI plus descendants after attachment; committed memory and transfer counters',
                       'timing': 'Child activity envelope excludes idle GUI setup; confirm completion manually',
                       'largePages': 'Requested in GUI; actual use not established by this monitor'}
        (output / 'run-environment.json').write_text(json.dumps(environment, indent=2), encoding='utf-8')
        started, first, last = time.perf_counter(), None, None
        with (output / 'samples.jsonl').open('w', encoding='utf-8') as log:
            while not (output / 'stop-monitor').exists():
                elapsed = time.perf_counter() - started
                stats = comparison.job_stats(job)
                if stats['activeProcesses'] > 1:
                    first = elapsed if first is None else first
                    last = elapsed
                progress = {'monitorSeconds': elapsed, 'firstChildObservedSeconds': first,
                            'lastChildObservedSeconds': last, **stats}
                log.write(json.dumps(progress) + '\n')
                log.flush()
                (output / 'progress.json').write_text(json.dumps(progress, indent=2), encoding='utf-8')
                if stats['activeProcesses'] == 0:
                    break
                time.sleep(1)
        progress['childActivityEnvelopeSeconds'] = None if first is None else last - first
        progress['samplingIntervalSeconds'] = 1
        (output / 'measurement.json').write_text(json.dumps(progress, indent=2), encoding='utf-8')
        print(json.dumps(progress))
    finally:
        job.close()
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    main()
