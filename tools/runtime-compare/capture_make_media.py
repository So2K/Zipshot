"""Build publication microvideos/GIFs and genuine interface details.

Requires FFmpeg, Pillow, and locally installed Ubuntu fonts. Raw source recordings
and capture results are read-only. All selection follows capture_visual_manifest.
"""
import argparse
import json
from pathlib import Path
import subprocess

from PIL import Image,ImageDraw,ImageChops
import compose_capture_compare as film
import statistics


def frame_timing_check(data):
    """Independent source-video ROI check; masks only the OS pointer hotspot."""
    width,height=data['meta']['display']['width'],data['meta']['display']['height']
    roi=data['meta']['roi'];frame_w,frame_h=film.FRAME_SIZE
    source_w,source_h=data['source_size'];scale=min(frame_w/source_w,frame_h/source_h)
    picture_w=round(source_w*scale);picture_h=round(source_h*scale)
    pad_x=(frame_w-picture_w)//2;pad_y=(frame_h-picture_h)//2
    box=(round(pad_x+roi['x']*picture_w/width),round(pad_y+roi['y']*picture_h/height),
         round(pad_x+(roi['x']+roi['width'])*picture_w/width),round(pad_y+(roi['y']+roi['height'])*picture_h/height))
    baseline=film.frame_at(data,data['key_video']-.12).crop(box)
    cursor=(frame_w//2-box[0],frame_h//2-box[1])
    first=None
    for pts,frame in zip(data['pts'],data['frames']):
        if pts<data['key_video']:continue
        diff=ImageChops.difference(baseline,frame.crop(box)).convert('L')
        mask=ImageDraw.Draw(diff);mask.rectangle((cursor[0]-12,cursor[1]-12,cursor[0]+12,cursor[1]+12),fill=0)
        count=sum(diff.histogram()[8:])
        if count>=16:first=pts;break
    gaps=[b-a for a,b in zip(data['pts'],data['pts'][1:]) if b>a]
    period=statistics.median(gaps)
    latency=(first-data['key_video'])*1000 if first is not None else None
    accepted=latency is not None and data['lower']-period*1000<=latency<=data['latency']+period*1000
    return {'source_video_roi':list(box),'os_pointer_hotspot_masked':True,'first_source_changed_frame_pts':first,
            'first_source_frame_change_ms':latency,'median_source_period_ms':period*1000,
            'inside_readback_interval_plus_one_source_period':accepted}


def make_interface(clips,source_root,output):
    image=Image.new('RGB',(1600,1260),film.BG);draw=ImageDraw.Draw(image)
    film.write(draw,(48,40),'Ctrl+PrtScn captured interfaces',44,weight='Medium')
    film.write(draw,(48,108),'Real screenshots · Same 2982×1672 isolated Windows desktop',25,film.MUTED)
    ordered=[next(c for c in clips if c['app']==app and c['scenario']=='region') for app in ('sharex22','zipshot')]
    records=[]
    for index,clip in enumerate(ordered):
        source=source_root/clip['actual_proof_file']
        if film.digest(source)!=clip['actual_proof_sha256']:raise ValueError('Interface photo hash mismatch')
        with Image.open(source) as opened:
            photo=opened.convert('RGB');mid=photo.width//2
            toolbar=(mid-800,0,mid+800,96)
            loupe=(1260,760,1980,1280)
            if photo.width<toolbar[2] or photo.height<loupe[3]:raise ValueError('Photo is too small for identical source crops')
            name='ShareX 22.4.0' if index==0 else 'Zipshot 1.0.1'
            y=166+index*174
            film.write(draw,(48,y),name,33,film.BLUE if index else film.TEXT,'Medium')
            draw.rectangle((47,y+45,1553,y+143),outline=film.BORDER,width=1)
            image.paste(film.fitted(photo.crop(toolbar),(1504,96)),(48,y+46))
            x=48+index*784
            film.write(draw,(x,605),name,28,film.BLUE if index else film.TEXT,'Medium')
            draw.rectangle((x-1,649,x+721,1171),outline=film.BORDER,width=1)
            image.paste(film.fitted(photo.crop(loupe),(720,520)),(x,650))
            records.append({'app':clip['app'],'source':clip['actual_proof_file'],'source_sha256':film.digest(source),
                            'toolbar_source_crop':list(toolbar),'toolbar_output_size':[1504,96],
                            'loupe_source_crop':list(loupe),'loupe_output_size':[720,520]})
    draw.line((48,528,1552,528),fill=film.BORDER,width=1)
    film.write(draw,(48,553),'Selection and magnifier',28,weight='Medium')
    film.write(draw,(48,1211),'Identical source crop rectangles and scale · Genuine app pixels; no UI reconstruction',22,film.MUTED)
    image.save(output,optimize=True)
    return {'dimensions':[1600,1260],'source_crops':records,'sha256':film.digest(output)}


def verify_video(path):
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-count_frames','-select_streams','v:0',
        '-show_entries','stream=codec_name,width,height,pix_fmt,avg_frame_rate,nb_read_frames:format=duration','-of','json',str(path)],text=True))
    stream=probe['streams'][0]
    if (stream['width'],stream['height'],stream['codec_name'],stream['pix_fmt'])!=(1920,1080,'h264','yuv420p'):raise ValueError('Unexpected MP4 format')
    subprocess.run(['ffmpeg','-v','error','-i',str(path),'-f','null','-'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    gif=path.with_suffix('.gif')
    with Image.open(gif) as frames:
        if frames.size!=(960,540) or frames.info.get('loop')!=0:raise ValueError('Unexpected GIF size/loop')
        durations=[]
        for index in range(frames.n_frames):frames.seek(index);durations.append(frames.info.get('duration',0))
        gif_seconds=sum(durations)/1000
        if abs(gif_seconds-float(probe['format']['duration']))>.1:raise ValueError('GIF chronology duration differs')
        gif_info={'size':[960,540],'frames':frames.n_frames,'loop':0,'duration_seconds':gif_seconds}
    metadata=json.loads(path.with_suffix('.json').read_text())
    for run in metadata['app_runs']:
        if abs(run['output_endpoint_clock_stop_s']-metadata['key_alignment_seconds']-run['latency_ms']/1000)>1e-7:raise ValueError('Clock/source endpoint inconsistency')
        if run['camera_hold_frame_pts']>run['camera_hold_time_source_s']+1e-8:raise ValueError('A future source frame was shown early')
    return {'full_decode_pass':True,'mp4':probe,'gif':gif_info,'clock_endpoint_consistency':True,
            'causal_source_frame_selection':True,'input_badge':'INPUT SENT'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=Path('D:/ZipshotRuntimeCompare-20261118/session02/output'))
    parser.add_argument('--manifest',type=Path,default=film.ROOT/'docs/runtime-comparison/capture_visual_manifest.json')
    parser.add_argument('--out',type=Path,default=film.ROOT/'media/runtime')
    parser.add_argument('--qa',type=Path,default=film.ROOT/'build/runtime-compare/capture-media-qa')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True);args.qa.mkdir(parents=True,exist_ok=True)
    manifest=json.loads(args.manifest.read_text());records=[]
    for scenario,endpoint,name in (('region','visible','hotkey-region'),('full','file','hotkey-png')):
        pair=[next(c for c in manifest['clips'] if c['app']==app and c['scenario']==scenario) for app in ('sharex22','zipshot')]
        loaded=[]
        for clip in pair:
            folder=args.source_root/clip['case']
            if film.digest(folder/'observer.mkv')!=clip['source_video_sha256']:raise ValueError('Source video hash mismatch')
            if film.digest(folder/'capture-results.json')!=clip['source_results_sha256']:raise ValueError('Source results hash mismatch')
            if film.digest(folder/'metadata.json')!=clip['source_metadata_sha256']:raise ValueError('Source metadata hash mismatch')
            data=film.load(folder,clip['selected_trial'],endpoint)
            if data['selection_sample_count']!=3:raise ValueError('Expected three measured visual repeats')
            if abs(data['key_video']-clip['input_video_s'])>1e-6 or abs(data['endpoint_video']-clip['endpoint_video_s'])>1e-6:raise ValueError('Event clocks differ from selection manifest')
            if abs(data['reported_latency']-clip['selected_endpoint_ms'])>1e-5:raise ValueError('Selected endpoint differs from manifest')
            loaded.append(data)
        output=args.out/(name+'.mp4')
        film.compose(*loaded,output,endpoint,('ShareX 22.4.0','Zipshot 1.0.1'),make_gif=True)
        verification=verify_video(output)
        if endpoint=='visible':
            verification['independent_source_roi_checks']=[frame_timing_check(data) for data in loaded]
            if not all(check['inside_readback_interval_plus_one_source_period'] for check in verification['independent_source_roi_checks']):
                raise ValueError('Source ROI changes do not corroborate measured readback timing')
        for second,label in ((.2,'waiting'),(1.0,'input'),(2.0,'result')):
            subprocess.run(['ffmpeg','-v','error','-y','-ss',str(second),'-i',str(output),'-frames:v','1',str(args.qa/(name+'-'+label+'.png'))],check=True)
        records.append({'asset':name,'verification':verification,'sha256':{suffix:film.digest(output.with_suffix(suffix)) for suffix in ('.mp4','.gif','.json')}})
        print(name,'MP4/GIF format, full decode, source clocks: PASS',flush=True)
    interface=make_interface(manifest['clips'],args.source_root,args.out/'capture-interface.png')
    Image.open(args.out/'capture-interface.png').resize((900,709),Image.Resampling.LANCZOS).save(args.qa/'interface-900.png')
    report={'schema_version':1,'source_selection_manifest_sha256':film.digest(args.manifest),'assets':records,
            'interface':interface,'source_rate_note':'Observed source rates 22.6–25.2 fps are labelled. Output CFR30/GIF15 sample the timeline, with no motion interpolation.',
            'scope':'Actual guest SendInput demonstrations; recorded examples separate from canonical n=30 unrecorded measurements.',
            'original_artifacts_modified':False,'font_policy':'Locally installed Ubuntu fonts rasterized; no font binaries copied.'}
    (args.out/'capture-media-manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('capture-interface.png and capture-media-manifest.json ready',flush=True)


if __name__=='__main__':main()
