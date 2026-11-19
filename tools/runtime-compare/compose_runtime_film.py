"""Assemble the measured review from verified micro-clips and standalone charts.

All action clips retain 1x chronological playback. Additional chapter time is
an explicitly held end frame, never a delay inserted into an application's run.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[2]
MEDIA=ROOT/'media/runtime'
WORK=ROOT/'build/runtime-media/film'
NARRATION=ROOT/'build/runtime-media/narration'


def run(command):subprocess.run(command,check=True,creationflags=0x08004000)

def duration(path):return float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration','-of','csv=p=0',str(path)],text=True).strip())

def stamp(t):
    ms=round(t*1000)
    return f'{ms//3600000:02}:{ms//60000%60:02}:{ms//1000%60:02},{ms%1000:03}'


def main():
    p=argparse.ArgumentParser();p.add_argument('--check',action='store_true');a=p.parse_args()
    # The individual tool clips remain separately linked for detailed review.
    plan=[
        ('intro','runtime-poster.png','intro',7),
        ('capture-interface','capture-interface.png',None,4),
        ('region-key','hotkey-region.mp4','region',6),
        ('png-key','hotkey-png.mp4','png',6),
        ('region-drawing','editor-region-freehand.mp4','pencil',6),
        ('arrow','editor-arrow.mp4','arrow',6),
        ('blur','editor-blur.mp4','tools',7),
        ('zoom','editor-zoom.mp4',None,5),
        ('undo','editor-undo.mp4',None,5),
        ('text','editor-text.mp4',None,4),
        ('pixelate','editor-pixelate.mp4',None,5),
        ('colorpicker','editor-colorpicker.mp4',None,5),
        ('capture-results','capture-latency.png','capture_data',9),
        ('capture-traces','capture-traces.png',None,6),
        ('recording','recording-profile.mp4','recording',8),
        ('recording-resources','recording-resources.png','resources',10),
        ('outro','runtime-poster.png','outro',7)
    ]
    missing=[name for _,name,_,_ in plan if not (MEDIA/name).exists()]
    if missing:
        print('Pending validated media:',', '.join(missing));return
    voices={x['name']:x for x in json.loads((NARRATION/'manifest.json').read_text(encoding='utf-8'))['clips']}
    if a.check:
        print('All review sources and narration clips exist');return
    WORK.mkdir(parents=True,exist_ok=True)
    segments=[];chapters=[];audio=[];cues=[];clock=0
    for index,(name,file,voice,minimum) in enumerate(plan):
        source=MEDIA/file
        moving=source.suffix.lower() not in ['.png','.jpg']
        source_duration=duration(source) if moving else None
        length=max(minimum,(voices[voice]['duration_seconds']+1.0) if voice else minimum,source_duration or 0)
        length=math.ceil(length*30)/30
        segment=WORK/f'{index:02}-{name}.mp4'
        command=['ffmpeg','-v','error','-y']
        if source.suffix.lower() in ['.png','.jpg']:
            command+=['-loop','1','-framerate','30','-i',str(source)]
            filters='scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:color=0x13161d,setsar=1,fps=30'
        else:
            command+=['-i',str(source)]
            filters=f'setpts=PTS-STARTPTS,scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:color=0x13161d,setsar=1,fps=30,tpad=stop_mode=clone:stop_duration={length}'
            if length>source_duration+.05:
                filters+=f",drawtext=fontfile='C\\:/Windows/Fonts/Ubuntu-Regular.ttf':text='RESULT HELD':fontsize=17:fontcolor=0xb8bdc5:x=w-tw-24:y=h-th-9:box=1:boxcolor=0x13161d:boxborderw=4:enable='gte(t,{source_duration})'"
        # Transitions occur before the action's input marker or after its held result.
        filters+=f',fade=t=in:st=0:d=0.12,fade=t=out:st={length-.12}:d=0.12'
        command+=['-vf',filters,'-frames:v',str(round(length*30)),'-an','-c:v','libx264','-preset','fast','-crf','20','-threads','4','-g','60','-pix_fmt','yuv420p',str(segment)]
        run(command);segments.append(segment)
        chapters.append({'name':name,'start_seconds':clock,'duration_seconds':length,'source':file,'source_duration_seconds':source_duration,'extra_end_hold_seconds':max(0,length-source_duration) if moving else 0,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'narration':voice})
        if voice:
            offset=clock+.35;audio.append((NARRATION/voices[voice]['file'],offset))
            sentences=re.split(r'(?<=[.!?])\s+',voices[voice]['text']);total=sum(len(s) for s in sentences);t=offset
            for sentence in sentences:
                end=t+voices[voice]['duration_seconds']*len(sentence)/total;cues.append((t,end,sentence));t=end
        clock+=length
        print(f'Chapter {index+1}/{len(plan)} {name} complete',flush=True)
    concat=WORK/'segments.txt'
    concat.write_text(''.join("file '"+str(path).replace('\\','/')+"'\n" for path in segments),encoding='utf-8')
    silent=WORK/'runtime-silent.mp4'
    run(['ffmpeg','-v','error','-y','-f','concat','-safe','0','-i',str(concat),'-c','copy',str(silent)])
    metadata=WORK/'chapters.ffmeta'
    metadata.write_text(';FFMETADATA1\ntitle=Zipshot and ShareX runtime comparison\ncomment=Actual isolated Windows recordings. Scripted guest input. Source data and scoped measurements accompany this film.\n'+''.join(
        f"[CHAPTER]\nTIMEBASE=1/1000\nSTART={round(c['start_seconds']*1000)}\nEND={round((c['start_seconds']+c['duration_seconds'])*1000)}\ntitle={c['name'].replace('-',' ').title()}\n" for c in chapters),encoding='utf-8')
    command=['ffmpeg','-v','error','-y','-i',str(silent),'-stream_loop','-1','-i',str(ROOT/'build/original-music.wav')]
    for file,_ in audio:command+=['-i',str(file)]
    command+=['-f','ffmetadata','-i',str(metadata)]
    filters=['[1:a]volume=0.5[bed]'];streams=['[bed]']
    for i,(_,offset) in enumerate(audio,2):
        filters.append(f'[{i}:a]aresample=48000,volume=1.1,adelay={round(offset*1000)}:all=1[v{i}]');streams.append(f'[v{i}]')
    filters.append(''.join(streams)+f'amix=inputs={len(streams)}:duration=longest:normalize=0,alimiter=limit=0.85[a]')
    target=MEDIA/'zipshot-runtime-comparison.mp4'
    command+=['-filter_complex',';'.join(filters),'-map','0:v','-map','[a]','-map_metadata',str(len(audio)+2),'-map_chapters',str(len(audio)+2),'-t',str(clock),'-c:v','copy','-c:a','aac','-b:a','192k','-movflags','+faststart',str(target)]
    run(command)
    (MEDIA/'runtime-ru.srt').write_text('\n'.join(f'{i}\n{stamp(start)} --> {stamp(end)}\n{text}\n' for i,(start,end,text) in enumerate(cues,1)),encoding='utf-8')
    (MEDIA/'film-manifest.json').write_text(json.dumps({'duration_seconds':clock,'width':1920,'height':1080,'output_fps':30,'language':'ru','chapters':chapters,
        'time_policy':'Action footage is 1x. End frames may be held to provide narration time. There is no invented pre-response delay. Source measurements and sampling uncertainty remain in each micro-clip manifest.',
        'caption_timing':'Script sentence durations proportioned to actual narration clips, not forced-aligned transcription.',
        'narration_manifest':json.loads((NARRATION/'manifest.json').read_text(encoding='utf-8')),
        'audio_sources':[{'file':file.name,'offset_seconds':offset,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()} for file,offset in audio],
        'original_music_sha256':hashlib.sha256((ROOT/'build/original-music.wav').read_bytes()).hexdigest(),
        'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'sha256':hashlib.sha256(target.read_bytes()).hexdigest()},indent=2)+'\n',encoding='utf-8')
    print(target,clock,flush=True)


if __name__=='__main__':main()
