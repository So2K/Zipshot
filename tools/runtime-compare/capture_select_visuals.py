"""Select recorded trials closest to each three-trial recorded median.

Keeps source epoch/QPC mapping, real packet gaps, artifact hashes, and the separate
unrecorded-baseline scope. Does not edit, retime, interpolate, or re-encode video.
"""
import argparse
import csv
from decimal import Decimal
import hashlib
import json
from pathlib import Path, PureWindowsPath
import statistics


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage',type=Path,required=True)
    p.add_argument('--series',default='v02')
    p.add_argument('--output',type=Path,default=Path('docs/runtime-comparison/capture_visual_manifest.json'))
    a=p.parse_args();root=a.stage/'output';clips=[]
    for app in ('zipshot','sharex22'):
        for scenario in ('region','full'):
            case=f'cap-{a.series}-{app}-{scenario}';folder=root/case
            state=json.loads((root/(case+'-status.json')).read_text(encoding='utf-8-sig'))
            if state['status']!='pass':raise ValueError(f'Visual run did not pass: {case}')
            data=json.loads((folder/'capture-results.json').read_text(encoding='utf-8-sig'))
            meta=json.loads((folder/'metadata.json').read_text(encoding='utf-8-sig'))
            trials=[t for t in data['trials'] if not t['warmup']]
            if len(trials)!=3 or any(t['status']!='pass' for t in trials):raise ValueError('Expected three successful measured visual trials')
            field='visible_upper_bound_ms' if scenario=='region' else 'file_observed_ms'
            endpoint='first_visible_qpc' if scenario=='region' else 'file_complete_qpc'
            median=statistics.median(t[field] for t in trials)
            selected=min(trials,key=lambda t:(abs(t[field]-median),t['index']))
            frequency=Decimal(data['qpc_frequency']);start_epoch=Decimal(str(meta['observer_source_start_unix_seconds']))
            def video_time(qpc):
                return float(Decimal(data['anchor_unix_100ns'])/Decimal(10000000)+(Decimal(qpc)-Decimal(data['anchor_qpc']))/frequency-start_epoch)
            input_time=video_time(selected['send_qpc']);endpoint_time=video_time(selected[endpoint])
            stream=meta['observer_probe']['streams'][0];duration=float(meta['observer_probe']['format']['duration'])
            if not 0<input_time<endpoint_time<duration:raise ValueError('QPC/video epoch mapping leaves source range')
            packets=[]
            with (folder/'observer-frame-times.csv').open(encoding='utf-8-sig',newline='') as stream_csv:
                for row in csv.reader(stream_csv):
                    if row and row[0] not in ('','N/A'):packets.append(float(row[0]))
            gaps=[(b-a)*1000 for a,b in zip(packets,packets[1:])]
            if gaps and min(gaps)<0:raise ValueError('Source timestamps are not monotonic')
            proof=None
            if scenario=='region':
                if not selected['target_window_evidence']:raise ValueError('Selected region lacks product-window corroboration')
                if selected.get('configured_region_post_evidence_hold_ms',0)<700:raise ValueError('Insufficient real visual hold')
                proof=PureWindowsPath(selected['endpoint_full_frame']).name
            else:
                copies=[c for c in meta['actual_saved_image_copies'] if c['trial']==selected['index']]
                if len(copies)!=1:raise ValueError('Selected full capture lacks an actual saved image copy')
                proof=copies[0]['file']
                if sha(folder/proof).lower()!=copies[0]['sha256'].lower():raise ValueError('Saved image hash mismatch')
            if not (folder/proof).is_file():raise ValueError('Missing actual endpoint proof')
            escape=video_time(selected['escape_sent_qpc']) if selected.get('escape_sent_qpc') else None
            trim_end=min(duration,endpoint_time+1.0)
            if escape is not None:trim_end=min(trim_end,escape-.1)
            clips.append({'app':app,'version':meta['version'],'scenario':scenario,'case':case,
                          'selected_trial':selected['index'],'selection':'Closest to the median of its own three recorded valid trials; never best-case selection.',
                          'endpoint_field':field,'selected_endpoint_ms':selected[field],
                          'recorded_endpoint_values_ms':[t[field] for t in trials],
                          'input_video_s':input_time,'endpoint_video_s':endpoint_time,
                          'visual_lower_video_s':video_time(selected['last_baseline_qpc']) if scenario=='region' else None,
                          'escape_video_s':escape,'suggested_trim_start_s':max(0,input_time-.6),'suggested_trim_end_s':trim_end,
                          'source_video':case+'/observer.mkv','source_video_sha256':sha(folder/'observer.mkv'),
                          'source_results_sha256':sha(folder/'capture-results.json'),'source_metadata_sha256':sha(folder/'metadata.json'),
                          'source_display':meta['display'],'source_recording_size':[stream['width'],stream['height']],
                          'source_frames':int(stream['nb_read_frames']),'source_duration_s':duration,
                          'actual_framecount_per_second':int(stream['nb_read_frames'])/duration,
                          'packet_gap_median_ms':statistics.median(gaps) if gaps else None,'packet_gap_max_ms':max(gaps) if gaps else None,
                          'requested_recording_fps':meta['video_requested_fps'],
                          'full_desktop_video':meta['video_full_desktop'],'input_method':data['input_method'],
                          'actual_proof_file':case+'/'+proof,'actual_proof_sha256':sha(folder/proof),
                          'zipshot_toast_excluded_from_capture':app=='zipshot' and scenario=='full',
                          'region_post_evidence_hold_ms':selected.get('configured_region_post_evidence_hold_ms') if scenario=='region' else None})
    report={'schema_version':1,'series':a.series,'artifact_root':'isolated session output folder',
            'time_policy':'Original source timestamps retained. Trims shift only the origin; footage is not retimed or interpolated.',
            'measurement_scope':'Recorded demonstrations have additional capture overhead and their own endpoint clocks; canonical unrecorded n=30 baseline remains separate.',
            'clips':clips}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(json.dumps([{'case':c['case'],'trial':c['selected_trial'],'ms':c['selected_endpoint_ms'],'input_s':c['input_video_s'],'stop_s':c['endpoint_video_s'],'actual_fps':c['actual_framecount_per_second']} for c in clips],indent=2))


if __name__=='__main__':main()
