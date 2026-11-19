"""Summarize genuine guest hotkey results without hiding failed or missing trials.

Usage: python tools/runtime-compare/capture_summarize.py capture-result.json ...
Optional --output writes a combined report. A smoke run is never labelled as a
qualified benchmark: at least 20 measured, successful trials are required.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def stats(values):
    values = sorted(values)
    if not values:
        return {"n": 0, "median_ms": None, "p95_ms": None, "min_ms": None, "max_ms": None}
    return {"n": len(values), "median_ms": statistics.median(values),
            "p95_ms": values[math.ceil(len(values) * .95) - 1],
            "min_ms": values[0], "max_ms": values[-1]}


def summarize(path, supplied=None):
    raw = path.read_bytes() if supplied is None else json.dumps(supplied, sort_keys=True).encode()
    data = json.loads(raw.decode("utf-8-sig")) if supplied is None else supplied
    if data.get("schema_version") != 1 or "trials" not in data:
        raise ValueError(f"Not a native hotkey controller result: {path}")
    measured = [t for t in data["trials"] if not t["warmup"]]
    failed = [t["index"] for t in measured if t["status"] != "pass"]
    confirmed = [t for t in measured if t.get("target_window_evidence") and t["visible_upper_bound_ms"] >= 0]
    candidates = [t for t in measured if not t.get("target_window_evidence") and t["visible_upper_bound_ms"] >= 0]
    for trial in confirmed:
        if not 0 <= trial["visible_lower_bound_ms"] <= trial["visible_upper_bound_ms"]:
            raise ValueError(f"Invalid visual-response interval in trial {trial['index']}")
    visual = stats([t["visible_upper_bound_ms"] for t in confirmed])
    visual["estimate"] = "Observed upper bound, not an exact input-to-photon time."
    visual["median_lower_bound_ms"] = statistics.median([t["visible_lower_bound_ms"] for t in confirmed]) if confirmed else None
    visual["median_interval_width_ms"] = statistics.median([t["visible_upper_bound_ms"]-t["visible_lower_bound_ms"] for t in confirmed]) if confirmed else None
    visual["unattributed_candidate_count"] = len(candidates)
    visual["missing_or_unconfirmed_count"] = len(measured)-len(confirmed)
    endpoints = {"target_confirmed_visible_response": visual}
    if data["scenario"] == "full":
        for name, field in (("clipboard_image_observed", "clipboard_observed_ms"),
                            ("closed_wic_image_file_observed", "file_observed_ms")):
            endpoint = stats([t[field] for t in measured if t[field] >= 0])
            endpoint["missing_count"] = len(measured)-endpoint["n"]
            endpoint["estimate"] = "First observed valid image, including observer validation overhead."
            endpoints[name] = endpoint
    required = visual["n"] if data["scenario"] == "region" else min(endpoints["clipboard_image_observed"]["n"], endpoints["closed_wic_image_file_observed"]["n"])
    qualified_count = len(measured) >= 20 and required >= 20 and not failed and data["status"] == "pass"
    dimensions_mismatch = [t["index"] for t in measured if data["scenario"] == "full" and (
        t.get("clipboard_width") != data["desktop"]["width"] or t.get("clipboard_height") != data["desktop"]["height"] or
        t.get("file_width") != data["desktop"]["width"] or t.get("file_height") != data["desktop"]["height"])]
    qualified_count = qualified_count and not dimensions_mismatch
    return {"source_file": path.parent.name+"/"+path.name, "source_sha256": hashlib.sha256(raw).hexdigest(),
            "scenario": data["scenario"], "target_executable": data["target_executable"],
            "desktop": data["desktop"], "observer_roi": data["observer_roi"],
            "input_method": data["input_method"], "scope": data["scope"],
            "measured_trials": len(measured), "warmup_trials": len(data["trials"])-len(measured),
            "failed_trial_indices": failed, "controller_status": data["status"],
            "full_capture_dimensions_mismatch_indices": dimensions_mismatch,
            "minimum_sample_count_gate_passed": qualified_count,
            "max_observer_gap_ms": max((t["max_observer_gap_ms"] for t in measured), default=None),
            "endpoints": endpoints,
            "limitations": ["Raw video and actual product settings still require review; the sample-count gate alone is not full validation.",
                            "Missing and failed trials are reported; successful-endpoint statistics are conditional on completion.",
                            "Visual-response intervals reflect compositor readback sampling uncertainty.",
                            "Unattributed visual changes require video review and are excluded from confirmed product-response statistics.",
                            "These are virtual-machine hotkey results, separate from the prior host capture API benchmark."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--merge", action="store_true", help="Pool same-workload blocks after verifying matching metadata and observer settings.")
    args = parser.parse_args()
    report = {"schema_version": 1, "runs": [summarize(path) for path in args.results]}
    if args.merge:
        sources = [json.loads(path.read_text(encoding="utf-8-sig")) for path in args.results]
        same_fields = ("scenario", "target_executable", "desktop", "observer_roi", "input_method", "visual_method", "scope")
        for source in sources[1:]:
            if any(source[field] != sources[0][field] for field in same_fields):
                raise ValueError("Cannot pool different scenarios, desktops, ROIs or measurement methods.")
        metadata = [json.loads((path.parent/"metadata.json").read_text(encoding="utf-8-sig")) for path in args.results]
        identity = ("app", "app_sha256", "controller_sha256", "observer_enabled", "version")
        if any(any(meta[field] != metadata[0][field] for field in identity) for meta in metadata[1:]):
            raise ValueError("Cannot pool different app identities, controller builds or recording workloads.")
        combined = dict(sources[0])
        combined["trials"] = [trial for source in sources for trial in source["trials"]]
        combined["status"] = "pass" if all(source["status"] == "pass" for source in sources) else "fail"
        report["merged"] = summarize(Path("derived/merged-blocks.json"), combined)
        report["merged"]["block_count"] = len(sources)
        report["merged"]["app"] = metadata[0]["app"]
        report["merged"]["version"] = metadata[0]["version"]
        report["merged"]["observer_enabled"] = metadata[0]["observer_enabled"]
        report["merged"]["source_blocks"] = [run["source_file"] for run in report["runs"]]
        report["merged"]["failed_trial_references"] = [{"block": path.parent.name, "index": trial["index"]}
            for path, source in zip(args.results, sources) for trial in source["trials"] if not trial["warmup"] and trial["status"] != "pass"]
    text = json.dumps(report, indent=2, ensure_ascii=False)+"\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
