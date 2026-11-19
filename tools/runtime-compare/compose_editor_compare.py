"""Render actual editor input sequences side by side, preserving source chronology.

Epoch PTS are normalized by subtracting the first decoded PTS. Clocks show input
sequence elapsed time, never rendering/response latency. The optional yellow
ring is explicitly a logged input position, not a recreated application cursor.
"""
import argparse
import bisect
from decimal import Decimal
import json
import math
from pathlib import Path
import statistics
import subprocess

from PIL import Image,ImageDraw,ImageOps
import compose_capture_compare as style

ROOT=Path(__file__).resolve().parents[2]
CASES=('arrow','freehand','blur','zoom','undo','text','pixelate','colorpicker')
LABELS={'arrow':'Arrow','freehand':'Freehand','blur':'Blur · cold and repeat','zoom':'Zoom',
        'undo':'Undo / redo','text':'Text','pixelate':'Pixelation','colorpicker':'Colour picker'}
PHASE_LABELS={'arrow':'Draw arrow','freehand':'Draw freehand','blur_cold':'Cold blur input',
              'blur_repeat':'Repeat blur input','zoom':'Mouse-wheel sequence','undo_redo':'Undo / redo keys',
              'text':'Type text','pixelate':'Pixelate region','colorpicker':'Open colour picker'}
VIEW=(860,605)
LEAD=.8
POSTROLL=.2
YELLOW='#E5CB78'


def source_index(data,time):
    return max(0,min(len(data['frames'])-1,bisect.bisect_right(data['pts'],time)-1))


def load_run(run,source_root,qa):
    source=source_root/run['source_folder']/run['source_video']
    if style.digest(source).lower()!=run['video']['sha256'].lower():raise ValueError('Editor source hash mismatch')
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0','-show_frames',
        '-show_entries','frame=best_effort_timestamp_time,width,height','-of','json',str(source)],text=True))
    original=[Decimal(frame['best_effort_timestamp_time']) for frame in probe['frames']]
    origin=original[0];pts=[float(value-origin) for value in original]
    if len(pts)!=run['video']['frame_count']:raise ValueError('Source decoded-frame count differs from manifest')
    if abs(float(origin)-run['video']['first_pts'])>.002 or abs(float(original[-1])-run['video']['last_pts'])>.002:raise ValueError('Source epoch bounds differ')
    if any(b<a for a,b in zip(pts,pts[1:])):raise ValueError('Nonchronological source frames')
    width,height=probe['frames'][0]['width'],probe['frames'][0]['height']
    phases=run['phases']
    if run['case']=='undo':phases=[p for p in phases if p['name']=='undo_redo']
    first=min(p['video_offset_s'] for p in phases)
    complete=max(p['video_offset_s']+p['duration_s'] for p in phases)
    if complete>pts[-1]+.05:raise ValueError('Input sequence extends past recorded source')
    hold=min(pts[-1],complete+POSTROLL)
    vf=(f"setpts=N,scale={VIEW[0]}:{VIEW[1]}:force_original_aspect_ratio=decrease:flags=lanczos,"
        f"pad={VIEW[0]}:{VIEW[1]}:(ow-iw)/2:(oh-ih)/2:color=0x181A1E")
    pixels=subprocess.check_output(['ffmpeg','-v','error','-i',str(source),'-vf',vf,'-fps_mode','passthrough',
        '-enc_time_base','1:1000000','-f','rawvideo','-pix_fmt','rgb24','-'])
    frame_bytes=VIEW[0]*VIEW[1]*3
    if len(pixels)!=frame_bytes*len(pts):raise ValueError('Raw frame buffers do not match PTS')
    frames=[Image.frombytes('RGB',VIEW,pixels[i*frame_bytes:(i+1)*frame_bytes]) for i in range(len(pts))]
    del pixels
    # Preserve original-size first/last source frames for independent review.
    proofs={}
    for label,index in (('first',0),('last',len(pts)-1)):
        output=qa/f"editor-{run['case']}-{run['engine']}-source-{label}.png"
        subprocess.run(['ffmpeg','-v','error','-y','-i',str(source),'-vf',f"select='eq(n,{index})'",'-frames:v','1',str(output)],check=True)
        proofs[label]={'file':output.name,'sha256':style.digest(output)}
    scale=min(VIEW[0]/width,VIEW[1]/height);fit=(round(width*scale),round(height*scale))
    pad=((VIEW[0]-fit[0])//2,(VIEW[1]-fit[1])//2)
    overlay=sorted((p for p in run.get('recorded_input_overlay',[]) if p['seconds']>=0 and p['phase']!='focus_setup'),key=lambda p:p['seconds'])
    if run['case']=='undo':overlay=[]
    return {'run':run,'source':source,'source_hash':style.digest(source),'pts':pts,'frames':frames,
            'first_phase':first,'complete_phase':complete,'span':complete-first,'hold_time':hold,'phases':phases,
            'overlay':overlay,'overlay_times':[p['seconds'] for p in overlay], 'fit_scale':scale,'fit_pad':pad,
            'source_size':[width,height],'proofs':proofs,'actual_source_fps':(len(pts)-1)/(pts[-1]-pts[0])}


def local_time(data,output_time):
    if output_time<LEAD and data['run']['case']=='undo':
        # The arrow already exists before undo begins. This baseline hold is
        # pre-input context; every actual undo/redo event remains chronological.
        return max(0,data['first_phase']-.1)
    return max(0,min(data['hold_time'],data['first_phase']+output_time-LEAD))


def phase_name(data,time):
    for phase in data['phases']:
        if phase['video_offset_s']<=time<=phase['video_offset_s']+phase['duration_s']:
            return PHASE_LABELS.get(phase['name'],phase['name'])
    return 'Pause between inputs' if time<data['complete_phase'] else 'Sequence complete'


def ring_at(data,time):
    if not data['overlay']:return None
    index=bisect.bisect_right(data['overlay_times'],time)-1
    if index<0:return None
    point=data['overlay'][index]
    active=time-point['seconds']<=.12
    if data['run']['case']=='zoom' and data['first_phase']<=time<=data['complete_phase']:active=True
    if not active:return None
    x=data['fit_pad'][0]+point['x']*data['fit_scale'];y=data['fit_pad'][1]+point['y']*data['fit_scale']
    if not 0<=x<VIEW[0] or not 0<=y<VIEW[1]:return None
    return x,y


def render_case(case,left,right,out,qa):
    fps=30;sequence=max(left['span'],right['span']);duration=LEAD+sequence+1.0
    frame_count=math.ceil(duration*fps);duration=frame_count/fps
    proc=subprocess.Popen(['ffmpeg','-v','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s','1920x1080','-r',str(fps),'-i','-',
        '-an','-c:v','libx264','-preset','fast','-crf','18','-threads','4','-pix_fmt','yuv420p','-movflags','+faststart',str(out)],
        stdin=subprocess.PIPE,creationflags=0x08004000)
    for index in range(frame_count):
        now=index/fps;image=Image.new('RGB',(1920,1080),style.BG);draw=ImageDraw.Draw(image)
        style.write(draw,(40,34),LABELS[case],42,weight='Bold')
        style.write(draw,(40,94),'Actual editor UI · Input sequence at 1×',25,style.MUTED)
        for ordinal,(data,label) in enumerate(((left,'ShareX 22.4.0'),(right,'Zipshot 1.0.1'))):
            x=40+ordinal*940;elapsed=max(0,min(data['span'],now-LEAD));time=local_time(data,now)
            draw.rounded_rectangle((x,144,x+900,979),radius=16,fill=style.PANEL,outline=style.BORDER,width=1)
            style.write(draw,(x+22,169),label,35,style.BLUE if ordinal else style.TEXT,'Medium')
            style.write(draw,(x+592,164),f'{elapsed:.2f} s',51,style.TEXT,'Medium')
            style.write(draw,(x+22,223),'INPUT ELAPSED · '+('READY' if now<LEAD else phase_name(data,time)),21,style.MUTED)
            viewport=data['frames'][source_index(data,time)].copy()
            ring=ring_at(data,time)
            if ring is not None:
                point_draw=ImageDraw.Draw(viewport);px,py=ring
                point_draw.ellipse((px-7,py-7,px+7,py+7),outline=YELLOW,width=2)
            image.paste(viewport,(x+20,265))
            style.write(draw,(x+22,892),f"Source: {data['actual_source_fps']:.1f} fps observed",23,style.MUTED)
            style.write(draw,(x+22,935),'Recorded input position',22,YELLOW)
            draw.ellipse((x+344,940,x+356,952),outline=YELLOW,width=2)
        style.write(draw,(40,1006),'One recorded input sequence per app · Elapsed input time is not a response-time benchmark',24,style.MUTED)
        style.write(draw,(40,1047),'Original frame timing · No motion interpolation · Complete result held after the event sequence',22,style.MUTED)
        proc.stdin.write(image.tobytes())
    proc.stdin.close()
    if proc.wait():raise RuntimeError('Editor MP4 encoder failed')
    subprocess.run(['ffmpeg','-v','error','-y','-i',str(out),'-filter_complex',
        '[0:v]fps=15,scale=960:-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3',
        '-loop','0',str(out.with_suffix('.gif'))],check=True,creationflags=0x08004000)
    proof=[]
    for data in (left,right):
        proof.append({'engine':data['run']['engine'],'source_folder':data['run']['source_folder'],
            'source_video':data['run']['source_video'],'source_video_sha256':data['source_hash'],
            'source_size':data['source_size'],'source_first_epoch_pts':data['run']['video']['first_pts'],
            'source_last_epoch_pts':data['run']['video']['last_pts'],'source_real_duration_s':data['pts'][-1],
            'source_observed_fps':data['actual_source_fps'],'first_relevant_phase_s':data['first_phase'],
            'complete_input_sequence_s':data['complete_phase'],'input_sequence_duration_s':data['span'],
            'camera_hold_after_sequence_source_s':data['hold_time'],
            'phases':[{k:p[k] for k in ('name','video_offset_s','duration_s')} for p in data['phases']],
            'source_frame_proofs':data['proofs'],'full_window_fit_scale':data['fit_scale'],'full_window_padding':list(data['fit_pad']),
            'overlay_policy':'Small neutral yellow ring at the latest logged input point, sampled without interpolation; not an app cursor.',
            'pre_input_policy':'Still of preexisting arrow before undo; no input event is removed.' if case=='undo' else 'Original pre-input chronology, padding first frame only if recording starts later than the prelude.'})
    record={'schema_version':1,'case':case,'title':LABELS[case],'clock_metric':'Input sequence elapsed time only; no UI response-speed winner is implied.',
        'input_alignment_output_s':LEAD,'duration_seconds':duration,'output_size':[1920,1080],'output_cfr_fps':30,
        'gif_size':[960,540],'gif_timeline_sample_fps':15,'aspect_ratio_policy':'Full source windows scale to fit the same 860×605 viewport with neutral padding, never stretched.',
        'chronology':'Normalize each original epoch PTS by its first PTS. Actual event chronology remains 1x. Camera holds only after the entire relevant sequence plus up to 200 ms of recorded post-roll.',
        'source_runs':proof,'output_sha256':style.digest(out),'gif_sha256':style.digest(out.with_suffix('.gif'))}
    out.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    for second,label in ((0,'first'),(min(duration-.1,LEAD+sequence+.4),'last')):
        subprocess.run(['ffmpeg','-v','error','-y','-ss',str(second),'-i',str(out),'-frames:v','1',str(qa/f'editor-{case}-{label}.png')],check=True)
    subprocess.run(['ffmpeg','-v','error','-i',str(out),'-f','null','-'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    with Image.open(out.with_suffix('.gif')) as gif:
        if gif.size!=(960,540) or gif.info.get('loop')!=0:raise ValueError('Unexpected editor GIF format')
        gif_ms=0
        for i in range(gif.n_frames):gif.seek(i);gif_ms+=gif.info.get('duration',0)
        if abs(gif_ms/1000-duration)>.1:raise ValueError('GIF sequence time changed')
        verification={'full_decode_pass':True,'gif_frames':gif.n_frames,'gif_duration_s':gif_ms/1000,'gif_loop':0}
    return {'case':case,'duration_s':duration,'verification':verification,
            'sha256':{suffix:style.digest(out.with_suffix(suffix)) for suffix in ('.mp4','.gif','.json')}}


def load_region(run,source_root,qa):
    qa.mkdir(parents=True,exist_ok=True)
    data=load_run(run,source_root,qa)
    data['first_phase']=run['offsets']['actual_button_down_s']
    data['span']=data['complete_phase']-data['first_phase']
    data['overlay']=[];data['overlay_times']=[]  # The actual cursor is already recorded.
    crop=(700,320,1600,1100)
    vf=(f"setpts=N,crop={crop[2]}:{crop[3]}:{crop[0]}:{crop[1]},"
        f"scale={VIEW[0]}:{VIEW[1]}:force_original_aspect_ratio=decrease:flags=lanczos,"
        f"pad={VIEW[0]}:{VIEW[1]}:(ow-iw)/2:(oh-ih)/2:color=0x181A1E")
    pixels=subprocess.check_output(['ffmpeg','-v','error','-i',str(data['source']),'-vf',vf,'-fps_mode','passthrough',
        '-enc_time_base','1:1000000','-f','rawvideo','-pix_fmt','rgb24','-'])
    length=VIEW[0]*VIEW[1]*3
    if len(pixels)!=length*len(data['pts']):raise ValueError('Region cropped frames differ from source chronology')
    data['cropped_frames']=[Image.frombytes('RGB',VIEW,pixels[i*length:(i+1)*length]) for i in range(len(data['pts']))]
    del pixels
    ui_time=max(run['offsets']['region_hotkey_s'],run['offsets']['tool_key_s']-.2)
    ui_index=source_index(data,ui_time)
    ui_photo=qa/f"editor-region-{run['case']}-{run['engine']}-full-interface.png"
    subprocess.run(['ffmpeg','-v','error','-y','-i',str(data['source']),'-vf',f"select='eq(n,{ui_index})'",'-frames:v','1',str(ui_photo)],check=True)
    data['full_interface_proof']={'file':ui_photo.name,'source_video_local_s':data['pts'][ui_index],
        'sha256':style.digest(ui_photo),'label':'Actual full capture interface before tool selection and central presentation crop.'}
    data['screen_crop']=list(crop)
    return data


def render_region_case(case,left,right,out,qa):
    # Include each genuine capture-input transition. Align the actual button-down
    # stroke, never pad a product's post-hotkey response or accelerate preparation.
    lead=max(d['first_phase']-d['run']['offsets']['region_hotkey_s'] for d in (left,right))+.6
    fps=30;duration=math.ceil((lead+max(left['span'],right['span'])+1)*fps)/fps
    proc=subprocess.Popen(['ffmpeg','-v','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s','1920x1080','-r',str(fps),'-i','-',
        '-an','-c:v','libx264','-preset','fast','-crf','18','-threads','4','-pix_fmt','yuv420p','-movflags','+faststart',str(out)],
        stdin=subprocess.PIPE,creationflags=0x08004000)
    for index in range(round(duration*fps)):
        now=index/fps;image=Image.new('RGB',(1920,1080),style.BG);draw=ImageDraw.Draw(image)
        style.write(draw,(40,34),'Ctrl+PrtScn · '+LABELS[case],42,weight='Bold')
        style.write(draw,(40,94),'Genuine capture overlays · Stroke inputs aligned · Original 1× chronology',25,style.MUTED)
        for ordinal,(data,label) in enumerate(((left,'ShareX 22.4.0'),(right,'Zipshot 1.0.1'))):
            x=40+ordinal*940;time=max(0,min(data['hold_time'],data['first_phase']+now-lead))
            elapsed=max(0,min(data['span'],now-lead));drawing=now>=lead
            draw.rounded_rectangle((x,144,x+900,979),radius=16,fill=style.PANEL,outline=style.BORDER,width=1)
            style.write(draw,(x+22,169),label,35,style.BLUE if ordinal else style.TEXT,'Medium')
            style.write(draw,(x+592,164),f'{elapsed:.2f} s',51,style.TEXT,'Medium')
            if time<data['run']['offsets']['region_hotkey_s']:phase='Capture input pending'
            elif time<data['run']['offsets']['tool_key_s']:phase='Ctrl+PrtScn sent · Capture interface'
            elif not drawing:phase='Tool selected · Waiting for stroke'
            elif time<data['complete_phase']:phase='Stroke input elapsed'
            else:phase='Stroke sequence complete'
            style.write(draw,(x+22,223),phase,21,style.MUTED)
            frame=(data['cropped_frames'] if drawing else data['frames'])[source_index(data,time)]
            image.paste(frame,(x+20,265))
            style.write(draw,(x+22,892),'Drawing detail · Same screen crop' if drawing else 'Actual full capture interface',23,style.MUTED)
            style.write(draw,(x+22,935),f"Source: {data['actual_source_fps']:.1f} fps observed · Actual cursor recorded",22,style.MUTED)
        style.write(draw,(40,1006),'One real stroke per app · Application drawing defaults differ · No speed ranking',24,style.MUTED)
        style.write(draw,(40,1047),'Visible drawing only · Region PNG saving is not claimed · No retiming or motion interpolation',22,style.MUTED)
        proc.stdin.write(image.tobytes())
    proc.stdin.close()
    if proc.wait():raise RuntimeError('Region editor encoder failed')
    subprocess.run(['ffmpeg','-v','error','-y','-i',str(out),'-filter_complex',
        '[0:v]fps=15,scale=960:-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3',
        '-loop','0',str(out.with_suffix('.gif'))],check=True,creationflags=0x08004000)
    records=[]
    for data in (left,right):
        records.append({'engine':data['run']['engine'],'source_folder':data['run']['source_folder'],
            'source_video_sha256':data['source_hash'],'source_size':data['source_size'],
            'source_observed_fps':data['actual_source_fps'],'actual_cursor_recorded':True,
            'source_first_epoch_pts':data['run']['video']['first_pts'],'source_last_epoch_pts':data['run']['video']['last_pts'],
            'stroke_button_down_source_s':data['first_phase'],'stroke_complete_source_s':data['complete_phase'],
            'stroke_duration_s':data['span'],'hotkey_output_s':lead+data['run']['offsets']['region_hotkey_s']-data['first_phase'],
            'stroke_input_alignment_output_s':lead,'camera_hold_after_sequence_source_s':data['hold_time'],
            'full_interface_proof_before_crop':data['full_interface_proof'],'source_frame_proofs':data['proofs'],
            'drawing_source_crop_xywh':data['screen_crop'],'crop_policy':'Identical 1600×1100 screen rectangle for both, fitted without stretching after stroke begins.',
            'timeline':'Original source chronology before and after hotkey; only pre-recording first-frame padding before any input, if needed.'})
    record={'schema_version':1,'case':case,'scope':'Actual full-app Ctrl+PrintScreen overlays with recorded cursor; visible drawing only, no region-save claim.',
        'clock_metric':'Stroke input elapsed time, not UI response latency. No speed winner claimed.',
        'input_alignment_output_s':lead,'duration_seconds':duration,'output_size':[1920,1080],'output_cfr_fps':30,
        'gif_size':[960,540],'gif_timeline_sample_fps':15,'source_runs':records,
        'chronology':'Actual stroke button-down aligned, source timestamps remain 1×; real Ctrl+PrtScn transitions included. No post-hotkey delay added, no motion interpolation.',
        'output_sha256':style.digest(out),'gif_sha256':style.digest(out.with_suffix('.gif'))}
    out.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    for second,label in ((max(.1,lead-.5),'interface'),(lead+1,'stroke'),(duration-.25,'last')):
        subprocess.run(['ffmpeg','-v','error','-y','-ss',str(second),'-i',str(out),'-frames:v','1',str(qa/f'editor-region-{case}-{label}.png')],check=True)
    subprocess.run(['ffmpeg','-v','error','-i',str(out),'-f','null','-'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    with Image.open(out.with_suffix('.gif')) as gif:
        if gif.size!=(960,540) or gif.info.get('loop')!=0:raise ValueError('Unexpected region GIF format')
        gif_ms=0
        for i in range(gif.n_frames):gif.seek(i);gif_ms+=gif.info.get('duration',0)
        if abs(gif_ms/1000-duration)>.1:raise ValueError('Region GIF time changed')
        verification={'full_decode_pass':True,'gif_frames':gif.n_frames,'gif_duration_s':gif_ms/1000,'gif_loop':0}
    return {'case':case,'asset':'editor-region-'+case,'duration_s':duration,'verification':verification,
        'sha256':{suffix:style.digest(out.with_suffix(suffix)) for suffix in ('.mp4','.gif','.json')}}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--source-root',type=Path,default=Path('D:/ZipshotRuntimeCompare-20261118/session02/output'))
    parser.add_argument('--out',type=Path,default=ROOT/'media/runtime')
    parser.add_argument('--qa',type=Path,default=ROOT/'build/runtime-compare/editor-media-qa')
    parser.add_argument('--cases',nargs='+',choices=CASES)
    parser.add_argument('--region',action='store_true',help='Compose the separately verified full-app Ctrl+PrtScn overlays.')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True);args.qa.mkdir(parents=True,exist_ok=True)
    if args.manifest is None:args.manifest=ROOT/('docs/runtime-comparison/editor_region_runs.json' if args.region else 'docs/runtime-comparison/editor_ui_runs.json')
    if args.cases is None:args.cases=['arrow','freehand'] if args.region else list(CASES)
    manifest=json.loads(args.manifest.read_text());runs=manifest['runs'];assets=[]
    for case in args.cases:
        pair=[next(r for r in runs if r['case']==case and r['engine']==engine) for engine in ('sharex22','native')]
        if args.region:
            region_qa=args.qa/'region';region_qa.mkdir(parents=True,exist_ok=True)
            left,right=[load_region(run,args.source_root,region_qa) for run in pair]
            assets.append(render_region_case(case,left,right,args.out/f'editor-region-{case}.mp4',region_qa))
        else:
            if pair[0]['fixture_sha256']!=pair[1]['fixture_sha256']:raise ValueError('Different synthetic source images')
            left,right=[load_run(run,args.source_root,args.qa) for run in pair]
            assets.append(render_case(case,left,right,args.out/f'editor-{case}.mp4',args.qa))
        del left,right
        print('editor-'+case+' MP4/GIF and source proof review frames ready; full decode PASS',flush=True)
    report={'schema_version':1,'source_manifest_sha256':style.digest(args.manifest),'assets':assets,
            'scope':'Actual editor input demonstrations; one sequence per app, source windows unchanged. No UI latency winner claimed.',
            'time_policy':'Epoch PTS normalized by first source PTS; chronological 1x causal frame sampling. No speed changes or motion interpolation.',
            'font_policy':'Installed Ubuntu fonts rasterized for external captions; no font binaries copied.','original_sources_modified':False}
    destination=args.out/'editor-media-manifest.json'
    if args.region and destination.exists():
        prior=json.loads(destination.read_text());prior['region_assets']=assets;prior['region_source_manifest_sha256']=style.digest(args.manifest)
        prior['region_scope']='Actual full-app region overlays; original input chronology, visible drawing only, no save/speed claim.'
        report=prior
    destination.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
