"""Verify alternating zoom/undo states from actual decoded pixels, without UI timing claims."""
import argparse
import json
import subprocess
from pathlib import Path
import numpy as np


def verify(folder):
    record = json.loads((folder / 'editor-interaction-result.json').read_text(encoding='utf-8-sig'))
    video = record['video']
    width, height = video['capture_crop'][2:]
    anchor = record['clock_before_input']
    anchor_qpc = (anchor['qpc_before'] + anchor['qpc_after']) / 2
    anchor_utc = anchor['filetime_utc_100ns'] / 1e7 - 11644473600
    utc = lambda q: anchor_utc + (q - anchor_qpc) / record['qpc_frequency']
    phase = record['phases'][-1]
    start, end = utc(phase['qpc_start']), utc(phase['qpc_end'])
    times = [float(frame['best_effort_timestamp_time']) for frame in json.loads((folder / 'video-frames.json').read_text())['frames']]
    decoder = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', str(folder / 'actual-ui.mkv'), '-fps_mode', 'passthrough', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-threads', '1', 'pipe:1'], stdout=subprocess.PIPE)
    observations = []
    for timestamp in times:
        data = decoder.stdout.read(width * height * 3)
        if len(data) != width * height * 3:
            raise RuntimeError('Actual frame count and decoded bytes disagree')
        frame = np.frombuffer(data, np.uint8).reshape(height, width, 3)
        roi = frame[180:min(height-100, 790), 80:min(width-80, 1200)]
        if record['case'] == 'zoom':
            mask = (roi[:, :, 0] > 225) & (roi[:, :, 0] < 244) & (roi[:, :, 1] > 230) & (roi[:, :, 1] < 248) & (roi[:, :, 2] > 235) & (roi[:, :, 2] < 253)
        else:
            mask = (roi[:, :, 0] > 170) & (roi[:, :, 1] < 100) & (roi[:, :, 2] < 100)
        if start-.05 <= timestamp <= end+.1:
            observations.append(dict(video_offset_s=timestamp-video['first_pts'], pixel_count=int(mask.sum())))
    decoder.stdout.close()
    if decoder.wait() != 0:
        raise RuntimeError('Actual clip decode failed')
    counts = np.array([row['pixel_count'] for row in observations])
    threshold = (counts.min() + counts.max()) / 2
    states = counts > threshold
    result = dict(scope='Actual decoded pixels during 30 alternating input actions. Visual state verification only, not latency or UI frame rate.', case=record['case'], frames=len(observations), pixel_count_min=int(counts.min()), pixel_count_max=int(counts.max()), thresholded_state_transitions=int(np.count_nonzero(states[1:] != states[:-1])), observations=observations)
    (folder / 'editor-feature-pixel-check.json').write_text(json.dumps(result, indent=2)+'\n')
    print(folder.name, {key: value for key, value in result.items() if key != 'observations'}, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    for folder in sorted(args.root.iterdir()):
        if 'zoom-' in folder.name or 'undo-' in folder.name:
            verify(folder)
