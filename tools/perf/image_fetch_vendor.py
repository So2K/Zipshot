"""Fetch pinned OPTIONAL encoder-benchmark sources into ignored build outputs.

The application already vendors its production fpng source. This fetch is only
for reproducing the comparative fpng/FPNGE encoder benchmark, not a dependency
installer or a decoder/fuzzer runner.
"""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PINS = json.loads(Path(__file__).with_name("image_vendor_pins.json").read_text())
PROJECTS = {"fpng": "richgel999/fpng", "fpnge": "veluca93/fpnge"}
for project, pin in PINS.items():
    destination = ROOT / "build/perf/images/vendor" / project
    destination.mkdir(parents=True, exist_ok=True)
    for name, expected in pin["files"].items():
        path = destination / name
        if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == expected:
            continue
        relative = "src/" + name if project == "fpng" and name in {"fpng.cpp", "fpng.h"} else name
        url = f"https://raw.githubusercontent.com/{PROJECTS[project]}/{pin['commit']}/{relative}"
        request = urllib.request.Request(url, headers={"User-Agent": "Zipshot-optional-encoder-benchmark"})
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError(f"Upstream SHA mismatch: {project}/{name}")
        path.write_bytes(data)
    print(f"Verified optional encoder source {project}@{pin['commit']}")
