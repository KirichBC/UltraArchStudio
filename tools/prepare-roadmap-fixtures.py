"""Create deterministic roadmap fixtures without deleting or replacing data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random

MIB = 1048576


def payload(family, length):
    rng = random.Random({"alpha": 12345, "beta": 67890, "dictionary": 45678}[family])
    vocabulary = [f"{family}_term_{n:03d}" for n in range(128)]
    chunks, size, line = [], 0, 0
    while size < length:
        words = " ".join(rng.choice(vocabulary) for _ in range(12))
        chunk = f"{family} record {line:08d}: {words}\n".encode("ascii")
        chunks.append(chunk)
        size += len(chunk)
        line += 1
    return b"".join(chunks)[:length]


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        if not path.is_file() or path.read_bytes() != data:
            raise RuntimeError(f"Existing artifact differs; refusing to replace: {path}")
        return
    with path.open("xb") as stream:
        stream.write(data)


def manifest(directory):
    return {
        "files": [
            {"path": p.relative_to(directory).as_posix(), "bytes": p.stat().st_size,
             "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(directory.rglob("*")) if p.is_file()
        ],
        "directories": sorted(p.relative_to(directory).as_posix()
                              for p in directory.rglob("*") if p.is_dir()),
    }


def record(directory):
    content = manifest(directory)
    output = directory.parent / "reference-manifest.json"
    put(output, (json.dumps(content, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return {"input": str(directory), "referenceManifest": str(output),
            "files": len(content["files"]), "directories": len(content["directories"]),
            "totalBytes": sum(f["bytes"] for f in content["files"])}


def native_pe_fixture(root):
    if os.name != "nt":
        raise RuntimeError("Native PE fixture requires Windows x64")
    windows = Path(os.environ.get("WINDIR", "C:/Windows"))
    source_root = windows / "System32"
    # A 32-bit Python process sees a redirected System32; Sysnative addresses
    # the actual x64 OS directory when that alias is available.
    if (windows / "Sysnative").is_dir():
        source_root = windows / "Sysnative"
    entries = [(source_root / "kernel32.dll", "kernel32-x64.dll", 0x8664),
               (source_root / "ntdll.dll", "ntdll-x64.dll", 0x8664),
               (source_root / "cmd.exe", "cmd-x64.exe", 0x8664),
               (windows / "SysWOW64" / "cmd.exe", "cmd-x86.exe", 0x014c)]
    directory = root / "pe-native" / "input"
    sources = []
    for source, filename, machine in entries:
        data = source.read_bytes()
        if len(data) < 64 or data[:2] != b"MZ":
            raise RuntimeError(f"Source is not a complete PE file: {source}")
        offset = int.from_bytes(data[60:64], "little")
        if offset + 6 > len(data) or data[offset:offset + 4] != b"PE\0\0" or \
                int.from_bytes(data[offset + 4:offset + 6], "little") != machine:
            raise RuntimeError(f"Unexpected PE architecture: {source}")
        put(directory / filename, data)
        sources.append({"source": str(source), "fixtureName": filename,
                        "machine": hex(machine), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    info = record(directory)
    if not 3 * MIB <= info["totalBytes"] <= 5 * MIB:
        raise RuntimeError(f"PE fixture is outside requested 3–5 MiB: {info['totalBytes']}")
    info["sources"] = sources
    info["sourceFilesExecuted"] = False
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    root = parser.parse_args().output.resolve()
    similarity = root / "similarity-text" / "input"
    similarity.mkdir(parents=True, exist_ok=True)
    for family, folder in [("alpha", "Тексты-日本語"), ("beta", "Данные-β")]:
        data = payload(family, 5 * MIB // 4)
        for copy in range(2):
            put(similarity / folder / f"{family}-копия-{copy + 1}.txt", data)
    (similarity / "Пустая-папка-日本語").mkdir(exist_ok=True)
    dictionary = root / "dictionary-40mib" / "input"
    put(dictionary / "Текст-日本語" / "dictionary-40mib.txt", payload("dictionary", MIB) * 40)
    report = {"schema": 1, "similarityText": record(similarity),
              "dictionary40MiB": record(dictionary),
              "generation": "Fixed PRNG seeds; paired copies 1.25 MiB; repeated text stream 40 MiB."}
    report["peNative"] = native_pe_fixture(root)
    metadata = root / "fixtures.json"
    if metadata.exists():
        previous = json.loads(metadata.read_text(encoding="utf-8"))
        for key, value in report.items():
            if key in previous and previous[key] != value:
                raise RuntimeError(f"Existing fixture metadata differs: {key}")
        updated = dict(previous, **report)
        if updated != previous:
            metadata.write_text(json.dumps(updated, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        report = updated
    else:
        put(metadata, (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
