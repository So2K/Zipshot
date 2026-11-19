"""Hash-copy finished recording jobs only. Never copy Sandbox host/RDP session files."""
import argparse
import hashlib
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--dest", type=Path, required=True)
    args = parser.parse_args()
    stage = args.stage.resolve()
    if not (stage / ".zipshot-runtime-session.json").is_file():
        raise RuntimeError("Unknown task-owned session")
    destination = args.dest.resolve()
    expected = Path("J:/TerraMaster/SSD1/Files/Zipshot/runtime-compare-20261118/recording").resolve()
    if not destination.is_relative_to(expected):
        raise RuntimeError("Destination is outside the specifically authorized recording archive")
    destination.mkdir(parents=True, exist_ok=True)
    entries, excluded, jobs = [], [], []

    def copy(source, relative):
        source = source.resolve()
        if not source.is_relative_to(stage):
            raise RuntimeError("Source escaped the owned session")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        original = digest(source)
        if target.exists():
            if digest(target) != original:
                raise RuntimeError("Existing archived file differs: " + str(relative))
        else:
            shutil.copy2(source, target)
        if digest(target) != original or digest(source) != original:
            raise RuntimeError("Archive verification failed: " + str(relative))
        entries.append({"relative_path": relative.as_posix(), "source_relative_path": source.relative_to(stage).as_posix(),
                        "bytes": source.stat().st_size, "sha256": original})

    for job_path in sorted((stage / "input/jobs").glob("*.json")):
        job = read(job_path)
        if job.get("kind") != "recording":
            continue
        status_path = stage / "output" / (job["id"] + "-status.json")
        if not status_path.exists():
            continue
        status = read(status_path)
        if status.get("status") not in ("pass", "fail", "timeout"):
            continue
        arguments = job.get("arguments", [])
        if "-OutputRoot" not in arguments:
            continue
        output = arguments[arguments.index("-OutputRoot") + 1].replace("\\", "/")
        prefix = "c:/ZipshotLab/output/".lower()
        if not output.lower().startswith(prefix):
            raise RuntimeError("Unexpected recording output path")
        folder_name = output[len(prefix):]
        if "/" in folder_name or ".." in folder_name or not folder_name.startswith("recording"):
            raise RuntimeError("Unexpected recording dataset name")
        folder = stage / "output" / folder_name
        copy(job_path, Path("metadata") / (job["id"] + "-job.json"))
        copy(status_path, Path("metadata") / status_path.name)
        for suffix in ("stdout.txt", "stderr.txt"):
            path = stage / "output" / (job["id"] + "-" + suffix)
            if path.exists():
                copy(path, Path("metadata") / path.name)
        if folder.is_dir():
            for source in sorted(folder.rglob("*")):
                if not source.is_file():
                    continue
                relative = source.relative_to(folder)
                if source.name.lower() == "zipshot.exe" and source.parent.name == "app":
                    excluded.append({"source_relative_path": source.relative_to(stage).as_posix(), "sha256": digest(source),
                                     "reason": "Staged copy of the already published release executable; INI and media remain archived."})
                    continue
                copy(source, Path("datasets") / folder_name / relative)
        jobs.append({"id": job["id"], "status": status["status"], "dataset": "datasets/" + folder_name})
    manifest = {"schema_version": 1, "utc": datetime.now(timezone.utc).isoformat(),
                "scope": "Finished explicitly whitelisted recording jobs only. No host session, VM configuration, RDP credentials, browser profiles or user desktop content.",
                "storage_evidence": {"verified_by": "root agent live findmnt/df before this copy", "device": "/dev/sda1",
                                     "uuid": "b5536ef2-245d-4541-bbc6-80ab6d60fe3d", "reported_free_bytes": 50945736704},
                "source_copies_retained": True, "jobs": jobs, "file_count": len(entries),
                "total_bytes": sum(item["bytes"] for item in entries), "files": entries, "excluded_staging_binaries": excluded}
    temporary = destination / "recording_archive_manifest.tmp"
    temporary.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    temporary.replace(destination / "recording_archive_manifest.json")
    print(json.dumps({"files": len(entries), "bytes": manifest["total_bytes"], "jobs": len(jobs),
                      "manifest": str(destination / "recording_archive_manifest.json")}, indent=2))


if __name__ == "__main__":
    main()
