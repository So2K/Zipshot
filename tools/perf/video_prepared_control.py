"""Full synthetic pipeline timing with a valid MP4 and an ISO free box, no GUI/capture."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import statistics
import struct
import argparse
import time

spec=importlib.util.spec_from_file_location('video_profile',Path(__file__).with_name('video_profile.py'))
profile=importlib.util.module_from_spec(spec);spec.loader.exec_module(profile)
parser=argparse.ArgumentParser();parser.add_argument('--actual',action='store_true');args=parser.parse_args()
root=profile.ROOT;folder=root/('build/perf/video/prepared-actual' if args.actual else 'build/perf/video/prepared-control')
if folder.exists():folder=folder.with_name(folder.name+'-'+str(time.time_ns()))
folder.mkdir(exist_ok=False)
ffmpeg=Path(shutil.which('ffmpeg'));ffprobe=Path(shutil.which('ffprobe'));source=folder/'fixture.mp4'
profile.execute([ffmpeg,'-v','error','-y','-f','lavfi','-i','testsrc2=size=640x360:rate=30:duration=4','-an','-c:v','libx264','-preset','ultrafast','-crf','0','-threads','4',source])
with source.open('ab') as stream:
    size=128*1024*1024
    stream.write(struct.pack('>I4s',size,b'free'))
    for _ in range((size-8)//(1<<20)):stream.write(bytes(1<<20))
    stream.write(bytes((size-8)%(1<<20)))
before=profile.sha(source)
_,probe=profile.execute([ffprobe,'-v','error','-count_frames','-select_streams','v:0','-show_entries','stream=width,height,nb_read_frames,duration,r_frame_rate','-of','json',source])
executable='video_prepared_actual.exe' if args.actual else 'video_prepared.exe'
metric,data=profile.execute([root/'build/perf/video'/executable,folder,ffmpeg,source],timeout=60)
result=json.loads(data);result.update({'schema':1,'source_base':'1fdf0a7','artifact_root':str(folder.relative_to(root)),'source_bytes':source.stat().st_size,'source_sha256':before,'input_probe':json.loads(probe),'process_metric':metric,'affinity_logical_processors':profile.CORES,'actual_production_api':args.actual,'production_source_sha256':{p:profile.sha(root/p) for p in ('src/cursor_video.cpp','src/cursor_video.h','src/recorder.cpp')},'decoded_output_sha256':[]})
if args.actual:result['scope']='Actual production old-compatible APIs vs PreparedSource API, including SaveTrack, baseline LoadTrack, Render and output validation. A single full SHA is protected by retained deny-write HANDLE; no benchmark shim.'
for rep in range(8):
    target=folder/f'run-{rep}-focus.mp4'
    _,data=profile.execute([ffmpeg,'-v','error','-threads','4','-i',target,'-map','0:v:0','-fps_mode','passthrough','-f','framemd5','-'])
    (folder/f'run-{rep}.framemd5').write_bytes(data);result['decoded_output_sha256'].append(hashlib.sha256(data).hexdigest())
result['all_decoded_outputs_exact']=len(set(result['decoded_output_sha256']))==1
result['source_unchanged']=profile.sha(source)==before
result['summary']={}
for prepared,name in ((False,'baseline'),(True,'prepared')):
    rows=[r for r in result['runs'] if r['prepared']==prepared]
    result['summary'][name]={key:{'median':statistics.median(r[key] for r in rows),'p95_nearest_rank':profile.percent([r[key] for r in rows],.95)} for key in ('save_ms','load_ms','render_ms','total_ms')}
result['median_reduction_fraction']=1-result['summary']['prepared']['total_ms']['median']/result['summary']['baseline']['total_ms']['median']
(folder.parent/('video-prepared-actual.json' if args.actual else 'video-prepared-control.json')).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({key:result[key] for key in ('source_bytes','input_probe','summary','median_reduction_fraction','all_decoded_outputs_exact','source_unchanged')},indent=2))
