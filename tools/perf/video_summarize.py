"""Publish compact numeric evidence without local absolute paths or generated media."""
import json
from pathlib import Path

root=Path(__file__).resolve().parents[2];folder=root/'build/perf/video'
read=lambda name:json.loads((folder/name).read_text())
short=read('video-profile.json');long=read('video-long-control.json');exact=read('video-exact-control.json');pipeline=read('video-pipeline.json');shim=read('video-prepared-control.json');actual=read('video-prepared-actual.json')
report={'schema':1,'base':'1fdf0a7','scope':'Owned synthetic inputs only, four-logical-processor affinity budget, below-normal priority. No capture or universal product comparison.','ffmpeg_sha256':short['ffmpeg_sha256'],'affinity_logical_processors':short['affinity_logical_processors'],'short_controls':[]}
for item in short['resolutions']:
    report['short_controls'].append({'dimensions':item['dimensions'],'input_sha256':item['input_sha256'],'command_counts':item['commands'],'renders':{name:{'n':len(value['runs']),'summary':value['summary'],'all_decoded_framemd5_exact':value['all_decoded_framemd5_exact'],'framemd5_sha256':value['framemd5_sha256']} for name,value in item['renders'].items()},'metadata':{name:{'n':len(value['runs']),'summary':value['summary'],'selected_metadata_byte_exact':value['selected_metadata_byte_exact']} for name,value in item['metadata'].items()}})
report['long_control']={'scope':long['scope'],'commands':long['commands'],'summary':long['summary'],'framemd5_sha256':long['framemd5_sha256'],'all_frames_exact':long['all_frames_exact'],'all_filtered_pixels_exact':exact['all_filtered_pixels_exact'],'filtered_framemd5_sha256':{name:[row['framemd5_sha256'] for row in rows] for name,rows in exact['runs'].items()}}
report['initial_hash_control']=pipeline['measurements']
report['retained_handle_shim']={key:shim[key] for key in ('source_bytes','source_sha256','summary','median_reduction_fraction','all_decoded_outputs_exact','source_unchanged')}
report['actual_api']={key:actual[key] for key in ('source_bytes','source_sha256','input_probe','summary','runs','median_reduction_fraction','all_decoded_outputs_exact','decoded_output_sha256','source_unchanged','production_source_sha256')}
report['actual_api']['method']='Actual compatible APIs vs PreparedSource APIs, four counterbalanced repetitions each. Full-pipeline wall time; helper counters exclude child process resources.'
out=root/'docs/perf/video_results.json';out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(report,indent=2)+'\n');print(out)
