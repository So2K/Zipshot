"""Compose genuine hotkey recordings using their own QPC/epoch event clocks.

Each pane samples its original VFR chronology at 1x. The camera continues for
100 ms after its observed endpoint, then holds that captured frame. Output CFR
30 is timeline sampling, not motion interpolation. Installed Ubuntu fonts are
rasterized; font binaries are never copied.
"""
import argparse
import bisect
from decimal import Decimal
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT=Path(__file__).resolve().parents[2]
FONT=Path('C:/Windows/Fonts')
BG,PANEL,TEXT,MUTED,BLUE,BORDER='#181A1E','#27282B','#EEF0F3','#B8BDC5','#3A8DFF','#3D3E42'
FONTS={}
FRAME_SIZE=(860,484)
POSTROLL=.100


def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def face(size,weight='Regular'):
    key=size,weight
    if key not in FONTS:FONTS[key]=ImageFont.truetype(str(FONT/f'Ubuntu-{weight}.ttf'),size)
    return FONTS[key]


def write(draw,xy,value,size=26,color=TEXT,weight='Regular'):
    draw.text(xy,value,font=face(size,weight),fill=color,anchor='lt')


def fitted(image,size,color=PANEL):
    result=Image.new('RGB',size,color)
    actual=ImageOps.contain(image.convert('RGB'),size,Image.Resampling.LANCZOS)
    result.paste(actual,((size[0]-actual.width)//2,(size[1]-actual.height)//2))
    return result


def load(run,trial_index,endpoint,lead=.8):
    raw=json.loads((run/'capture-results.json').read_text(encoding='utf-8-sig'))
    meta=json.loads((run/'metadata.json').read_text(encoding='utf-8-sig'))
    trial=next(t for t in raw['trials'] if not t['warmup'] and t['index']==trial_index)
    if trial['status']!='pass':raise RuntimeError('Failed capture cannot be a successful example')
    field='visible_upper_bound_ms' if endpoint=='visible' else ('file_observed_ms' if endpoint=='file' else 'clipboard_observed_ms')
    values=[t[field] for t in raw['trials'] if not t['warmup'] and t['status']=='pass' and t[field]>=0]
    median=statistics.median(values)
    if abs(trial[field]-median)>1e-5:raise RuntimeError('Selected trial is not the median filmed example')
    if endpoint=='visible':
        if not trial['target_window_evidence'] or trial[field]<0:raise RuntimeError('Uncorroborated visual event')
        lower=trial['visible_lower_bound_ms'];endpoint_qpc=trial['first_visible_qpc']
    else:
        lower=None;endpoint_qpc=trial['file_complete_qpc'] if endpoint=='file' else trial['clipboard_valid_qpc']
        if trial[field]<0:raise RuntimeError('Missing output readiness event')
    def video_time(qpc):
        return float(Decimal(raw['anchor_unix_100ns'])/Decimal(10000000)+
            (Decimal(qpc)-Decimal(raw['anchor_qpc']))/Decimal(raw['qpc_frequency'])-
            Decimal(str(meta['observer_source_start_unix_seconds'])))
    key_video=video_time(trial['send_qpc']);endpoint_video=video_time(endpoint_qpc)
    source=run/'observer.mkv'
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0','-show_frames',
        '-show_entries','frame=best_effort_timestamp_time','-of','json',str(source)],text=True))
    all_pts=[float(x['best_effort_timestamp_time']) for x in probe['frames']]
    first=max(0,bisect.bisect_right(all_pts,key_video-lead)-1)
    last=min(len(all_pts),bisect.bisect_right(all_pts,endpoint_video+POSTROLL)+1)
    if first>=last:raise RuntimeError('No source frames cover the event')
    size=FRAME_SIZE
    # Raw RGB buffers have no timeline. Enumerate their sink timestamps uniquely
    # while keeping the ORIGINAL ffprobe PTS array for every displayed frame.
    vf=(f"select='between(n,{first},{last-1})',setpts=N,scale={size[0]}:{size[1]}:"
        f"force_original_aspect_ratio=decrease:flags=lanczos,pad={size[0]}:{size[1]}:(ow-iw)/2:(oh-ih)/2:color=0x181A1E")
    pixels=subprocess.check_output(['ffmpeg','-v','error','-i',str(source),'-vf',vf,
        '-fps_mode','passthrough','-enc_time_base','1:1000000','-f','rawvideo','-pix_fmt','rgb24','-'])
    frame_bytes=size[0]*size[1]*3;pts=all_pts[first:last]
    if len(pixels)!=frame_bytes*len(pts):raise RuntimeError('Decoded frames and timestamps differ')
    frames=[Image.frombytes('RGB',size,pixels[i*frame_bytes:(i+1)*frame_bytes]) for i in range(len(pts))]
    del pixels
    photo=next(iter(sorted(run.glob(f'capture-endpoint-{trial_index}.png'))),None)
    if photo is None:photo=next(iter(sorted(run.glob(f'capture-endpoint-{trial_index}.bmp'))),None)
    toolbar=None
    if photo and endpoint=='visible':
        with Image.open(photo) as im:
            mid=im.width//2;toolbar=fitted(im.crop((mid-800,0,mid+800,96)),(860,56))
    saved=None;saved_path=None
    if endpoint=='file':
        item=next(c for c in meta['actual_saved_image_copies'] if c['trial']==trial_index)
        saved_path=run/item['file']
        if digest(saved_path).lower()!=item['sha256'].lower():raise RuntimeError('Saved PNG hash mismatch')
        with Image.open(saved_path) as im:saved=fitted(im,(140,80),BG)
    stream=meta['observer_probe']['streams'][0];duration=float(meta['observer_probe']['format']['duration'])
    return {'run':run.name,'source':source,'raw_sha256':digest(run/'capture-results.json'),
        'video_sha256':digest(source),'trial':trial,'meta':meta,'key_video':key_video,'endpoint_video':endpoint_video,
        'latency':(endpoint_video-key_video)*1000,'reported_latency':trial[field],'lower':lower,'selection_sample_count':len(values),
        'pts':pts,'frames':frames,'toolbar':toolbar,'photo':photo,'saved':saved,'saved_path':saved_path,
        'actual_source_fps':int(stream['nb_read_frames'])/duration,'source_size':[stream['width'],stream['height']]}


def source_index(data,seconds):
    return max(0,min(len(data['frames'])-1,bisect.bisect_right(data['pts'],seconds)-1))


def frame_at(data,seconds):
    return data['frames'][source_index(data,seconds)]


def compose(left,right,out,endpoint,labels,seconds=5.5,lead=.8,make_gif=True):
    out.parent.mkdir(parents=True,exist_ok=True);fps=30
    title='Ctrl + PrtScn' if endpoint=='visible' else 'PrtScn'
    label={'visible':'First captured interface response','file':'PNG file ready','clipboard':'Clipboard image ready'}[endpoint]
    proc=subprocess.Popen(['ffmpeg','-v','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s','1920x1080','-r',str(fps),'-i','-',
        '-an','-c:v','libx264','-preset','fast','-crf','18','-threads','4','-pix_fmt','yuv420p','-movflags','+faststart',str(out)],
        stdin=subprocess.PIPE,creationflags=0x08004000)
    for index in range(round(seconds*fps)):
        now=index/fps;elapsed=max(0,now-lead)
        image=Image.new('RGB',(1920,1080),BG);d=ImageDraw.Draw(image)
        write(d,(40,34),title,42,weight='Bold');write(d,(450,44),label,30,MUTED)
        write(d,(40,92),'Aligned OS inputs inside an isolated Windows guest',23,MUTED)
        for k,(data,app_label) in enumerate(zip((left,right),labels)):
            x=40+k*940;limit=data['latency']/1000
            d.rounded_rectangle((x,138,x+900,974),radius=16,fill=PANEL,outline=BORDER,width=1)
            write(d,(x+22,163),app_label,35,BLUE if k else TEXT,'Medium')
            displayed=min(elapsed*1000,data['latency']) if now>=lead else 0
            write(d,(x+558,157),f'{math.ceil(displayed)} ms',58,BLUE if k else TEXT,'Medium')
            badge='WAITING' if now<lead else ('RESPONSE OBSERVED' if now>=lead+limit else 'INPUT SENT')
            d.rounded_rectangle((x+20,225,x+330,262),radius=6,fill=BLUE if now>=lead else '#34373D')
            write(d,(x+32,233),badge,21,weight='Medium')
            source_time=data['key_video']+min(now-lead,limit+POSTROLL)
            image.paste(frame_at(data,source_time),(x+20,280))
            if data['lower'] is not None:
                write(d,(x+22,792),f"Pixel interval {math.floor(data['lower'])}–{math.ceil(data['latency'])} ms",27,MUTED)
            else:write(d,(x+22,792),f"First observed closed PNG, {math.ceil(data['latency'])} ms",27,MUTED)
            fps_note=f"Recorded source: {data['actual_source_fps']:.1f} fps observed"
            if endpoint=='file' and data['trial']['visible_upper_bound_ms']<0 and data['trial']['target_window_evidence'] is False:
                fps_note+=' · toast excluded'
            write(d,(x+22,835),fps_note,23,MUTED)
            if now>=lead+limit+POSTROLL:
                if data['toolbar']:
                    write(d,(x+22,879),'Post-response screenshot detail · actual toolbar',21,MUTED)
                    image.paste(data['toolbar'],(x+20,914))
                elif data['saved']:
                    image.paste(data['saved'],(x+22,881))
                    write(d,(x+180,888),'Actual saved PNG',24,weight='Medium')
                    display=data['meta']['display']
                    write(d,(x+180,927),f"{display['width']}×{display['height']} · file copy verified",21,MUTED)
            elif endpoint=='file':write(d,(x+22,899),'Waiting for the image file',24,MUTED)
        write(d,(40,1005),'1× source chronology before result · Result frame held after 100 ms of real post-roll',24,MUTED)
        write(d,(40,1046),'Median-selected filmed examples. Repeated n=30 measurements are documented separately.',22,MUTED)
        proc.stdin.write(image.tobytes())
    proc.stdin.close()
    if proc.wait():raise RuntimeError('Video encoding failed')
    if make_gif:
        subprocess.run(['ffmpeg','-v','error','-y','-i',str(out),'-filter_complex',
            '[0:v]fps=15,scale=960:-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3',
            '-loop','0',str(out.with_suffix('.gif'))],check=True,creationflags=0x08004000)
    runs=[]
    for data,app_label in zip((left,right),labels):
        hold_time=data['endpoint_video']+POSTROLL;hold_index=source_index(data,hold_time)
        runs.append({'app_label':app_label,'run':data['run'],'selected_trial':data['trial']['index'],
            'selection':f"Median of its own {data['selection_sample_count']} recorded valid trials; no best-case selection.",
            'raw_sha256':data['raw_sha256'],'video_sha256':data['video_sha256'],'key_video':data['key_video'],
            'endpoint_video':data['endpoint_video'],'latency_ms':data['latency'],'readback_lower_ms':data['lower'],
            'source_size':data['source_size'],'observed_source_fps':data['actual_source_fps'],
            'camera_hold_time_source_s':hold_time,'camera_hold_frame_pts':data['pts'][hold_index],
            'decoded_buffer_timeline':'Untimed RGB buffers; displayed positions use original source PTS, including ties.',
            'output_input_marker_s':lead,'output_endpoint_clock_stop_s':lead+data['latency']/1000,
            'static_detail_photo_sha256':digest(data['photo']) if data['photo'] else None,
            'actual_saved_png_sha256':digest(data['saved_path']) if data['saved_path'] else None})
    record={'schema_version':1,'scope':'Actual full-app global hotkey path inside Windows Sandbox. SendInput OS events, not a hardware keyboard.',
        'endpoint':endpoint,'input_badge':'INPUT SENT','key_alignment_seconds':lead,'duration_seconds':seconds,
        'output_size':[1920,1080],'output_fps':fps,'gif_size':[960,540] if make_gif else None,'gif_sample_fps':15 if make_gif else None,
        'aspect_ratio_policy':'Source fits inside each viewport with neutral padding; no stretching.',
        'timeline':'Causal sampling of original VFR timestamps at 1x. Camera continues 100 ms after its own observed endpoint, then holds that captured frame. No pre-result delay or motion interpolation.',
        'static_details':'Only post-response context: genuine captured toolbar photo or actual product-saved PNG, labelled separately from source video.',
        'notification_note':'Zipshot marks its toast WDA_EXCLUDEFROMCAPTURE. File-readiness timing does not depend on toast visibility, and the PNG preview is an actual saved file, not a reconstructed notification.' if endpoint=='file' else None,
        'app_runs':runs,'output_sha256':digest(out),'gif_sha256':digest(out.with_suffix('.gif')) if make_gif else None}
    out.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    return record


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--left',type=Path,required=True);p.add_argument('--right',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--endpoint',choices=['visible','file','clipboard'],default='visible')
    p.add_argument('--trial',type=int,default=0,help='Compatibility fallback for both panes')
    p.add_argument('--left-trial',type=int);p.add_argument('--right-trial',type=int)
    p.add_argument('--left-label',default='ShareX 22.4.0');p.add_argument('--right-label',default='Zipshot 1.0.1')
    p.add_argument('--gif',action='store_true');p.add_argument('--seconds',type=float,default=5.5)
    a=p.parse_args();lead=.8
    left=load(a.left,a.trial if a.left_trial is None else a.left_trial,a.endpoint,lead)
    right=load(a.right,a.trial if a.right_trial is None else a.right_trial,a.endpoint,lead)
    compose(left,right,a.out,a.endpoint,(a.left_label,a.right_label),a.seconds,lead,a.gif)
    print(a.out,flush=True)


if __name__=='__main__':main()
