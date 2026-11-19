"""Manifest, sender-based montage offsets and explicitly scoped UI summary CSV."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path,default=Path('D:/ZipshotRuntimeCompare-20261118/session02/output'))
    ap.add_argument('--docs',type=Path,default=Path(__file__).resolve().parents[2]/'docs/runtime-comparison')
    args=ap.parse_args();args.docs.mkdir(parents=True,exist_ok=True)
    records=[];rows=[];curves=[]
    for group in ['editor-pairs','editor-extras','editor-more']:
        for path in sorted((args.output/group).glob('*/editor-interaction-result.json')):
            record=json.loads(path.read_text(encoding='utf-8-sig'))
            if record.get('status')!='captured_pending_visual_and_frame_analysis':continue
            folder=path.parent;anchor=record['clock_before_input'];frequency=record['qpc_frequency']
            q=(anchor['qpc_before']+anchor['qpc_after'])/2;t=anchor['filetime_utc_100ns']/10_000_000-11644473600
            utc=lambda timestamp:t+(timestamp-q)/frequency
            start=record['video']['first_pts'];left,top=record['video']['capture_crop'][:2]
            phases=[dict(name=p['name'],kind=p.get('kind'),video_offset_s=utc(p['qpc_start'])-start,
                         duration_s=(p['qpc_end']-p['qpc_start'])/frequency,details=p) for p in record['phases']]
            overlays=[]
            for event in record['injected_events']:
                if event.get('kind')=='move':
                    overlays.append(dict(seconds=utc(event['qpc_before'])-start,x=event['x']-left,y=event['y']-top,
                                         phase=event['phase'],index=event.get('index'),label='Recorded input position'))
            export=np.asarray(Image.open(folder/'editor-snapshot.png').convert('RGB'))
            export_red=int(((export[:,:,0]>170)&(export[:,:,1]<100)&(export[:,:,2]<100)).sum())
            analysis_path=folder/'editor-video-analysis.json'
            analysis=json.loads(analysis_path.read_text()) if analysis_path.exists() else None
            item=dict(engine=record['engine'],case=record['case'],source_folder=str(folder.relative_to(args.output)).replace('\\','/'),
                      source_video='actual-ui.mkv',video=record['video'],
                      real_duration_s=record['video']['last_pts']-start,
                      timing_warning='Container timestamps are absolute epoch values. Normalize with setpts=PTS-STARTPTS; do not infer duration from format.duration or speed up the recording.',
                      scope='Actual standalone editor. Native uses unchanged production HWND renderer in a guarded harness; ShareX22 is the unmodified stock app via -portable -ImageEditor.',
                      ready=record['ready'],fixture_sha256=record['fixture_sha256'],phases=phases,
                      recorded_input_overlay=overlays,overlay_policy='A neutral ring may represent these actual sender coordinates if labeled Recorded input position; it is not a captured application cursor.',
                      export_dimensions=[export.shape[1],export.shape[0]],export_red_pixels=export_red,
                      export_sha256=hashlib.sha256((folder/'editor-snapshot.png').read_bytes()).hexdigest(),analysis=analysis)
            records.append(item)
            if analysis:
                for phase in analysis['phases']:
                    lag=phase.get('input_to_sample_ms') or {};gap=phase.get('video_sampled_progress_intervals_ms') or {}
                    rows.append(dict(engine=record['engine'],case=record['case'],phase=phase['phase'],image_width=record['ready']['image_size'][0],image_height=record['ready']['image_size'][1],
                                     video_frames=record['video']['frame_count'],observed_video_fps=record['video']['observed_pts_fps'],
                                     first_red_sample_ms=phase.get('first_move_to_first_red_sample_ms'),point_sample_n=lag.get('n'),point_sample_median_ms=lag.get('median'),point_sample_p95_ms=lag.get('p95'),point_sample_max_ms=lag.get('maximum'),
                                     sampled_progress_gap_p95_ms=gap.get('p95'),sampled_progress_gap_max_ms=gap.get('maximum'),
                                     gesture_repetitions=1,scope='Video-sampled estimates with about33ms observer intervals and shape/localization differences. Not physical latency, UI FPS, or independent n30 gesture trials.'))
                    sender=[e for e in record['injected_events'] if e.get('kind')=='move' and e.get('phase')==phase['phase'] and e.get('index',-1)>=0]
                    original_phase=next(p for p in record['phases'] if p['name']==phase['phase'])
                    origin=utc(original_phase['qpc_start'])
                    for observation in phase['observations']:
                        sent=[e for e in sender if utc(e['qpc_before'])<=observation['pts']]
                        input_index=max((e['index'] for e in sent),default=0)
                        observed=observation.get('nearest_trajectory_index')
                        if abs(observation.get('x_localization_error_px',0))>8:observed=None
                        curves.append(dict(engine=record['engine'],case=record['case'],phase=phase['phase'],time_s=observation['pts']-origin,
                                           input_fraction=input_index/119,observed_fraction=observed/119 if observed is not None else None,
                                           input_index=input_index,observed_index=observed,red_pixels=observation['red_pixels'],
                                           visible_tip_x=observation.get('tip_x_global'),localization_error_px=observation.get('x_localization_error_px'),
                                           scope='Actual decoded red tip vs actual timestamped sender, one gesture, about33ms sampling and renderer/smoothing differences'))
    manifest=dict(schema_version=1,source='Actual guarded disposable Windows Sandbox editor recordings, synthetic PNG only',
                  clocks='Precise paired FILETIME/QPC sender anchors and decoded epoch capture PTS, all original timings retained.',
                  speed='Real time, 1x. The manifest gives phase offsets, not retiming.',runs=records)
    (args.docs/'editor_ui_runs.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    if rows:
        with (args.docs/'editor_ui_summary.csv').open('w',newline='',encoding='utf-8') as f:
            writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    if curves:
        with (args.docs/'editor_ui_progress.csv').open('w',newline='',encoding='utf-8') as f:
            writer=csv.DictWriter(f,fieldnames=list(curves[0]));writer.writeheader();writer.writerows(curves)
    print(json.dumps({'runs':len(records),'summary_rows':len(rows),'progress_rows':len(curves),'manifest':str(args.docs/'editor_ui_runs.json')},indent=2))


if __name__=='__main__':main()
