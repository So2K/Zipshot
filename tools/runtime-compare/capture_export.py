"""Export the 3x10 baseline hotkey blocks as auditable CSV and grouped JSON.

No screenshots or personal process data are copied. Original per-job JSON files
remain the primary artifacts; their SHA256 values identify this derived export.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re

from capture_summarize import summarize


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=Path('docs/runtime-comparison'))
    args=parser.parse_args(); source=args.stage/'output'; args.output.mkdir(parents=True,exist_ok=True)
    files=sorted(source.glob('cap-b*-*/capture-results.json'))
    if len(files)!=12: raise ValueError(f'Expected 12 complete baseline blocks, found {len(files)}')
    records=[]; frames=[]; manifest=[]; groups={}
    fields=('index','warmup','status','send_qpc','send_return_qpc','last_baseline_qpc','first_visible_qpc',
            'visible_lower_bound_ms','visible_upper_bound_ms','target_window_evidence','clipboard_valid_qpc',
            'clipboard_observed_ms','clipboard_width','clipboard_height','file_complete_qpc','file_observed_ms',
            'file_width','file_height','observer_frames','max_observer_gap_ms')
    for path in files:
        data=json.loads(path.read_text(encoding='utf-8-sig'))
        meta_path=path.parent/'metadata.json';meta=json.loads(meta_path.read_text(encoding='utf-8-sig'))
        if data['status']!='pass' or meta['observer_enabled']: raise ValueError('Unqualified or video-observed baseline block')
        measured=[t for t in data['trials'] if not t['warmup']]
        if len(measured)!=10 or any(t['status']!='pass' for t in measured): raise ValueError('Incomplete measured block')
        block=int(re.search(r'cap-b(\d+)-',path.parent.name)[1])
        groups.setdefault((meta['app'],data['scenario']),[]).append((path,data,meta))
        manifest.append({'job':path.parent.name,'raw_json_sha256':sha(path),'metadata_sha256':sha(meta_path),
                         'app':meta['app'],'app_version':meta['version'],'app_exe_sha256':meta['app_sha256'],
                         'controller_sha256':meta['controller_sha256'],'qpc_frequency':data['qpc_frequency'],
                         'anchor_qpc':data['anchor_qpc'],'anchor_unix_100ns':data['anchor_unix_100ns']})
        for trial in data['trials']:
            base={'job':path.parent.name,'app':meta['app'],'version':meta['version'],'scenario':data['scenario'],'block':block}
            records.append(base|{field:trial[field] for field in fields})
            for ordinal,sample in enumerate(trial['frame_samples_qpc_changed_count']):
                frames.append(base|{'trial_index':trial['index'],'warmup':trial['warmup'],'frame_ordinal':ordinal,
                                   'readback_start_qpc':sample[0],'readback_end_qpc':sample[1],'changed_pixels_capped_at_16':sample[2]})
    summaries=[]
    for (app,scenario),blocks in sorted(groups.items()):
        if len(blocks)!=3: raise ValueError('Expected three blocks per app/scenario')
        first=blocks[0][1];identity=blocks[0][2]
        for path,data,meta in blocks[1:]:
            if any(data[k]!=first[k] for k in ('desktop','observer_roi','scope','input_method','visual_method')): raise ValueError('Mixed measurement method')
            if any(meta[k]!=identity[k] for k in ('app_sha256','controller_sha256','version','observer_enabled')): raise ValueError('Mixed application or controller identity')
        combined=dict(first);combined['trials']=[trial for _,data,_ in blocks for trial in data['trials']]
        summary=summarize(Path(f'derived/{app}-{scenario}.json'),combined)
        summary.update(app=app,version=identity['version'],block_count=3,app_exe_sha256=identity['app_sha256'],
                       controller_sha256=identity['controller_sha256'],source_jobs=[path.parent.name for path,_,_ in blocks])
        if not summary['minimum_sample_count_gate_passed']: raise ValueError('Merged sample/dimension gate failed')
        summaries.append(summary)
    for name,rows in (('capture_trials.csv',records),('capture_frames.csv',frames)):
        with (args.output/name).open('w',newline='',encoding='utf-8') as stream:
            writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    ready=json.loads((source/'guest-ready.json').read_text(encoding='utf-8-sig'))
    report={'schema_version':1,'dataset':'Unmodified Zipshot 1.0.1 versus official ShareX 22.4.0 baseline',
            'scope':'Warm full-application hotkeys through guest SendInput, not physical hardware input; measured in Windows Sandbox, separate from the prior host API benchmark.',
            'environment':{'os':ready['os'],'gpu':ready['gpu'],'logical_processors':ready['logical_processors'],
                           'display':ready['display'],'remote_session':True},
            'protocol':{'blocks':3,'measured_per_block':10,'discarded_warmups_per_process':3,
                        'order':'Zipshot first in blocks 1 and 3, ShareX first in block 2; region and full capture separate.',
                        'recorder_running_during_baseline':False,'pixel_observer_roi':[1280,720],
                        'visual_response':'Last unchanged readback start through first changed readback end, with target-window evidence.',
                        'clipboard_file_response':'First observed valid clipboard image and closed WIC-readable file; includes observer validation overhead.',
                        'full_zipshot_toast':'Unobservable through screen APIs because production toast uses WDA_EXCLUDEFROMCAPTURE; missing values are not zero latency.'},
            'measured_trials':sum(not row['warmup'] for row in records),'discarded_warmups':sum(bool(row['warmup']) for row in records),
            'summaries':summaries,'source_manifest':manifest,
            'csv':{name:{'sha256':sha(args.output/name)} for name in ('capture_trials.csv','capture_frames.csv')},
            'limits':['No universal speed or memory claim.','Actual display is a virtual RDP/vGPU environment, not the physical host desktop.',
                      'Compositor readback sampling limits visual timing resolution; intervals and gaps are retained.',
                      'Recorded demonstrations are separate workloads with their own source clocks and observed frame rates.']}
    (args.output/'capture_baseline.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(json.dumps({'measured':report['measured_trials'],'warmups':report['discarded_warmups'],'raw_frames':len(frames),'groups':len(summaries)},indent=2))


if __name__=='__main__':main()
