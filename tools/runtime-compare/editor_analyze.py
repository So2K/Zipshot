"""Offline video-sampled red-stroke evidence. Never assumes declared video FPS is UI FPS."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import numpy as np
from PIL import Image


def stats(values):
    values = sorted(values)
    if not values:
        return None
    return dict(n=len(values), median=statistics.median(values), p95=values[math.ceil(len(values)*.95)-1], maximum=values[-1], minimum=values[0])


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('run',type=Path)
    ap.add_argument('--ffmpeg',default='ffmpeg')
    args=ap.parse_args()
    record=json.loads((args.run/'editor-interaction-result.json').read_text(encoding='utf-8-sig'))
    video=json.loads((args.run/'video-frames.json').read_text(encoding='utf-8-sig'))
    stream=next(s for s in video['streams'] if 'width' in s)
    width,height=stream['width'],stream['height']
    times=[float(f.get('best_effort_timestamp_time',f.get('pts_time'))) for f in video['frames']]
    anchor=record['clock_before_input'];frequency=record['qpc_frequency']
    anchor_qpc=(anchor['qpc_before']+anchor['qpc_after'])/2
    anchor_utc=anchor['filetime_utc_100ns']/10_000_000-11644473600
    utc=lambda q:anchor_utc+(q-anchor_qpc)/frequency
    clip_origin=record['video']['capture_crop'][:2]
    phases=[p for p in record['phases'] if p.get('kind') in ('arrow','freehand')]
    analyses=[]
    for phase in phases:
        events=[e for e in record['injected_events'] if e.get('kind')=='move' and e.get('phase')==phase['name'] and e.get('index',-1)>=1]
        event_map={e['index']:e for e in events}
        points=phase['points']
        xs=[p['x'] for p in points];ys=[p['y'] for p in points]
        x0=max(0,math.floor(min(xs)-clip_origin[0]-18));x1=min(width,math.ceil(max(xs)-clip_origin[0]+19))
        y0=max(0,math.floor(min(ys)-clip_origin[1]-18));y1=min(height,math.ceil(max(ys)-clip_origin[1]+19))
        analyses.append(dict(name=phase['name'],kind=phase['kind'],events=events,event_map=event_map,points=points,
                             roi=[x0,y0,x1,y1],start=utc(phase['qpc_start']),end=utc(phase['qpc_end']),observations=[]))
    command=[args.ffmpeg,'-v','error','-i',str(args.run/'actual-ui.mkv'),'-map','0:v:0','-fps_mode','passthrough','-f','rawvideo','-pix_fmt','rgb24','pipe:1']
    process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    baseline={};last_image=None;decoded=0
    for index,timestamp in enumerate(times):
        data=process.stdout.read(width*height*3)
        if len(data)!=width*height*3:
            raise RuntimeError('Video frame bytes/count disagree with ffprobe')
        frame=np.frombuffer(data,np.uint8).reshape(height,width,3)
        if index==0:Image.fromarray(frame).save(args.run/'video-first-frame.png')
        last_image=frame.copy();decoded+=1
        for a in analyses:
            x0,y0,x1,y1=a['roi'];region=frame[y0:y1,x0:x1]
            mask=(region[:,:,0]>170)&(region[:,:,1]<100)&(region[:,:,2]<100)
            if index==0:baseline[a['name']]=mask.copy()
            mask &= ~baseline[a['name']]
            if timestamp<a['start'] or timestamp>a['end']:
                continue
            count=int(mask.sum())
            observation=dict(frame=index,pts=timestamp,red_pixels=count)
            if count>=3:
                tip_x=float(np.where(mask)[1].max()+x0+clip_origin[0])
                candidate=min(range(1,len(a['points'])),key=lambda j:abs(a['points'][j]['x']-tip_x))
                event=a['event_map'].get(candidate)
                observation.update(tip_x_global=tip_x,nearest_trajectory_index=candidate,x_localization_error_px=tip_x-a['points'][candidate]['x'],red_mask_hash=hashlib.sha256(np.packbits(mask).tobytes()).hexdigest())
                if event and record['video']['epoch_pts']:
                    observation['input_to_sample_ms']=(timestamp-utc(event['qpc_before']))*1000
            a['observations'].append(observation)
    process.stdout.close();errors=process.stderr.read().decode(errors='replace');exit_code=process.wait()
    if exit_code:raise RuntimeError(errors)
    if last_image is not None:Image.fromarray(last_image).save(args.run/'video-last-frame.png')
    summaries=[]
    intervals=[(b-a)*1000 for a,b in zip(times,times[1:])]
    for a in analyses:
        visible=[o for o in a['observations'] if o.get('red_pixels',0)>=3]
        progress=[];previous_index=0
        for o in visible:
            j=o['nearest_trajectory_index']
            if j>previous_index and abs(o['x_localization_error_px'])<=8:
                progress.append(o);previous_index=j
        latency=[o['input_to_sample_ms'] for o in progress if 'input_to_sample_ms' in o]
        moves=a['events'];move_intervals=[(b['qpc_before']-x['qpc_before'])*1000/frequency for x,b in zip(moves,moves[1:])]
        progress_intervals=[(b['pts']-x['pts'])*1000 for x,b in zip(progress,progress[1:])]
        first=None
        if visible and moves and record['video']['epoch_pts']:
            first=(visible[0]['pts']-utc(moves[0]['qpc_before']))*1000
        summaries.append(dict(phase=a['name'],kind=a['kind'],injected_move_events=len(moves)+1,
                              injection_intervals_ms=stats(move_intervals),observed_progress_samples=len(progress),
                              first_move_to_first_red_sample_ms=first,input_to_sample_ms=stats(latency),
                              video_sampled_progress_intervals_ms=stats(progress_intervals),last_observed_trajectory_index=previous_index,
                              synchronization_warning=bool(latency and min(latency)<-10),observations=a['observations']))
    report=dict(schema_version=1,engine=record['engine'],case=record['case'],scope='Video-sampled annotation progress, not physical display latency or a claim of UI render FPS.',
                timing='Epoch GDI capture PTS aligned with paired precise FILETIME/QPC anchor. Each sample includes observer acquisition and sampling delay.',
                uncertainty='At least one observed capture interval plus pixel localization/injection jitter. Negative or ambiguous samples are retained and flagged, not silently clamped.',
                requested_capture_fps=record['video']['capture_requested_fps'],observed_pts_fps=record['video']['observed_pts_fps'],
                capture_interval_ms=stats(intervals),decoded_frames=decoded,clock_anchor_bracket_ms=(anchor['qpc_after']-anchor['qpc_before'])*1000/frequency,
                epoch_pts=record['video']['epoch_pts'],red_predicate='R>170,G<100,B<100, baseline subtraction, trajectory ROI excludes app toolbar',phases=summaries,
                video_sha256=hashlib.sha256((args.run/'actual-ui.mkv').read_bytes()).hexdigest())
    report['cursor_occlusion_warning']=record['video'].get('cursor_drawn_by_observer',True)
    if report['cursor_occlusion_warning']:
        report['metric_status']='Provisional only. Observer cursor may hide the red tip and overestimate point latency. Use cursor-free reruns for comparison.'
    else:
        report['metric_status']='Video-sampled estimates, subject to the stated observer and localization uncertainty.'
    (args.run/'editor-video-analysis.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='phases'}|{'phases':[{k:v for k,v in p.items() if k!='observations'} for p in summaries]},indent=2))


if __name__=='__main__':main()
