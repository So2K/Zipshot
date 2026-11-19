"""Serial, synthetic-only image-codec profile. Run after the three image_* builds.

No installed program is launched. ShareX's official methods run through our own
CLI entry point against an isolated copy of its unmodified self-contained runtime.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(command, log):
    start = time.monotonic()
    result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}; see {log}")
    return time.monotonic() - start

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--n", type=int, default=20)
    parser.add_argument("--out", default="build/perf/images/corpus-v1")
    parser.add_argument("--cases", default="ui,text,alpha-ui,alpha,photo,entropy")
    parser.add_argument("--sizes", default="1920x1080,3840x2160")
    args = parser.parse_args()
    out = (ROOT / args.out).resolve()
    allowed = (ROOT / "build/perf/images").resolve()
    if not out.is_relative_to(allowed):
        raise ValueError("Outputs must stay inside task-owned build/perf/images")
    out.mkdir(parents=True, exist_ok=True)
    metadata = {
        "schema": 1, "scope": "image APIs, not application/UI latency",
        "n_warm": args.n, "first_call": "first timed call per operation; earlier verification can warm codecs; not cold startup",
        "priority": "BelowNormal", "affinity_mask": 15, "maximum_allowed_logical_processors": 4,
        "platform": platform.platform(), "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "binaries": {p: sha(ROOT / p) for p in ["build/perf/images/image_native.exe", "build/perf/images/image_fastpng.exe", "build/perf/images/sharex-runtime/image_sharex.exe"]},
        "sharex_runtime": "10.0.14 self-contained official portable runtime",
        "sharex_source": "08bfff15e748a945944e609aca7faffbe888c7af (v22.4.0)",
        "source_files": {p: sha(ROOT / p) for p in ["src/output.cpp", "tools/perf/image_native.cpp", "tools/perf/image_fastpng.cpp", "tools/perf/image_sharex.cs"]},
        "cases": [],
    }
    for size in args.sizes.split(","):
        width, height = map(int, size.split("x"))
        for kind in args.cases.split(","):
            folder = out / f"{size}-{kind}"
            status = folder / "complete.json"
            if status.exists():
                metadata["cases"].append(json.loads(status.read_text(encoding="utf-8")))
                continue
            folder.mkdir(parents=True, exist_ok=True)
            elapsed = {}
            elapsed["native"] = run([ROOT / "build/perf/images/image_native.exe", folder, kind, width, height, args.n, "codecs"], folder / "native.log")
            elapsed["fastpng"] = run([ROOT / "build/perf/images/image_fastpng.exe", folder, kind, width, height, args.n], folder / "fastpng.log")
            elapsed["sharex"] = run([ROOT / "build/perf/images/sharex-runtime/image_sharex.exe", folder, width, height, str(kind.startswith("alpha")).lower(), args.n], folder / "sharex.log")
            native = list(csv.DictReader((folder / "native.csv").open()))
            fast = list(csv.DictReader((folder / "fastpng.csv").open()))
            reference = native[0]["source_sha256"]
            if any(row["source_sha256"] != reference or row["exact_pixels"] != "1" for row in fast):
                raise AssertionError(f"Pixel equality failed for {folder}")
            sharex_meta = json.loads((folder / "sharex-defaults.json").read_text())
            if sharex_meta["source_straight_sha256"] != reference:
                raise AssertionError(f"ShareX source pixels differ for {folder}")
            case = {"id": folder.name, "width": width, "height": height, "kind": kind, "alpha": kind.startswith("alpha"),
                    "folder": str(folder.relative_to(ROOT)).replace("\\", "/"), "input_sha256": reference,
                    "elapsed_s": elapsed, "csv_sha256": {p: sha(folder / p) for p in ["native.csv", "fastpng.csv", "sharex.csv"]}}
            status.write_text(json.dumps(case, indent=2) + "\n", encoding="utf-8")
            metadata["cases"].append(case)
            (out / "profile-manifest.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
            print(json.dumps({"case": case["id"], "status": "PASS", "seconds": round(sum(elapsed.values()), 2)}), flush=True)
    (out / "profile-manifest.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
