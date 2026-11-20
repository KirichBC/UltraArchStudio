"""One serial quality/control comparison per pinned fixture; no speedup inference."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys


def sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--old", type=Path, required=True)
    parser.add_argument("--new", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--existing-old", type=Path, help="Reuse pinned old measurements and unchanged fixtures; run only the new build")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rng = random.Random(20261011)
    noise = rng.randbytes(1048576)
    fixtures = {
        "text": {"LICENSE": (b"record 2026: content verification and deterministic search\n" * 60000)},
        "incompressible": {"random.bin": rng.randbytes(3 * 1048576)},
        "distant-repeat": {"history.bin": noise + rng.randbytes(2 * 1048576) + noise},
    }
    results = []
    for name, files in fixtures.items():
        fixture_base = args.existing_old or args.output
        root = fixture_base / "fixtures" / name
        reference = fixture_base / (name + "-reference.json")
        if not args.existing_old:
            root.mkdir(parents=True)
            for relative, data in files.items():
                (root / relative).write_bytes(data)
            reference.write_text(json.dumps({"files": [
                {"path": relative, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                for relative, data in files.items()], "directories": []}, indent=2), encoding="utf-8")
        for version, package in (("old", args.old), ("new", args.new)):
            reused = version == "old" and args.existing_old
            destination = (args.existing_old if reused else args.output) / (name + "-" + version)
            command = [sys.executable, str(Path(__file__).with_name("compare-archivers-windows.py")),
                       "--exe", str(package / "UltraArchStudio.exe"), "--tools", str(package / "bin"),
                       "--input", str(root), "--reference", str(reference), "--output", str(destination),
                       "--ram", "1024", "--workers", "2", "--cpu-threads", "4",
                       "--lzma-dictionary", "16", "--strategy", "whole", "--search", "balanced"]
            if not reused:
                subprocess.run(command, check=True, timeout=300)
            measurement = json.loads((destination / "measurement.json").read_text(encoding="utf-8"))
            report = json.loads((destination / "application-report.json").read_text(encoding="utf-8"))
            assert sha(package / "UltraArchStudio.exe") == measurement["executableSha256"], "Pinned executable changed"
            if version == "new":
                old = results[-1]
                assert old["inputSha256"] == measurement["inputSha256"], "Paired input changed"
                assert old["backendSha256"] == measurement["backendSha256"] and old["dllSha256"] == measurement["dllSha256"], "Paired backend changed"
            results.append({"fixture": name, "version": version,
                            "seconds": measurement["seconds"], "outputBytes": measurement["outputBytes"],
                            "executableSha256": measurement["executableSha256"],
                            "backendSha256": measurement["backendSha256"], "dllSha256": measurement["dllSha256"],
                            "inputSha256": measurement["inputSha256"],
                            "perFileSha256Matched": measurement["independentVerification"]["perFileSha256Matched"],
                            "temporaryIO": report.get("temporaryIO"),
                            "searchCompletion": report.get("searchCompletion", "Legacy unspecified")})
            (args.output / "summary.json").write_text(json.dumps({
                "scope": "One sequential run per version and fixture; quality/control evidence, no statistical speedup claim",
                "reusedOldEvidence": str(args.existing_old) if args.existing_old else None,
                "runs": results}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
