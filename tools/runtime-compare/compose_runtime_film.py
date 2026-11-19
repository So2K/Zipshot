"""Join actual runtime microclips into a silent, action-only montage.

Every video source remains in order at 1x. A four-second capture-result chart
is the only added still. There is no narration, music or subtitle track.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MEDIA = ROOT / 'media/runtime'
WORK = ROOT / 'build/runtime-media/silent-montage'
SOURCES = [
    ('Region hotkey', 'hotkey-region.mp4'),
    ('PrintScreen to PNG', 'hotkey-png.mp4'),
    ('Capture results', 'capture-latency.png'),
    ('Region freehand', 'editor-region-freehand.mp4'),
    ('Region arrow', 'editor-region-arrow.mp4'),
    ('Editor arrow', 'editor-arrow.mp4'),
    ('Editor freehand', 'editor-freehand.mp4'),
    ('Blur', 'editor-blur.mp4'),
    ('Zoom', 'editor-zoom.mp4'),
    ('Undo and redo', 'editor-undo.mp4'),
    ('Text', 'editor-text.mp4'),
    ('Pixelate', 'editor-pixelate.mp4'),
    ('Color picker', 'editor-colorpicker.mp4'),
    ('Recording output', 'recording-profile.mp4'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def probe(path):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-show_entries',
        'stream=codec_type,width,height,r_frame_rate,nb_frames,duration',
        '-of', 'json', str(path)
    ], text=True))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    chapters = []
    clip_paths = []
    cursor = 0
    for title, name in SOURCES:
        source = MEDIA / name
        chart = source.suffix.lower() == '.png'
        if chart:
            frames = 120
            clip_path = WORK / 'capture-results.mp4'
            if not args.check:
                subprocess.run([
                    'ffmpeg', '-v', 'error', '-y', '-loop', '1', '-framerate', '30', '-i', str(source),
                    '-vf', 'scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:color=0x13161d,setsar=1',
                    '-frames:v', str(frames), '-an', '-c:v', 'libx264', '-preset', 'fast', '-crf', '18',
                    '-threads', '4', '-pix_fmt', 'yuv420p', str(clip_path)
                ], check=True, creationflags=0x08004000)
        else:
            video = next(s for s in probe(source)['streams'] if s['codec_type'] == 'video')
            if (video['width'], video['height'], video['r_frame_rate']) != (1920, 1080, '30/1'):
                raise RuntimeError('Unexpected source format ' + name)
            frames = int(video['nb_frames'])
            clip_path = source
        clip_paths.append(clip_path)
        chapters.append({
            'name': title, 'source': name, 'source_sha256': sha(source),
            'source_kind': 'static_chart' if chart else 'video',
            'start_frame': cursor, 'frame_count': frames,
            'start_seconds': cursor / 30, 'duration_seconds': frames / 30,
            'extra_end_hold_seconds': 0,
        })
        cursor += frames
    if args.check:
        print(json.dumps({'clips': len(chapters), 'frames': cursor, 'duration_seconds': cursor / 30}))
        return
    WORK.mkdir(parents=True, exist_ok=True)
    inputs = WORK / 'clips.txt'
    inputs.write_text(''.join("file '" + path.as_posix() + "'\n" for path in clip_paths), encoding='utf-8')
    metadata = WORK / 'chapters.ffmeta'
    metadata.write_text(
        ';FFMETADATA1\ntitle=Zipshot and ShareX silent runtime montage\n' + ''.join(
            f"[CHAPTER]\nTIMEBASE=1/30\nSTART={c['start_frame']}\nEND={c['start_frame'] + c['frame_count']}\ntitle={c['name']}\n"
            for c in chapters
        ), encoding='utf-8'
    )
    target = MEDIA / 'zipshot-runtime-comparison.mp4'
    subprocess.run([
        'ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(inputs),
        '-f', 'ffmetadata', '-i', str(metadata), '-map', '0:v:0', '-an', '-sn',
        '-map_metadata', '1', '-map_chapters', '1', '-c:v', 'libx264',
        '-preset', 'fast', '-crf', '18', '-threads', '4', '-pix_fmt', 'yuv420p',
        '-fps_mode', 'passthrough', '-movflags', '+faststart', str(target)
    ], check=True, creationflags=0x08004000)
    streams = probe(target)['streams']
    if any(s['codec_type'] in ('audio', 'subtitle') for s in streams):
        raise RuntimeError('Montage unexpectedly contains audio or subtitles')
    if int(next(s for s in streams if s['codec_type'] == 'video')['nb_frames']) != cursor:
        raise RuntimeError('Montage frame count changed')
    manifest = {
        'schema_version': 2, 'duration_seconds': cursor / 30, 'frame_count': cursor,
        'width': 1920, 'height': 1080, 'output_fps': 30,
        'audio_streams': 0, 'subtitle_streams': 0, 'chapters': chapters,
        'time_policy': 'All 13 complete microclips are joined at original 1x chronology. One measured capture chart appears for exactly four seconds. No extra intro, outro or video holds are added. Source labels and recorded endpoint clocks remain unchanged.',
        'audio_policy': 'No voice, music or other audio. No subtitle track.',
        'generator_sha256': sha(Path(__file__)), 'sha256': sha(target),
    }
    (MEDIA / 'film-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'file': str(target), 'seconds': cursor / 30, 'frames': cursor, 'sha256': manifest['sha256']}), flush=True)


if __name__ == '__main__':
    main()
