"""Render actual recording excerpts and CPU/RAM evidence. No desktop capture or VM control."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "media/runtime"
DOC = ROOT / "docs/runtime-comparison"
BG, TEXT, MUTED, BLUE, GREY = "#13161d", "#f4f6fa", "#acb5c6", "#3a8dff", "#a8adb7"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def run(args):
    result = subprocess.run(args, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace")[-3000:])
    return result.stdout


def probe(path):
    return json.loads(run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=width,height,avg_frame_rate,nb_read_frames:format=duration,size", "-of", "json", str(path)]))


def source(stage, folder, case_name):
    dataset = stage / "output" / folder
    case = next(c for c in load(dataset / "recording_guest_results.json")["cases"] if c["name"] == case_name)
    item = next(item for item in case["files"] if item["relative_path"].lower().endswith(".mp4"))
    path = dataset / item["relative_path"].replace("\\", "/")
    if sha(path) != item["sha256"]:
        raise RuntimeError("Source hash mismatch")
    return path, case, item, "session02/datasets/" + folder + "/" + item["relative_path"].replace("\\", "/")


def ticks(path):
    raw = run(["ffmpeg", "-v", "error", "-threads", "1", "-i", str(path), "-filter_threads", "1", "-vf", "crop=320:16:32:32,scale=320:1:flags=area,format=gray", "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "gray", "-"])
    sequence = []
    for start in range(0, len(raw)-319, 320):
        values = [raw[start+i*20+8] for i in range(16)]
        if any(50 < x < 205 for x in values):
            raise RuntimeError("Ambiguous source tick")
        sequence.append(sum((x >= 128) << i for i, x in enumerate(values)))
    frame_data = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames", "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(path)]))
    times = [float(frame["best_effort_timestamp_time"]) for frame in frame_data["frames"]]
    if len(times) != len(sequence):
        raise RuntimeError("Frame/tick count mismatch")
    return sequence, times


def background(path, left, right):
    image = Image.new("RGB", (1920, 1080), BG)
    draw = ImageDraw.Draw(image)
    bounds = []
    def text(x, y, value, size=24, color=TEXT, bold=False):
        face = ImageFont.truetype("C:/Windows/Fonts/Ubuntu-"+("Bold" if bold else "Regular")+".ttf", size)
        box = draw.textbbox((x,y), value, font=face, anchor="lt")
        if box[2] > 1880 or box[3] > 1050:
            raise RuntimeError("Caption outside canvas: " + value)
        draw.text((x,y), value, font=face, fill=color, anchor="lt")
        bounds.append({"text":value,"bounds":list(box)})
    text(56, 52, "Recording output, side by side", 46, bold=True)
    text(56, 116, "Real saved files  /  1920 × 1080  /  30 fps requested  /  audio and cursor off", 25, MUTED)
    draw.line((56,163,170,163), fill=BLUE, width=5)
    for x, name, profile, info, color in ((56,"Zipshot 1.0.1","Factory fallback · x264 ultrafast · CRF 23",left,BLUE),(980,"ShareX 22.4.0","Native · software allowed · 10,000 kbps configured",right,GREY)):
        draw.rounded_rectangle((x,194,x+884,840), radius=16, fill="#1c2029", outline="#343c49", width=2)
        text(x+26,215,name,32,color,True)
        text(x+26,264,profile,23,MUTED)
        text(x+26,793,info,22,MUTED)
    text(56,885,"Same recorded fixture tick at the start. Playback 1×. No speed change.",28)
    text(56,932,"Different encoding and quality controls. Windows Sandbox remote display and virtual GPU.",24,MUTED)
    text(56,976,"This compares saved output. It is not a capture-start latency race or a normal-desktop FPS guarantee.",22,MUTED)
    image.save(path)
    return bounds


def resource_plot(stage):
    os.environ.setdefault("MPLCONFIGDIR", "D:/ZipshotRuntimeCompare-20261118/matplotlib-cache")
    import numpy as np
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import pyplot as plt, font_manager
    for weight in ("Regular","Bold"):
        font_manager.fontManager.addfont("C:/Windows/Fonts/Ubuntu-"+weight+".ttf")
    plt.rcParams.update({"font.family":"Ubuntu","text.color":TEXT,"axes.labelcolor":MUTED,"xtick.color":MUTED,"ytick.color":MUTED,"axes.edgecolor":"#454c59","figure.facecolor":BG,"axes.facecolor":BG,"svg.fonttype":"none","font.size":11})
    summary = load(DOC / "recording_local_summary.json")
    trials = summary["trials"]
    factory = [t for t in trials if t["repeat"].startswith("factory")]
    controlled = [t for t in trials if t["case"] == "zipshot-video30" and not t["repeat"].startswith("factory")]
    native = [t for t in trials if t["case"].startswith("sharex22-video30")]
    fig, axes = plt.subplots(3,2,figsize=(16,13.4),constrained_layout=True)
    groups=[("Zipshot factory fallback · n=1",factory,BLUE,"o"),("Zipshot controlled veryfast · n=2",controlled,BLUE,"s"),("ShareX native software · n=3",native,GREY,"o")]
    for axis,metric,title in ((axes[0,0],"tree_cpu_seconds","MP4  /  whole-lifecycle CPU seconds"),(axes[0,1],"tree_peak_private_mib","MP4  /  concurrent tree private memory, MiB")):
        for y,(label,group,color,marker) in enumerate(groups):
            values=[t[metric] for t in group];median=float(np.median(values))
            axis.plot([min(values),max(values)],[y,y],color=color,linewidth=3)
            axis.scatter(values,[y]*len(values),color=color,s=52,marker=marker,zorder=4)
            axis.scatter([median],[y],color=color,edgecolor=TEXT,s=56,marker="D",zorder=5)
            axis.annotate(f"median {median:.1f}",(median,y),xytext=(8,14),textcoords="offset points",va="center",color=TEXT,fontsize=10)
            if metric=="tree_cpu_seconds" and len(values)>2 and max(values)>median+5:
                axis.annotate(f"max {max(values):.1f}",(max(values),y),xytext=(7,-15),textcoords="offset points",va="center",color=MUTED,fontsize=10)
        axis.set_yticks(range(3),[g[0] for g in groups]);axis.invert_yaxis();axis.set_xlim(0,47 if metric=="tree_cpu_seconds" else 1280);axis.margins(y=.3);axis.set_title(title,loc="left",fontweight="bold",pad=15)
        axis.grid(axis="x",alpha=.15);axis.spines[["top","right"]].set_visible(False)
    data_groups=[("Factory",["recording-factory-local-v30"],BLUE,"-"),("Controlled",["recording-local-zipshot-r1","recording-local-zipshot-r2"],BLUE,"--"),("Native",["recording-local-sharex22-v30-r1","recording-local-sharex22-v30-r2","recording-local-sharex22-v30-r3"],GREY,"-")]
    def curves(axis,row_index,mode):
        configurations=data_groups if mode=="MP4" else [("Zipshot GIF",["recording-local-zipshot-r1","recording-local-zipshot-r2"],BLUE,"-"),("ShareX GIF",["recording-local-sharex22-g15-r1","recording-local-sharex22-g15-r2"],GREY,"-")]
        for label,folders,color,style in configurations:
            for i,folder in enumerate(folders):
                root=stage/"output"/folder;cases=load(root/"recording_guest_results.json")["cases"]
                case=next(c for c in cases if ("gif" in c["name"]) == (mode=="GIF"))
                samples=load(root/case["name"]/"recording_samples.json")
                times=np.array([s["elapsed_seconds"] for s in samples]);cpu=[];memory=[]
                for sample in samples:
                    rows=sample["processes"];rows=[rows] if isinstance(rows,dict) else rows
                    cpu.append(sum(p["cpu_seconds"] for p in rows));memory.append(sum(p["private_bytes"] for p in rows if p["alive"])/1048576)
                if row_index==0:
                    cpu=np.array(cpu);begin=np.maximum(times-1,times[0]);dt=times-begin
                    values=np.divide((cpu-np.interp(begin,times,cpu))*100,dt,out=np.zeros_like(cpu),where=dt>0)
                else:values=memory
                axis.plot(times,values,color=color,linestyle=style,alpha=1 if label=="Factory" else .68,linewidth=2 if label=="Factory" else 1.3,label=label if i==0 else None)
        axis.set_ylim(bottom=0);axis.set_xlim(left=0);axis.grid(alpha=.15);axis.legend(frameon=False,labelcolor=TEXT,fontsize=9,loc="upper right");axis.spines[["top","right"]].set_visible(False);axis.set_xlabel("Seconds since product launch")
    curves(axes[1,0],0,"MP4");curves(axes[1,1],1,"MP4");curves(axes[2,0],0,"GIF");curves(axes[2,1],1,"GIF")
    for row,kind in ((1,"MP4"),(2,"GIF")):
        axes[row,0].set_title(kind+"  /  CPU, 1 s trailing window",loc="left",fontweight="bold");axes[row,0].set_ylabel("% of one logical core")
        axes[row,1].set_title(kind+"  /  alive process-tree private memory",loc="left",fontweight="bold");axes[row,1].set_ylabel("MiB")
    fig.suptitle("Recording profiles in one isolated Windows guest\n",fontsize=23,fontweight="bold",x=.5)
    fig.supxlabel("Different encoders, quality controls and encoded durations. No CPU winner claim. All local repetitions retained.\nPrivate memory includes children. CPU 100% = one logical core. GPU load not measured. Raw ~200 ms samples are archived.",fontsize=10,color=MUTED)
    for suffix in ("png","svg"):
        temporary=OUT/(".recording-resources.new."+suffix)
        fig.savefig(temporary,dpi=150,facecolor=BG)
        temporary.replace(OUT/("recording-resources."+suffix))
    plt.close(fig)


def main():
    p=argparse.ArgumentParser();p.add_argument("--stage",type=Path,default=Path("D:/ZipshotRuntimeCompare-20261118/session02"));p.add_argument("--charts-only",action="store_true");args=p.parse_args()
    OUT.mkdir(parents=True,exist_ok=True);temp=Path("D:/ZipshotRuntimeCompare-20261118/recording-media");temp.mkdir(exist_ok=True)
    if args.charts_only:
        resource_plot(args.stage)
        manifest=load(OUT/"recording-media-manifest.json")
        for name in ("recording-resources.png","recording-resources.svg"):
            manifest["assets"][name]={"sha256":sha(OUT/name),"bytes":(OUT/name).stat().st_size}
        manifest["provenance"]={"zipshot_release_exe_sha256":"0730b10f1f00295e32f603feb3c482fe2aa66da08b644bb5595592b0ef1ac783",
            "data_summary_sha256":sha(DOC/"recording_local_summary.json"),"generator_sha256":sha(Path(__file__)),
            "raw_archive_manifest_relative_path":"session02/recording_archive_manifest.json",
            "raw_archive_manifest_sha256":sha(Path("J:/TerraMaster/SSD1/Files/Zipshot/runtime-compare-20261118/recording/session02/recording_archive_manifest.json"))}
        (OUT/"recording-media-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
        print("Updated charts only; actual paired movie unchanged.")
        return
    z,zc,zm,zrel=source(args.stage,"recording-factory-local-v30","zipshot-video30")
    s,sc,sm,srel=source(args.stage,"recording-local-sharex22-v30-r1","sharex22-video30-native-software-allowed")
    zt,zp=ticks(z);st,sp=ticks(s);threshold=max(zt[0],st[0])+60
    common=sorted(set(zt)&set(st));target=next(t for t in common if t>=threshold)
    zi,si=zt.index(target),st.index(target)
    if zp[-1]-zp[zi]<10 or sp[-1]-sp[si]<10:raise RuntimeError("Not enough aligned source duration")
    zd=float(zm["ffprobe"]["format"]["duration"]);sd=float(sm["ffprobe"]["format"]["duration"])
    captions=background(temp/"background.png",f"File 30 fps · {len(set(zt))/zd:.1f} distinct fixture states/s",f"File 30 fps · {len(set(st))/sd:.1f} distinct fixture states/s")
    graph=f"[1:v]trim=start_frame={zi},setpts=PTS-STARTPTS,trim=duration=10,scale=832:468:flags=lanczos,setsar=1[z];[2:v]trim=start_frame={si},setpts=PTS-STARTPTS,trim=duration=10,scale=832:468:flags=lanczos,setsar=1[s];[0:v][z]overlay=82:308:shortest=1[b];[b][s]overlay=1006:308:shortest=1[v]"
    movie=OUT/"recording-profile.mp4"
    run(["ffmpeg","-v","error","-y","-loop","1","-framerate","30","-i",str(temp/"background.png"),"-threads","1","-i",str(z),"-threads","1","-i",str(s),"-filter_complex_threads","1","-filter_complex",graph,"-map","[v]","-an","-t","10","-r","30","-c:v","libx264","-preset","veryfast","-crf","18","-threads","4","-pix_fmt","yuv420p","-movflags","+faststart",str(movie)])
    palette=temp/"palette.png"
    run(["ffmpeg","-v","error","-y","-threads","1","-i",str(movie),"-vf","fps=12,scale=1280:-2:flags=lanczos,palettegen=stats_mode=full","-update","1",str(palette)])
    run(["ffmpeg","-v","error","-y","-threads","1","-i",str(movie),"-i",str(palette),"-filter_complex_threads","1","-filter_complex","[0:v]fps=12,scale=1280:-2:flags=lanczos[v];[v][1:v]paletteuse=dither=bayer:bayer_scale=3","-loop","0",str(OUT/"recording-profile.gif")])
    resource_plot(args.stage)
    inputs=[{"product":"Zipshot 1.0.1","archive_relative_path":zrel,"sha256":sha(z),"profile":"Factory fallback software preset ultrafast, CRF23, actual DDA capture","frames":len(zt),"duration_seconds":zd,"observed_encoded_fps":len(zt)/zd,"distinct_fixture_states_per_second":len(set(zt))/zd},{"product":"ShareX 22.4.0","archive_relative_path":srel,"sha256":sha(s),"profile":"Official native recorder, software allowed, configured10000kbps; predeclared first local repetition","frames":len(st),"duration_seconds":sd,"observed_encoded_fps":len(st)/sd,"distinct_fixture_states_per_second":len(set(st))/sd}]
    metadata={"schema_version":1,"scope":"Actual saved MP4 excerpts only. Original synthetic fixture in a clean Windows Sandbox; no host desktop content.","sources":inputs,"alignment":{"recorded_fixture_tick":target,"zipshot_start_frame":zi,"sharex_start_frame":si,"zipshot_original_pts":zp[zi],"sharex_original_pts":sp[si],"operation":"Trim to an identical recorded tick, reset timestamp origins only, preserve1xplayback; no interpolation or speed scaling"},"presentation":{"video_seconds":10,"video_fps":30,"source_scale":"1920x1080 to832x468 each, Lanczos, no color/contrast edits","comparison_encoding":"x264 veryfast CRF18 yuv420p","gif":"Derived preview of the comparison MP4, 1280x720 at12fps; not either application's original GIF export"},"limitations":["Different encoder quality controls and capture backends; not an equal-quality codec test.","Not a capture-start latency race.","30Hzfixture and remote display cannot establish60distinct visual updates/s.","Resources use every local repetition, including the40CPU-second/16drop native outlier.","GPU load was not measurable by owned processID counters."],"caption_bounds":captions}
    metadata["outputs"]={name:{"sha256":sha(OUT/name),"bytes":(OUT/name).stat().st_size,"probe":probe(OUT/name)} for name in ("recording-profile.mp4","recording-profile.gif")}
    (OUT/"recording-profile.json").write_text(json.dumps(metadata,indent=2)+"\n")
    manifest={"schema_version":1,"fonts":["Ubuntu Regular","Ubuntu Bold"],"palette":{"background":BG,"zipshot":BLUE,"sharex":GREY},"sources":inputs,"assets":{name:{"sha256":sha(OUT/name),"bytes":(OUT/name).stat().st_size} for name in ("recording-profile.mp4","recording-profile.gif","recording-profile.json","recording-resources.png","recording-resources.svg")}}
    (OUT/"recording-media-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    print(json.dumps({"outputs":list(manifest["assets"]),"alignment_tick":target,"mp4_probe":metadata["outputs"]["recording-profile.mp4"]["probe"]},indent=2))


if __name__=="__main__":main()
