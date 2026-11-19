"""Summarize guest recording process samples and decode the original fixture tick.

This reads only explicitly supplied task output media. It never captures a desktop.
CPU is process CPU seconds, with parent and descendants separated. Tree memory is
the peak concurrent sum of alive process samples, not a sum of per-process peaks.
"""
import argparse
import json
import math
import re
import statistics
import subprocess
from pathlib import Path


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def rows(sample):
    value = sample.get("processes", [])
    return [value] if isinstance(value, dict) else value


def read_ticks(ffmpeg, path, origin=(32, 32)):
    # Bit i is a 16px black/white box at x=32+20*i, y=32 in the fixture.
    command = [ffmpeg, "-v", "error", "-threads", "1", "-i", str(path), "-filter_threads", "1", "-vf", f"crop=320:16:{origin[0]}:{origin[1]},scale=320:1:flags=area,format=gray",
               "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    result = subprocess.run(command, capture_output=True, timeout=180)
    if result.returncode:
        return {"status": "decode_failed", "exit_code": result.returncode}
    data = result.stdout
    ticks, ambiguous = [], 0
    for offset in range(0, len(data) - 319, 320):
        samples = [data[offset + i * 20 + 8] for i in range(16)]
        ambiguous += any(50 < value < 205 for value in samples)
        ticks.append(sum((value >= 128) << i for i, value in enumerate(samples)))
    changes = [(b - a) % 65536 for a, b in zip(ticks, ticks[1:])]
    good = [value for value in changes if value < 120]
    return {"status": "decoded" if not result.stderr else "decoded_with_errors", "full_decode_clean": not bool(result.stderr), "frames": len(ticks), "first_tick": ticks[0] if ticks else None,
            "last_tick": ticks[-1] if ticks else None, "ambiguous_frames": ambiguous,
            "repeated_tick_transitions": sum(value == 0 for value in changes),
            "tick_jumps_over_one": sum(value > 1 for value in good),
            "implausible_transitions": sum(value >= 120 for value in changes),
            "unique_ticks": len(set(ticks)), "deltas": dict((str(v), changes.count(v)) for v in sorted(set(changes))),
            "interpretation": "Fixture is 30Hz. Repeated ticks are expected at 60fps, and jumps of two are expected at 15fps. These are fixture presentation observations, not an encoder internal dropped-frame counter."}


def summarize(case, root, logical, ffmpeg, decode_ticks, tick_origin=(32, 32)):
    samples = load(root / case["name"] / "recording_samples.json")
    if isinstance(samples, dict):
        samples = [samples]
    parent = case["parent_pid"]
    final = case["final_processes"]
    if isinstance(final, dict):
        final = [final]
    cpu_parent = sum(row["cpu_seconds"] for row in final if row["pid"] == parent)
    cpu_children = sum(row["cpu_seconds"] for row in final if row["pid"] != parent)
    alive_totals = [{"t": sample["elapsed_seconds"],
                     "private": sum(row["private_bytes"] for row in rows(sample) if row["alive"]),
                     "ws": sum(row["working_set_bytes"] for row in rows(sample) if row["alive"]),
                     "parent_private": sum(row["private_bytes"] for row in rows(sample) if row["alive"] and row["pid"] == parent),
                     "parent_ws": sum(row["working_set_bytes"] for row in rows(sample) if row["alive"] and row["pid"] == parent),
                     "children_private": sum(row["private_bytes"] for row in rows(sample) if row["alive"] and row["pid"] != parent),
                     "children_ws": sum(row["working_set_bytes"] for row in rows(sample) if row["alive"] and row["pid"] != parent)} for sample in samples]
    commands = case.get("ffmpeg_commands") or []
    if isinstance(commands, dict):
        commands = [commands]
    successful = {row["pid"] for row in final if row.get("exit_code") == 0 and row["pid"] != parent}
    capture_pids = {row["pid"] for row in commands if row["pid"] in successful and re.search(r"ddagrab|gdigrab", row.get("command") or "")}
    capture_backends = sorted({"ddagrab" if "ddagrab" in (item.get("command") or "") else "gdigrab" for item in commands if item["pid"] in capture_pids})
    conversion_pids = {row["pid"] for row in commands if row["pid"] in successful and row["pid"] not in capture_pids and re.search(r"palettegen|paletteuse|-f concat", row.get("command") or "")}
    # Command-line lookup can stall on the first CIM call. The sampled process
    # snapshot, rather than completion of that lookup, defines first observation.
    first_capture = min((sample["elapsed_seconds"] for sample in samples if any(row["pid"] in capture_pids for row in rows(sample))), default=None)
    intervals = [b["elapsed_seconds"] - a["elapsed_seconds"] for a, b in zip(samples, samples[1:])]
    capture_alive_times = [sample["elapsed_seconds"] for sample in samples if any(row["pid"] in capture_pids and row["alive"] for row in rows(sample))]
    conversion_alive_times = [sample["elapsed_seconds"] for sample in samples if any(row["pid"] in conversion_pids and row["alive"] for row in rows(sample))]
    steady = []
    if first_capture is not None:
        for sample in samples:
            if sample["elapsed_seconds"] >= first_capture + 2 and any(row["pid"] in capture_pids and row["alive"] for row in rows(sample)):
                steady.append(sample)
    steady_result = None
    if len(steady) >= 2:
        beginning = {row["pid"]: row["cpu_seconds"] for row in rows(steady[0])}
        ending = {row["pid"]: row["cpu_seconds"] for row in rows(steady[-1])}
        elapsed = steady[-1]["elapsed_seconds"] - steady[0]["elapsed_seconds"]
        cpu = sum(max(0, value - beginning.get(pid, value)) for pid, value in ending.items())
        steady_result = {"sampled_seconds": elapsed, "cpu_seconds": cpu, "one_core_percent": cpu / elapsed * 100,
                         "guest_machine_percent": cpu / elapsed / logical * 100,
                         "note": "Starts two seconds after the successful capture FFmpeg child was first observed; interval is actual and may be shorter than the requested clip."}
    native_window = None
    request, complete = case.get("native_start_observed_seconds"), case.get("completed_file_observed_seconds")
    if request is not None and complete is not None:
        window = [sample for sample in samples if request <= sample["elapsed_seconds"] <= complete]
        if len(window) >= 2:
            beginning = {row["pid"]: row["cpu_seconds"] for row in rows(window[0])}
            ending = {row["pid"]: row["cpu_seconds"] for row in rows(window[-1])}
            elapsed = window[-1]["elapsed_seconds"] - window[0]["elapsed_seconds"]
            cpu = sum(max(0, value - beginning.get(pid, value)) for pid, value in ending.items())
            native_window = {"observed_seconds": elapsed, "cpu_seconds": cpu, "one_core_percent": cpu / elapsed * 100,
                "guest_machine_percent": cpu / elapsed / logical * 100,
                "scope": "Observed product native-record request through completed-file observation. Includes setup bar, preparation and finalization; this is not a pure steady encoder interval."}
    artifacts = []
    for media in case.get("files", []):
        artifact = dict(media)
        path = root / media["relative_path"].replace("\\", "/")
        if decode_ticks and path.exists():
            artifact["fixture_tick"] = read_ticks(ffmpeg, path, tick_origin)
        streams = (media.get("ffprobe") or {}).get("streams") or []
        stream = streams[0] if streams else {}
        counted = str(stream.get("nb_read_frames", ""))
        artifact["file_validation"] = {"codec": stream.get("codec_name"), "width": stream.get("width"), "height": stream.get("height"),
            "decoded_frames": int(counted) if counted.isdigit() else None,
            "expected_dimensions_and_decodable_frames": bool(media.get("ffprobe_exit") == 0 and stream.get("width") == 1920 and stream.get("height") == 1080 and counted.isdigit() and int(counted) > 0)}
        if path.exists() and path.suffix.lower() == ".mp4":
            payload = path.read_bytes()
            if b"x264 - core" in payload:
                settings = {}
                for key in ("threads", "lookahead_threads", "sliced_threads", "rc_lookahead", "crf", "bframes", "ref"):
                    match = re.search(rb"(?:^| )" + key.encode() + rb"=([0-9.]+)", payload)
                    if match:
                        settings[key] = match.group(1).decode()
                artifact["embedded_x264_settings"] = settings
        artifacts.append(artifact)
    native_result = case.get("native_result")
    native_result_source = "guest_recording_result" if native_result else None
    product_log = root / case["name"] / "recording_product_log.txt"
    if native_result is None and product_log.exists():
        match = re.search(r"Native screen recording completed\. Encoder: (.*?), hardware: (True|False), frames: (\d+), dropped: (\d+), audio discontinuities: (\d+)", product_log.read_text(encoding="utf-8-sig"))
        if match:
            native_result = {"encoder": match.group(1), "hardware_accelerated": match.group(2) == "True",
                "video_frames": int(match.group(3)), "dropped_video_frames": int(match.group(4)), "audio_discontinuities": int(match.group(5))}
            native_result_source = "actual_product_log"
    valid_artifacts = [artifact for artifact in artifacts if artifact["file_validation"]["expected_dimensions_and_decodable_frames"]]
    moving = [artifact for artifact in valid_artifacts if artifact.get("fixture_tick", {}).get("full_decode_clean") and artifact["fixture_tick"].get("ambiguous_frames") == 0 and artifact["fixture_tick"].get("unique_ticks", 0) > 1]
    durations = [float((artifact.get("ffprobe") or {}).get("format", {}).get("duration", 0)) for artifact in moving]
    gpu_path = root / case["name"] / "recording_gpu_samples.jsonl"
    engines = {}
    gpu_status = "no_samples"
    if gpu_path.exists():
        own = {row["pid"] for row in final}
        for line in gpu_path.read_text(encoding="utf-8-sig").splitlines():
            batch = json.loads(line)
            if "unavailable" in batch:
                gpu_status = "unavailable_" + batch["unavailable"]
                continue
            gpu_status = "sampled"
            values = {}
            for engine in batch.get("engines", []):
                match = re.search(r"pid_(\d+).*engtype_([^_]+)", engine["instance"])
                if match and int(match.group(1)) in own:
                    kind = match.group(2)
                    values[kind] = values.get(kind, 0) + engine["percent"]
            for kind, value in values.items():
                engines.setdefault(kind, []).append(value)
        if gpu_status == "sampled" and not engines:
            gpu_status = "counter_available_no_owned_process_samples"
    process_memory = []
    command_by_pid = {item["pid"]: item.get("command") or "" for item in commands}
    for process in final:
        pid = process["pid"]
        observations = [row for sample in samples for row in rows(sample) if row["pid"] == pid and row["alive"]]
        command = command_by_pid.get(pid, "")
        role = "parent" if pid == parent else "capture" if pid in capture_pids else "palettegen" if "palettegen" in command else "paletteuse" if "paletteuse" in command else "remux" if "-c copy" in command else "other_or_failed"
        process_memory.append({"pid": pid, "role": role, "cpu_seconds": process["cpu_seconds"],
            "peak_sampled_private_bytes": max((row["private_bytes"] for row in observations), default=0),
            "peak_sampled_working_set_bytes": max((row["working_set_bytes"] for row in observations), default=0)})
    return {"name": case["name"], "timed_out": case["timed_out"], "failure": case.get("failure"), "cleanup_note": case.get("cleanup_note"), "wall_seconds": case["wall_seconds"],
            "sampling_seconds": {"median": statistics.median(intervals) if intervals else None, "max": max(intervals) if intervals else None, "count": len(samples)},
            "valid_media_count": len(valid_artifacts), "has_verified_moving_media": bool(moving),
            "long_clip_data_ready": bool(durations and min(durations) >= 15 and not case["timed_out"] and not case.get("failure")),
            "comparison_note": "Media validity and duration do not establish a codec quality match or a general performance ranking.",
            "parent_cpu_seconds": cpu_parent, "ffmpeg_descendants_cpu_seconds": cpu_children,
            "tree_cpu_seconds": cpu_parent + cpu_children,
            "tree_peak_concurrent_private_bytes": max((item["private"] for item in alive_totals), default=0),
            "tree_peak_concurrent_working_set_bytes": max((item["ws"] for item in alive_totals), default=0),
            "parent_peak_sampled_private_bytes": max((item["parent_private"] for item in alive_totals), default=0),
            "parent_peak_sampled_working_set_bytes": max((item["parent_ws"] for item in alive_totals), default=0),
            "children_peak_concurrent_private_bytes": max((item["children_private"] for item in alive_totals), default=0),
            "children_peak_concurrent_working_set_bytes": max((item["children_ws"] for item in alive_totals), default=0),
            "first_successful_capture_child_observed_seconds": first_capture, "steady_capture": steady_result,
            "native_request_to_file_window": native_window, "native_result": native_result, "native_result_source": native_result_source,
            "configured_profile": case.get("profile", case.get("video_profile")), "configured_bitrate_kbps": case.get("video_bitrate_kbps"),
            "successful_ffmpeg_capture_backends": capture_backends,
            "software_preset": case.get("software_preset"), "process_memory_peaks": process_memory,
            "phase_breakdown": {"capture_children_cpu_seconds": sum(row["cpu_seconds"] for row in final if row["pid"] in capture_pids),
                "conversion_children_cpu_seconds": sum(row["cpu_seconds"] for row in final if row["pid"] in conversion_pids),
                "other_or_failed_children_cpu_seconds": sum(row["cpu_seconds"] for row in final if row["pid"] != parent and row["pid"] not in capture_pids | conversion_pids),
                "capture_child_last_alive_observed_seconds": max(capture_alive_times) if capture_alive_times else None,
                "conversion_children_observed_span_seconds": max(conversion_alive_times) - min(conversion_alive_times) if conversion_alive_times else None,
                "post_capture_to_process_exit_seconds": case["wall_seconds"] - max(capture_alive_times) if capture_alive_times else None,
                "note": "Child CPU totals use retained handles. Phase boundaries and conversion spans use approximately 200ms observations; muxer finalization inside the capture child is not separately exposed."},
            "gpu": {"status": gpu_status, "per_engine_sum_for_owned_pids": {kind: {"mean_percent": statistics.mean(values), "peak_percent": max(values), "samples": len(values)} for kind, values in engines.items()},
                    "note": "Different engines must not be summed into a single physical-GPU utilization percentage."},
            "processes": final, "artifacts": artifacts}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--out", type=Path, default=Path("docs/runtime-comparison/recording_results.json"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--decode-ticks", action="store_true")
    parser.add_argument("--tick-x", type=int, default=32)
    parser.add_argument("--tick-y", type=int, default=32)
    args = parser.parse_args()
    report = load(args.root / "recording_guest_results.json")
    result = {"schema_version": 1, "scope": report["scope"], "io_note": report.get("io_note"),
              "executable_location": report.get("executable_location", "earlier mapped-input lab run"),
              "logical_processors": report["logical_processors"],
              "fixture_tick_origin": [args.tick_x, args.tick_y],
              "cases": [summarize(case, args.root, report["logical_processors"], args.ffmpeg, args.decode_ticks, (args.tick_x, args.tick_y)) for case in report["cases"]]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for case in result["cases"]:
        print(case["name"], "CPU s", round(case["tree_cpu_seconds"], 3), "peak private MiB", round(case["tree_peak_concurrent_private_bytes"] / 1048576, 1), "files", len(case["artifacts"]))


if __name__ == "__main__":
    main()
