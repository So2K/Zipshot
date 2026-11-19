"""Export process-tree CPU/RAM curves and their exact CSV data from guest samples."""
import argparse
import csv
import json
import os
from pathlib import Path

from recording_analyze import load, rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    os.environ.setdefault("MPLCONFIGDIR", "D:/ZipshotRuntimeCompare-20261118/matplotlib-cache")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    report = load(args.root / "recording_guest_results.json")
    args.out.mkdir(parents=True, exist_ok=True)
    for case in report["cases"]:
        samples = load(args.root / case["name"] / "recording_samples.json")
        if isinstance(samples, dict):
            samples = [samples]
        parent = case["parent_pid"]
        records, previous = [], None
        for sample in samples:
            processes = rows(sample)
            cpu_parent = sum(p["cpu_seconds"] for p in processes if p["pid"] == parent)
            cpu_children = sum(p["cpu_seconds"] for p in processes if p["pid"] != parent)
            alive = [p for p in processes if p["alive"]]
            t = sample["elapsed_seconds"]
            dt = t - previous[0] if previous else 0
            record = {"elapsed_seconds": t,
                      "parent_cpu_seconds": cpu_parent, "children_cpu_seconds": cpu_children,
                      "parent_cpu_one_core_percent": (cpu_parent - previous[1]) / dt * 100 if dt > 0 else None,
                      "children_cpu_one_core_percent": (cpu_children - previous[2]) / dt * 100 if dt > 0 else None,
                      "tree_private_bytes": sum(p["private_bytes"] for p in alive),
                      "tree_working_set_bytes": sum(p["working_set_bytes"] for p in alive),
                      "parent_private_bytes": sum(p["private_bytes"] for p in alive if p["pid"] == parent),
                      "children_private_bytes": sum(p["private_bytes"] for p in alive if p["pid"] != parent)}
            records.append(record)
            previous = (t, cpu_parent, cpu_children)
        csv_path = args.out / (case["name"] + ".csv")
        with csv_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(records[0]))
            writer.writeheader()
            writer.writerows(records)

        figure, axes = plt.subplots(3, 1, figsize=(12, 8.4), sharex=True, constrained_layout=True)
        t = [row["elapsed_seconds"] for row in records]
        for key, label, color in (("parent_cpu_one_core_percent", "Product parent", "#596576"),
                                  ("children_cpu_one_core_percent", "FFmpeg / descendants", "#3A8DFF")):
            axes[0].plot(t, [row[key] if row[key] is not None else float("nan") for row in records], label=label, color=color, linewidth=1.2)
        axes[0].set_ylabel("CPU (% of one core)")
        for key, label, color in (("tree_private_bytes", "Concurrent tree private", "#3A8DFF"),
                                  ("tree_working_set_bytes", "Concurrent tree working set", "#596576")):
            axes[1].plot(t, [row[key] / 1048576 for row in records], label=label, color=color, linewidth=1.5)
        axes[1].set_ylabel("Memory (MiB)")
        for key, label, color in (("parent_private_bytes", "Product parent private", "#596576"),
                                  ("children_private_bytes", "Children private", "#3A8DFF")):
            axes[2].plot(t, [row[key] / 1048576 for row in records], label=label, color=color, linewidth=1.5)
        axes[2].set_ylabel("Memory split (MiB)")
        axes[2].set_xlabel("Seconds since product process launch")
        for axis in axes:
            axis.set_ylim(bottom=0)
            axis.grid(alpha=.18)
            axis.legend(loc="upper right", framealpha=.9, fontsize=8)
            axis.spines[["top", "right"]].set_visible(False)
        files = case.get("files") or []
        durations = [str((item.get("ffprobe") or {}).get("format", {}).get("duration", "unknown")) for item in files]
        title = case["name"] + "\nRequested " + str(case["requested_seconds"]) + " s / " + str(case["fps"]) + " fps; output duration " + ", ".join(durations) + " s"
        figure.suptitle(title, fontsize=13, fontweight="bold")
        figure.supxlabel("Raw nominal 200 ms samples; actual timestamps retained. 100% CPU = one logical core. Guest RDP display; GPU load not inferred.", fontsize=8)
        png = args.out / (case["name"] + ".png")
        figure.savefig(png, dpi=150)
        plt.close(figure)
        print(png)


if __name__ == "__main__":
    main()
