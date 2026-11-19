"""Separate dynamic-filter pixels from encoder nondeterminism; synthetic source only."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

spec=importlib.util.spec_from_file_location('video_profile',Path(__file__).with_name('video_profile.py'))
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
folder=profile.ROOT/'build/perf/video/long-control'
ffmpeg=Path(shutil.which('ffmpeg'))
result={'scope':'Pre-encoder framemd5 control, four logical cores/below-normal, repeated identical dynamic filters, original synthetic 60s/3600-frame input.','runs':{}}
for name,commands in (('baseline','all.cmd'),('delta','delta.cmd')):
    result['runs'][name]=[]
    for i in range(2):
        graph=f'sendcmd=f={commands},crop@camera=320:180:0:0,scale=320:180:eval=frame'
        metric,data=profile.execute([ffmpeg,'-v','error','-threads','4','-i',folder/'source.mp4','-filter_threads','1','-vf',graph,'-an','-pix_fmt','yuv420p','-fps_mode','passthrough','-f','framemd5','-'],cwd=folder)
        path=folder/f'filtered-{name}-{i}.framemd5';path.write_bytes(data)
        result['runs'][name].append({'metric':metric,'framemd5_sha256':hashlib.sha256(data).hexdigest()})
hashes=[run['framemd5_sha256'] for runs in result['runs'].values() for run in runs]
result['all_filtered_pixels_exact']=len(set(hashes))==1
(folder.parent/'video-exact-control.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'hashes':hashes,'all_filtered_pixels_exact':result['all_filtered_pixels_exact']}))
