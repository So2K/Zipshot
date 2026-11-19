"""Write complete, nonselective codec statistics and result-equality evidence."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]

def png_metadata(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(path)
    p, chunks, metadata = 8, [], {}
    while p + 12 <= len(data):
        size = struct.unpack_from(">I", data, p)[0]
        name, body = data[p + 4:p + 8], data[p + 8:p + 8 + size]
        if p + size + 12 > len(data):
            raise ValueError(f"Truncated PNG: {path}")
        if zlib.crc32(name + body) != struct.unpack_from(">I", data, p + size + 8)[0]:
            raise ValueError(f"PNG CRC: {path}")
        tag = name.decode("ascii")
        chunks.append(tag)
        if tag in {"IHDR", "sRGB", "gAMA", "pHYs", "iCCP", "eXIf"}:
            metadata[tag] = body.hex()
        p += size + 12
        if tag == "IEND":
            break
    if p != len(data) or not chunks or chunks[-1] != "IEND":
        raise ValueError(f"PNG trailing bytes or no IEND: {path}")
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "chunks": chunks,
            "metadata": metadata, "crc_and_complete_file": True}

def stats(rows, width, height):
    warm = [float(r["ms"]) for r in rows if r["phase"] == "warm"]
    ordered = sorted(warm)
    median = statistics.median(warm)
    return {"n": len(warm), "first_timed_ms": float(rows[0]["ms"]), "median_ms": median,
            "p95_ms": ordered[math.ceil(.95 * len(ordered)) - 1], "min_ms": min(warm), "max_ms": max(warm),
            "mean_ms": statistics.mean(warm), "stdev_ms": statistics.stdev(warm) if len(warm) > 1 else 0,
            "megapixels_per_second": width * height / (median * 1000),
            "encoded_bytes": sorted({int(r["bytes"]) for r in rows}),
            "decoded_sha256": sorted({r["decoded_sha256"] for r in rows if r.get("decoded_sha256")}),
            "encoded_sha256": sorted({r["encoded_sha256"] for r in rows if r.get("encoded_sha256")})}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", default="build/perf/images/corpus-v1")
    args = parser.parse_args()
    corpus = ROOT / args.corpus
    manifest = json.loads((corpus / "profile-manifest.json").read_text())
    report = {"schema": 1, "scope": "image APIs only, not UI or complete-application speed",
              "manifest": manifest, "percentile": "nearest rank; all measured trials retained", "cases": []}
    trials = []
    lines = ["# Image codec profile", "",
             "Synthetic 1080p/4K corpus, 20 warm measurements per operation. Processes use below-normal priority and affinity mask 0xF. Timings include allocation and codec work; verification/hashing is outside the timed interval. Ambient user/background load is uncontrolled.", "",
             "`first` means first **timed** call of that operation, not cold process startup: earlier pixel verification can warm a decoder. Fresh-process cold measurements, when available, are reported separately. PNG candidates preserve exact straight BGRA pixels, dimensions, alpha flag and the baseline IHDR/sRGB/gAMA metadata. Both baseline and candidate omit pHYs (DPI).", "",
             "| Case | WIC PNG ms | fpng slower ms | WIC/fpng | ShareX PNG ms | fpng p95 ms | WIC → fpng bytes |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for case in manifest["cases"]:
        folder = ROOT / case["folder"]
        groups = {}
        for source in ["native.csv", "fastpng.csv", "sharex.csv"]:
            rows = list(csv.DictReader((folder / source).open()))
            for row in rows:
                trials.append({"case": case["id"], "source": source, **row})
                groups.setdefault((row["engine"], row["operation"]), []).append(row)
        measurements = {f"{e}/{o}": stats(rows, case["width"], case["height"]) for (e, o), rows in groups.items()}
        pngs = {p.name: png_metadata(p) for p in folder.glob("*.png")}
        baseline = pngs["engine0.png"]["metadata"]
        for i in range(4):
            candidate = pngs[f"fast{i}.png"]["metadata"]
            for key in ["IHDR", "sRGB", "gAMA", "pHYs"]:
                if candidate.get(key) != baseline.get(key):
                    raise AssertionError((case["id"], i, key))
        a = measurements["zipshot-current/png-encode-memory"]
        b = measurements["fpng-slower/png-encode-memory"]
        c = measurements["sharex-22.4.0/png-encode-memory"]
        lines.append(f"| {case['id']} | {a['median_ms']:.2f} | {b['median_ms']:.2f} | {a['median_ms']/b['median_ms']:.2f}× | {c['median_ms']:.2f} | {b['p95_ms']:.2f} | {a['encoded_bytes'][0]:,} → {b['encoded_bytes'][0]:,} |")
        report["cases"].append({**case, "statistics": measurements, "png_files": pngs,
                                "sharex_defaults": json.loads((folder / "sharex-defaults.json").read_text())})
    lines += ["", "The full JSON and trial CSV include JPEG encode/decode, PNG decode, both stock save configurations, FPNGE alternatives and file sizes; this table does not imply that every operation is faster.", "",
              "ShareX 22.4.0 is executed through its unmodified TaskHelpers.SaveImageAsStream(image, format, taskSettings), TaskHelpers.PrepareImage and ImageData.Write methods, using the vendor's .NET 10.0.14 runtime. Its default PNG compression level is 1 with AllFilters, PNGBitDepth.Default, JPEG quality 90/4:2:0 and automatic JPEG above 2048 **decimal** KB. Zipshot's default threshold is 2048 **binary** KiB. The separately labelled stock matched run uses 2,048,000 bytes; thresholds are not silently equated.", "",
              "ShareX's native image representation is premultiplied BGRA; input alpha premultiplication is performed outside the codec timer. Alpha round-tripping can quantize invisible/partially transparent RGB. Zipshot and its candidates use straight BGRA, and candidate equality is checked against that exact source. Stock automatic save can flatten alpha to JPEG; those alpha save timings are not equal-output comparisons.", "",
              "Quality/metadata limits: 8-bit generated UI, text, opacity gradients, procedural photo-like noise and entropy images only. No user gallery, desktop pixels or clipboard were used. No HDR, external ICC profile, EXIF orientation, palette or 16-bit corpus is covered here. The library-decoder prototype is not approved for production based on these fixtures alone.", "",
              "Production policy proposal: fast lossless PNG for alpha or explicitly forced PNG, retain current WIC PNG-size decision for opaque automatic JPEG. Larger files and slower decoding on some generated PNGs are disclosed in the JSON. No JPEG/video quality change is implemented by this profile.", "",
              "Sources: [ShareX v22.4.0 TaskHelpers](https://github.com/ShareX/ShareX/blob/08bfff15e748a945944e609aca7faffbe888c7af/ShareX/TaskHelpers.cs), [stock Skia helpers](https://github.com/ShareX/ShareX/blob/08bfff15e748a945944e609aca7faffbe888c7af/ShareX.HelpersLib/Helpers/SkiaImageHelpers.cs), [fpng source and Unlicense](https://github.com/richgel999/fpng/tree/925796543b9d26b8edfcdcecd94c1dac280f29fc), [FPNGE source and Apache-2.0](https://github.com/veluca93/fpnge/tree/097ebccf3d3db443d9a8f238b1deed010a82f961).", ""]
    destination = ROOT / "docs/perf"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "image_summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (destination / "image_results.md").write_text("\n".join(lines), encoding="utf-8")
    fields = ["case", "source", "engine", "operation", "phase", "trial", "ms", "bytes", "source_sha256", "encoded_sha256", "decoded_sha256", "exact_pixels", "alpha", "format"]
    with (destination / "image_trials.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=fields, extrasaction="ignore")
        writer.writeheader(); writer.writerows(trials)
    print(json.dumps({"cases": len(report["cases"]), "trials": len(trials), "metadata_crc_complete": "PASS",
                      "summary": "docs/perf/image_summary.json", "report": "docs/perf/image_results.md"}))

if __name__ == "__main__":
    main()
