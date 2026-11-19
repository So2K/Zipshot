"""Isolate longer sendcmd dispatch with identical synthetic pixels and fixed codec settings."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

spec=importlib.util.spec_from_file_location('video_profile',Path(__file__).with_name('video_profile.py'))
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
ROOT=profile.ROOT;folder=ROOT/'build/perf/video/long-control';folder.mkdir(exist_ok=True)
ffmpeg=Path(shutil.which('ffmpeg'))
plan=json.loads(subprocess.check_output([str(ROOT/'build/perf/video/video_commands.exe'),'320','180',str(folder),'60','60'],text=True))
source=folder/'source.mp4'
profile.execute([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=320x180:rate=60:duration=60','-an','-c:v','libx264','-preset','ultrafast','-crf','0','-threads','4',source],timeout=60)
runs={'baseline':[],'changed_scalars_only':[]};hashes={key:[] for key in runs}
for rep,name in enumerate(('baseline','changed_scalars_only','changed_scalars_only','baseline','baseline','changed_scalars_only','changed_scalars_only','baseline')):
    command='all.cmd' if name=='baseline' else 'delta.cmd';target=folder/f'{name}-{rep}.mp4'
    graph=f'sendcmd=f={command},crop@camera=320:180:0:0,scale=320:180:eval=frame'
    metric,_=profile.execute([ffmpeg,'-v','error','-y','-i',source,'-filter_threads','1','-vf',graph,'-an','-c:v','libx264','-preset','veryfast','-crf','18','-threads','4','-pix_fmt','yuv420p','-fps_mode','passthrough','-movflags','+faststart',target],cwd=folder,timeout=60)
    _,data=profile.execute([ffmpeg,'-v','error','-threads','4','-i',target,'-map','0:v:0','-fps_mode','passthrough','-f','framemd5','-'],timeout=60)
    runs[name].append(metric);hashes[name].append(hashlib.sha256(data).hexdigest())
    (folder/f'{name}-{rep}.framemd5').write_bytes(data)
report={'scope':'Synthetic 60s/3600-frame low-resolution command-dispatch control, not a general recording benchmark. Fixed CRF18/veryfast/4encoderthreads/1filterthread, four-logical-core affinity. Counterbalanced variant order.','commands':plan,'runs':runs,'summary':{key:profile.stats(value) for key,value in runs.items()},'framemd5_sha256':hashes,'all_frames_exact':len({h for value in hashes.values() for h in value})==1}
(ROOT/'build/perf/video/video-long-control.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'commands':plan,'summary':report['summary'],'all_frames_exact':report['all_frames_exact']},indent=2))
