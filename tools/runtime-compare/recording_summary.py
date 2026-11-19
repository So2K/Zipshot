"""Build a transparent local-image result index; never pick a winning repetition."""
import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--docs", type=Path, default=Path("docs/runtime-comparison"))
    args = parser.parse_args()
    base = args.docs
    inputs = [(f"recording_local_zipshot_r{r}.json", f"r{r}", "controlled veryfast CRF23") for r in (1, 2)]
    inputs += [(f"recording_local_sharex22_v30_r{r}.json", f"r{r}", "native software allowed, 10000 kbps configured") for r in (1, 2, 3)]
    inputs += [(f"recording_local_sharex22_g15_r{r}.json", f"r{r}", "native adaptive GIF") for r in (1, 2)]
    inputs += [("recording_factory_local_v30.json", "factory exploratory n=1", "factory software preset, X264Preset omitted")]
    trials = []
    for filename, repeat, profile in inputs:
        path = base / filename
        document = json.loads(path.read_text())
        for case in document["cases"]:
            if not case["long_clip_data_ready"]:
                raise RuntimeError("A selected case lacks a valid long moving clip: " + filename)
            media = case["artifacts"][0]
            tick = media["fixture_tick"]
            duration = float(media["ffprobe"]["format"]["duration"])
            actual_profile = "factory GIF preset, full palette / sierra2_4a; cursor off" if case["name"] == "zipshot-gif15" else profile
            trials.append({"case": case["name"], "repeat": repeat, "profile": actual_profile,
                "source_summary": filename, "source_summary_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "output_seconds": duration, "decoded_frames": media["file_validation"]["decoded_frames"],
                "unique_fixture_ticks": tick["unique_ticks"], "unique_fixture_ticks_per_second": tick["unique_ticks"] / duration,
                "tree_cpu_seconds": case["tree_cpu_seconds"], "parent_cpu_seconds": case["parent_cpu_seconds"],
                "children_cpu_seconds": case["ffmpeg_descendants_cpu_seconds"], "whole_process_wall_seconds": case["wall_seconds"],
                "tree_peak_private_mib": case["tree_peak_concurrent_private_bytes"] / 1048576,
                "tree_peak_working_set_mib": case["tree_peak_concurrent_working_set_bytes"] / 1048576,
                "parent_peak_private_mib": case["parent_peak_sampled_private_bytes"] / 1048576,
                "children_peak_private_mib": case["children_peak_concurrent_private_bytes"] / 1048576,
                "capture_backends": case["successful_ffmpeg_capture_backends"] or ["product-native"],
                "native_result": case["native_result"], "media_sha256": media["sha256"],
                "media_relative_path": media["relative_path"], "x264_settings": media.get("embedded_x264_settings"),
                "gpu_status": case["gpu"]["status"], "full_decode_clean": tick["full_decode_clean"]})
    groups = {}
    for trial in trials:
        key = trial["case"] + " | " + trial["profile"]
        groups.setdefault(key, []).append(trial)
    measures = ("output_seconds", "tree_cpu_seconds", "whole_process_wall_seconds", "tree_peak_private_mib", "tree_peak_working_set_mib", "decoded_frames", "unique_fixture_ticks_per_second")
    aggregates = []
    for key, group in groups.items():
        aggregates.append({"case_profile": key, "n": len(group),
            "statistics": {name: {"median": statistics.median(t[name] for t in group), "min": min(t[name] for t in group), "max": max(t[name] for t in group)} for name in measures}})
    report = {"schema_version": 1,
        "scope": "Canonical local-executable software recording in an isolated Windows Sandbox. No host user desktop was recorded.",
        "notes": ["Both products use 1920x1080, nominal video 30fps or GIF 15fps, cursor/audio disabled.",
                  "Codecs, quality controls, decoded duration and visible fixture cadence differ. These are not equal-quality comparisons.",
                  "Zipshot factory fallback uses ultrafast; the separate controlled software profile uses veryfast CRF23.",
                  "Current ShareX native hardware requirement fails in this guest; MP4 trials explicitly allow software encoding.",
                  "Keep every repetition. Current native MP4 includes one 40 CPU-second / 16-drop outlier alongside two approximately 16 CPU-second / zero-drop runs.",
                  "30Hz fixture state count is not a promise of 60 distinct visual frames. Adaptive GIF may coalesce identical frames.",
                  "The native hardware flag describes encoder acceleration, not total GPU use. The GIF hardware-required profile label does not make its adaptive GIF encoder hardware accelerated.",
                  "No GPU samples could be attributed to the recording process IDs. GPU load is not measured.",
                  "Earlier mapped-input runs include a confirmed VM image-launch transport artifact and are retained as diagnostics, not primary startup comparisons."],
        "trials": trials, "aggregates": aggregates}
    (base / "recording_local_summary.json").write_text(json.dumps(report, indent=2) + "\n")
    fields = [key for key in trials[0] if key not in ("native_result", "x264_settings")]
    with (base / "recording_local_summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for trial in trials:
            writer.writerow({key: trial[key] for key in fields})
    lines = ["# Local-image recording results", "", report["scope"], "", "All repeated observations are retained. Values below are median [min-max]. CPU seconds include the product parent and descendants over the whole product lifecycle. Memory is the peak concurrent sampled process tree.", "", "| Case and profile | n | Encoded seconds | CPU seconds | Peak private MiB | Peak working set MiB | Whole lifecycle seconds |", "|---|---:|---:|---:|---:|---:|---:|"]
    def cell(stat):
        return f"{stat['median']:.2f} [{stat['min']:.2f}-{stat['max']:.2f}]"
    for group in aggregates:
        stats = group["statistics"]
        lines.append("| " + group["case_profile"].replace("|", "\\|") + " | " + str(group["n"]) + " | " + " | ".join(cell(stats[name]) for name in ("output_seconds", "tree_cpu_seconds", "tree_peak_private_mib", "tree_peak_working_set_mib", "whole_process_wall_seconds")) + " |")
    lines += ["", "The factory row is a single exploratory trial. It used a successful DDA fallback variant, while the controlled MP4 trials used GDI. Do not attribute their difference solely to the preset.", ""]
    lines += ["- " + note for note in report["notes"]]
    lines += ["", "Exact per-trial results, media SHA-256, codec settings and input summary hashes are in [recording_local_summary.json](recording_local_summary.json). Machine-readable table data is in [recording_local_summary.csv](recording_local_summary.csv). Raw CPU/RAM curves and their exact CSV samples are under [recording_curves/](recording_curves/).", ""]
    (base / "recording_local_summary.md").write_text("\n".join(lines), encoding="utf-8")
    print("Wrote", len(trials), "trials and", len(aggregates), "explicit profiles")


if __name__ == "__main__":
    main()
