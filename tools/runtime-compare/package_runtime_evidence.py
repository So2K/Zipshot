"""Package original accepted lab evidence without vendor binaries or host/session data."""
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAGE = Path("D:/ZipshotRuntimeCompare-20261118/session02")
RAW = STAGE / "output"
RECORDING = Path("J:/TerraMaster/SSD1/Files/Zipshot/runtime-compare-20261118/recording/session02")
OUT = ROOT / "build/runtime-release"
ZIP = OUT / "Zipshot-runtime-evidence-20261118.zip"
REPORT = OUT / "evidence-build.json"
ALLOWED = {".md", ".txt", ".log", ".json", ".jsonl", ".csv", ".tsv", ".ini", ".cfg", ".xml", ".svg", ".png", ".bmp", ".mp4", ".mkv", ".gif", ".py", ".ps1", ".psm1", ".cpp", ".c", ".h", ".hpp", ".cs", ".bat", ".yml", ".yaml"}
TEXT = ALLOWED - {".png", ".bmp", ".mp4", ".mkv", ".gif"}
BAD_DIRS = {"app", "runtime", "vendor", "sharex-runtime", "__pycache__", ".git", "node_modules"}
BAD_NAMES = {"host-session.json", "guest-ready.json", "capture_state.md", "capture-coordination.md", "film-manifest.json", "runtime-ru.srt", "zipshot-runtime-comparison.mp4"}
SECRET = {
    "private_key": rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----",
    "github_token": rb"\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{30,})",
    "api_token": rb"\b(?:sk-(?:proj-|ant-)?[A-Za-z0-9_-]{30,}|AIza[A-Za-z0-9_-]{35}|AKIA[A-Z0-9]{16}|xox[baprs]-[A-Za-z0-9-]{20,})",
    "jwt": rb"\beyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}",
    "basic_auth_url": rb"https?://[^/\s:@]{1,64}:[^/\s@]{3,100}@",
}
ASSIGN = re.compile(rb"(?im)(?:[\x22\x27]?(?:password|passwd|access_token|refresh_token|api[_-]?key|client_secret)[\x22\x27]?)\s*[:=]\s*[\x22\x27]([^\x22\x27\r\n]{8,})[\x22\x27]")
EMAIL = re.compile(rb"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


class Build:
    def __init__(self):
        self.files = {}; self.excluded = []; self.verified = []; self.findings = []
        self.silent_containers_checked = 0
        self.counts = {"capture_baseline_blocks": 0, "capture_v02_recordings": 0, "editor_standalone_runs": 0, "editor_region_runs": 0, "recording_manifest_files_checked": 0}

    def add(self, source, member, origin, raw=False, expected=None):
        source = Path(source); member = member.replace("\\", "/")
        if member.startswith("/") or ".." in Path(member).parts:
            raise RuntimeError("Unsafe archive member")
        if not source.is_file():
            raise RuntimeError("Missing required source: " + origin)
        suffix = source.suffix.lower()
        parts = {p.lower() for p in Path(member).parts}
        if source.name.lower() in BAD_NAMES or suffix not in ALLOWED or (raw and parts & BAD_DIRS):
            self.excluded.append({"source": origin, "reason": "explicit content/directory whitelist"}); return
        if any(word in source.name.lower() for word in ("coordination", "handoff")):
            self.excluded.append({"source": origin, "reason": "private working coordination"}); return
        digest = sha(source)
        if expected and digest != expected.lower():
            raise RuntimeError("Original manifest hash mismatch: " + origin)
        entry = {"source": source, "member": member, "origin": origin, "sha256": digest, "bytes": source.stat().st_size}
        if member in self.files and self.files[member]["sha256"] != digest:
            raise RuntimeError("Conflicting member: " + member)
        self.files[member] = entry

    def check(self, source, expected, description):
        if sha(source) != expected.lower():
            raise RuntimeError("Expected original source hash failed: " + description)
        self.verified.append(description)

    def tree(self, folder, prefix, origin):
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                relative = path.relative_to(folder)
                self.add(path, prefix + "/" + relative.as_posix(), origin + "/" + relative.as_posix(), raw=True)

    def run_folder(self, relative, job=None):
        folder = RAW / relative
        if not folder.resolve().is_relative_to(RAW.resolve()):
            raise RuntimeError("Original run escaped controlled output")
        self.tree(folder, "original/session02/output/" + relative, "session02/output/" + relative)
        if job:
            for suffix in ("status.json", "stdout.txt", "stderr.txt"):
                path = RAW / (job + "-" + suffix)
                if path.exists():
                    self.add(path, "original/session02/output/" + path.name, "session02/output/" + path.name, raw=True)
            path = STAGE / "input/jobs" / (job + ".json")
            if path.exists():
                self.add(path, "original/session02/jobs/" + path.name, "session02/input/jobs/" + path.name, raw=True)

    def collect_raw(self):
        docs = ROOT / "docs/runtime-comparison"
        baseline = read(docs / "capture_baseline.json")
        for block in baseline["source_manifest"]:
            name = block["job"]; self.run_folder(name, name)
            self.check(RAW/name/"capture-results.json", block["raw_json_sha256"], name+"/capture-results.json")
            self.check(RAW/name/"metadata.json", block["metadata_sha256"], name+"/metadata.json")
            self.counts["capture_baseline_blocks"] += 1
        for clip in read(docs / "capture_visual_manifest.json")["clips"]:
            name = clip["case"]; self.run_folder(name, name)
            for path, field in ((RAW/clip["source_video"], "source_video_sha256"), (RAW/name/"capture-results.json", "source_results_sha256"), (RAW/name/"metadata.json", "source_metadata_sha256")):
                self.check(path, clip[field], str(path.relative_to(RAW)))
            if clip.get("actual_proof_file"):
                self.check(RAW/clip["actual_proof_file"], clip["actual_proof_sha256"], clip["actual_proof_file"])
            self.counts["capture_v02_recordings"] += 1
        for manifest, count in (("editor_ui_runs.json", "editor_standalone_runs"), ("editor_region_runs.json", "editor_region_runs")):
            for run in read(docs/manifest)["runs"]:
                name = run["source_folder"]; self.run_folder(name)
                self.check(RAW/name/run["source_video"], run["video"]["sha256"], name+"/"+run["source_video"])
                if run.get("export_sha256"):
                    if not any(sha(p)==run["export_sha256"].lower() for p in (RAW/name).rglob("*.png")):
                        raise RuntimeError("Original editor export absent: " + name)
                for saved in run.get("saved_outputs", []):
                    self.check(RAW/name/saved["file"], saved["sha256"], name+"/"+saved["file"])
                self.counts[count] += 1
        config = STAGE / "input/zipshot/zipshot.ini"
        if config.exists():
            self.add(config, "original/session02/config-snapshots/zipshot-initial.ini", "session02/input/zipshot/zipshot.ini", raw=True)
        archive = read(RECORDING / "recording_archive_manifest.json")
        self.add(RECORDING/"recording_archive_manifest.json", "original/recording-archive-manifest.json", "recording-archive/session02/recording_archive_manifest.json", raw=True)
        for item in archive["files"]:
            path = RECORDING/item["relative_path"]
            self.check(path, item["sha256"], "recording-archive/session02/"+item["relative_path"])
            self.counts["recording_manifest_files_checked"] += 1
            relative = item["relative_path"]
            if relative.startswith("datasets/"):
                relative = relative[len("datasets/"):]
                if "app" in Path(relative).parts and path.suffix.lower()==".ini":
                    relative = relative.replace("/app/", "/config-snapshot/")
                self.add(path, "original/session02/output/"+relative, "recording-archive/session02/"+item["relative_path"], raw=True, expected=item["sha256"])
            elif relative.startswith("metadata/"):
                self.add(path, "original/session02/recording-job-metadata/"+Path(relative).name, "recording-archive/session02/"+item["relative_path"], raw=True, expected=item["sha256"])

    def collect_repo(self):
        for directory in ("docs/runtime-comparison", "tools/runtime-compare", "media/runtime"):
            folder = ROOT / directory
            for path in sorted(folder.rglob("*")):
                if path.is_file() and "__pycache__" not in path.parts:
                    relative = path.relative_to(ROOT).as_posix()
                    self.add(path, "repository/"+relative, "repo/"+relative)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            # LICENSE has no suffix but is explicitly required.
            path = ROOT/name
            if name=="LICENSE":
                self.files["repository/LICENSE"]={"source":path,"member":"repository/LICENSE","origin":"repo/LICENSE","sha256":sha(path),"bytes":path.stat().st_size}
            else:self.add(path,"repository/"+name,"repo/"+name)

    def scan(self):
        for entry in self.files.values():
            path=entry["source"]
            if path.suffix.lower() not in TEXT and path.name!="LICENSE":continue
            data=path.read_bytes()
            if data.startswith((b"\xff\xfe",b"\xfe\xff")):
                data=data.decode("utf-16").encode("utf-8")
            for category,pattern in SECRET.items():
                match=re.search(pattern,data)
                if match:self.findings.append({"path":entry["member"],"line":data[:match.start()].count(b"\n")+1,"category":category})
            for match in ASSIGN.finditer(data):
                value=match.group(1).strip()
                if value.startswith((b"$",b"<",b"[",b"\\")) or value.lower() in (b"redacted",b"undefined",b"changeme",b"placeholder"):continue
                self.findings.append({"path":entry["member"],"line":data[:match.start()].count(b"\n")+1,"category":"literal_credential_assignment"})
            for match in EMAIL.finditer(data):
                domain=match.group().split(b"@",1)[1].lower()
                if domain not in (b"gnu.org",b"fsf.org"):
                    self.findings.append({"path":entry["member"],"line":data[:match.start()].count(b"\n")+1,"category":"non_license_email_address"})

    def write(self):
        self.scan()
        if self.findings:
            raise RuntimeError("Private/secret text scan failed; see locations in evidence-build.json")
        checked = set()
        for entry in self.files.values():
            if entry["source"].suffix.lower() not in (".mp4", ".mkv") or entry["sha256"] in checked:
                continue
            result = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type", "-of", "json", str(entry["source"])], capture_output=True, timeout=30)
            if result.returncode:
                raise RuntimeError("Cannot verify silent video container: " + entry["member"])
            streams = json.loads(result.stdout).get("streams", [])
            if any(stream.get("codec_type") == "audio" for stream in streams):
                raise RuntimeError("Forbidden audio stream in public media: " + entry["member"])
            checked.add(entry["sha256"])
            self.silent_containers_checked += 1
        index={"schema_version":1,"scope":"Original accepted isolated-lab evidence and frozen source/report/microclip snapshots. No vendor binaries or host session data.","counts":self.counts,"files":[{"path":e["member"],"source":e["origin"],"bytes":e["bytes"],"sha256":e["sha256"]} for e in self.files.values()],"excluded":self.excluded}
        readme="""# Zipshot runtime evidence, 2026-11-18

Repository and report https://github.com/So2K/Zipshot and https://github.com/So2K/Zipshot/blob/main/docs/runtime-comparison/README.md

repository/ contains frozen reports, source tools and derived runtime microclips/charts. The silent montage and its manifest are separate release assets and are not in this archive. Audio files and video audio streams are forbidden in public media.
original/session02/output/ preserves accepted original lab paths and bytes for all 12 capture baseline blocks, the four cap-v02 recordings, 16 standalone editor runs, four region-overlay runs and recording datasets. Original resource/process snapshots, JSON, CSV, logs, PNG/BMP proofs and source MKV/MP4/GIF files are included.
original/session02/jobs/ and recording-job-metadata/ contain task-only run arguments and outcomes. config-snapshots/ and config-snapshot/ preserve task INI data outside excluded app directories. Effective per-run settings are also recorded in raw metadata and source wrappers.
original/recording-archive-manifest.json is the existing verified recording archive index. EVIDENCE-SOURCES.json maps every included ZIP path to its original logical source path, byte count and SHA-256. Source D-drive and recording J-drive copies remain intact.
App/runtime/vendor directories and executables, DLLs, PDBs, VM/RDP/host-session files, credentials and browser profiles are excluded. All source content is governed by repository/LICENSE and repository/THIRD_PARTY_NOTICES.md.
"""
        temporary=ZIP.with_suffix(".zip.partial")
        with zipfile.ZipFile(temporary,"w",compression=zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as archive:
            for entry in self.files.values():archive.write(entry["source"],entry["member"])
            archive.writestr("EVIDENCE-SOURCES.json",json.dumps(index,indent=2)+"\n")
            archive.writestr("README.md",readme)
        if temporary.stat().st_size>500*1024*1024:raise RuntimeError("Compressed evidence exceeds 500MiB target")
        with zipfile.ZipFile(temporary) as archive:
            expected={entry["member"]:entry for entry in self.files.values()}
            for name,entry in expected.items():
                h=hashlib.sha256()
                with archive.open(name) as handle:
                    for block in iter(lambda:handle.read(1024*1024),b""):h.update(block)
                if h.hexdigest()!=entry["sha256"]:raise RuntimeError("Full ZIP hash verification failed: "+name)
            if archive.testzip() is not None:raise RuntimeError("ZIP CRC verification failed")
        temporary.replace(ZIP)
        return {"status":"complete","zip_file":ZIP.name,"bytes":ZIP.stat().st_size,"sha256":sha(ZIP),"file_count":len(self.files)+2,"source_file_count":len(self.files),"uncompressed_source_bytes":sum(e["bytes"] for e in self.files.values()),"counts":self.counts,"original_manifest_hash_checks":len(self.verified),"full_zip_decompression_sha256_verified":True,"zip_crc_verified":True,"text_scan":"passed","audio_policy":"no audio files or audio streams","silent_video_containers_checked":self.silent_containers_checked,"private_findings":[],"excluded":self.excluded}


def main():
    OUT.mkdir(parents=True,exist_ok=True);builder=Build()
    try:
        builder.collect_raw();builder.collect_repo();report=builder.write()
    except Exception as error:
        report={"status":"blocked","error":str(error),"private_findings":builder.findings,"collected_files":len(builder.files)}
        REPORT.write_text(json.dumps(report,indent=2)+"\n")
        print(json.dumps(report,indent=2));return 1
    report["built_utc"]=datetime.now(timezone.utc).isoformat()
    REPORT.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({k:report[k] for k in ("status","file_count","bytes","sha256","counts")},indent=2));return 0


if __name__=="__main__":sys.exit(main())
