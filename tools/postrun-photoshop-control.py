"""One bounded full-input control. Retains all evidence and resumable workspaces."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time
from datetime import datetime, timezone

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-archivers-windows.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--search-seconds", type=int, default=540)
    parser.add_argument("--process-timeout", type=int, default=300)
    parser.add_argument("--dictionary", type=int, default=0)
    args = parser.parse_args()
    package, source, evidence = args.package.resolve(), args.input.resolve(), args.output.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    exe, tools = package / "UltraArchStudio.exe", package / "bin"
    archive, report = evidence / "result.7z", evidence / "application-report.json"
    command = [str(exe), "--cli", "--input", str(source), "--output", str(archive),
               "--temp", str(evidence / "scratch"), "--tools", str(tools), "--report", str(report),
               "--profile", "extreme", "--search", "balanced", "--goal", "size", "--content", "exact",
               "--verifier", "dual", "--ram", "26624", "--cpu-auto", "--grouping-only",
               "--lzma-dictionary", str(args.dictionary), "--ram-candidates", "--ram-group-limit", "40",
               "--ram-temp-budget", "1024", "--probe-sample-mb", "25", "--probe-budget", "20",
               "--probe-timeout", "5", "--probe-passes", "2", "--full-attempt-limit", "2",
               "--time-budget", str(args.search_seconds), "--timeout", str(args.process_timeout)]
    identity = {"inputSha256": comparison.sha(source), "inputBytes": source.stat().st_size,
                "executableSha256": comparison.sha(exe), "backendSha256": comparison.sha(tools / "7z.exe"),
                "dllSha256": comparison.sha(tools / "7z.dll")}
    environment = {"command": command, **identity, "utcStart": datetime.now(timezone.utc).isoformat(),
                   "logicalProcessors": os.cpu_count(), "scope": "One bounded full-input control; not an Exhaustive search or an Ultra7z comparison",
                   "searchBudgetSeconds": args.search_seconds, "externalDeadlineSeconds": 1200,
                   "fullAttemptsPerGroup": 2, "note": "Final verification and assembly can exceed search budget. No OS cache flush."}
    (evidence / "run-environment.json").write_text(json.dumps(environment, indent=2), encoding="utf-8")
    start = time.perf_counter()
    with (evidence / "stdout.log").open("wb") as stdout, (evidence / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
        job = None
        try:
            job = comparison.support.MemoryJob(process, 32768)
            with (evidence / "job-samples.jsonl").open("w", encoding="utf-8") as samples:
                while process.poll() is None:
                    elapsed = time.perf_counter() - start
                    progress = {"elapsedSeconds": elapsed, **comparison.job_stats(job)}
                    samples.write(json.dumps(progress) + "\n"); samples.flush()
                    (evidence / "progress.json").write_text(json.dumps(progress, indent=2), encoding="utf-8")
                    if elapsed > 1200:
                        raise TimeoutError("Explicit external control deadline exceeded; inspect retained workspaces")
                    try: process.wait(timeout=1)
                    except subprocess.TimeoutExpired: pass
            measured = {"seconds": time.perf_counter() - start, "exitCode": process.returncode,
                        **comparison.job_stats(job), "jobNotifications": job.notifications()}
        except BaseException as error:
            (evidence / "failure.json").write_text(json.dumps({"error": str(error),
                "elapsedSeconds": time.perf_counter() - start, "environment": environment}, indent=2), encoding="utf-8")
            raise
        finally:
            if process.poll() is None:
                process.kill(); process.wait()
            if job: job.close()
    measured.update(environment)
    (evidence / "measurement.json").write_text(json.dumps(measured, indent=2), encoding="utf-8")
    if process.returncode: raise RuntimeError("Control failed; evidence retained")
    application = json.loads(report.read_text(encoding="utf-8"))
    assert comparison.sha(source) == identity["inputSha256"], "Source changed"
    assert comparison.sha(exe) == identity["executableSha256"], "Executable changed"
    assert comparison.sha(tools / "7z.exe") == identity["backendSha256"], "Backend changed"
    assert comparison.sha(tools / "7z.dll") == identity["dllSha256"], "Decoder changed"
    with (evidence / "independent-test.log").open("wb") as log:
        checked = subprocess.run([str(tools / "7z.exe"), "t", str(archive)], stdout=log,
                                 stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    assert checked.returncode == 0, "Independent archive CRC test failed"
    measured.update({"outputBytes": archive.stat().st_size, "outputSha256": comparison.sha(archive),
                     "independentArchiveTestPassed": True, "sourceUnchanged": True,
                     "contentVerification": "Application mandatory full manifest/SHA dual verification; independent 7z t",
                     "publicationOutcome": application.get("publicationOutcome"),
                     "searchCompletion": application.get("searchCompletion"), "stopAction": application.get("stopAction"),
                     "sessionRetained": application.get("sessionRetained"), "temporaryIO": application.get("temporaryIO"),
                     "groups": [{"groupKey": g.get("groupKey"), "searchCompletion": g.get("searchCompletion")} for g in application.get("groups", [])]})
    measured["gainBytes"] = measured["inputBytes"] - measured["outputBytes"]
    (evidence / "measurement.json").write_text(json.dumps(measured, indent=2), encoding="utf-8")
    print(json.dumps({key: measured[key] for key in ("seconds", "gainBytes", "publicationOutcome", "searchCompletion")}))


if __name__ == "__main__":
    main()
