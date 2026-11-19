"""Package real full-application Ctrl+PrintScreen drawing evidence separately from editor timings."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def annotation_counts(path):
    with Image.open(path) as image:
        array = np.asarray(image.convert('RGB'))
        width, height = image.size
    roi = array[450:1150, 800:2050]
    red = (roi[:, :, 0] > 170) & (roi[:, :, 1] < 100) & (roi[:, :, 2] < 100)
    green = (roi[:, :, 1] > 170) & (roi[:, :, 0] < 100) & (roi[:, :, 2] < 100)
    return dict(dimensions=[width, height], red_pixels=int(red.sum()), green_pixels=int(green.sum()), sha256=digest(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=Path('D:/ZipshotRuntimeCompare-20261118/session02/output'))
    parser.add_argument('--docs', type=Path, default=Path(__file__).resolve().parents[2]/'docs/runtime-comparison')
    args = parser.parse_args()
    runs = []
    for path in sorted((args.output/'editor-region').glob('*/editor-region-result.json')):
        record = json.loads(path.read_text(encoding='utf-8-sig'))
        if record['status'] != 'captured_pending_visual_and_frame_analysis':
            continue
        folder = path.parent
        video = record['video']
        if digest(folder/'actual-ui.mkv').lower() != video['sha256'].lower():
            raise RuntimeError('Original real video hash mismatch')
        anchor = record['clock_before_input']
        qpc = (anchor['qpc_before']+anchor['qpc_after'])/2
        utc = anchor['filetime_utc_100ns']/1e7-11644473600
        frequency = record['qpc_frequency']
        offset = lambda tick: utc+(tick-qpc)/frequency-video['first_pts']
        events = record['injected_events']
        phase = record['phases'][0]
        tool = next(event for event in events if event['phase'] == 'select_tool' and event['kind'] == 'key' and event['down'])
        hotkey = next(event for event in events if event['phase'] == 'region_hotkey' and event['kind'] == 'key' and event['code'] == 44 and event['down'])
        down = next(event for event in events if event['phase'] == phase['name'] and event['kind'] == 'button' and event['down'])
        moves = [event for event in events if event['kind'] == 'move' and event['phase'] == phase['name']]
        before = annotation_counts(folder/'overlay-before.png')
        after = annotation_counts(folder/'overlay-after.png')
        channel = 'green_pixels' if record['engine'] == 'native' else 'red_pixels'
        if after[channel]-before[channel] < 1000:
            raise RuntimeError('Actual region annotation is not present in the expected common drawing area')
        saved = [dict(file=str(file.relative_to(folder)).replace('\\', '/'), **annotation_counts(file)) for file in (folder/'saved').rglob('*.png')]
        runs.append(dict(engine=record['engine'], case=record['case'], source_folder=str(folder.relative_to(args.output)).replace('\\', '/'), source_video='actual-ui.mkv', raw_report='editor-region-result.json', application=record['application'], overlay=record['overlay'], video=video, real_duration_s=video['last_pts']-video['first_pts'], offsets=dict(region_hotkey_s=offset(hotkey['qpc_before']), tool_key_s=offset(tool['qpc_before']), stroke_phase_s=offset(phase['qpc_start']), actual_button_down_s=offset(down['qpc_before']), first_move_s=offset(moves[0]['qpc_before']), last_move_s=offset(moves[-1]['qpc_before'])), phases=[dict(name=phase['name'], kind=phase['kind'], video_offset_s=offset(phase['qpc_start']), duration_s=(phase['qpc_end']-phase['qpc_start'])/frequency)], recorded_input_overlay=[dict(seconds=offset(event['qpc_before']), x=event['x'], y=event['y'], index=event['index'], phase=event['phase'], label='Recorded input position') for event in moves], before=before, after=after, saved_outputs=saved, save_verified=bool(saved) and all(output[channel] > 1000 for output in saved), scope='Actual unmodified full application. Ctrl+PrintScreen region overlay then A or F and the same120 screen-coordinate points over2seconds. Default annotation style differs. Not a quality-matched latency or UI-frame-rate benchmark.'))
    staged_driver = args.output.parent/'input/editor_region_interaction_r1.ps1'
    driver = dict(file=staged_driver.name, sha256=digest(staged_driver), note='Original run driver. Current repository script only clarifies unused fixture provenance in the report header; the recording and input paths are unchanged.') if staged_driver.is_file() else None
    manifest = dict(schema_version=1, driver=driver, source='Genuine disposable Windows Sandbox region overlays over its public/default guest desktop. No PNG fixture is loaded; an unused common-wrapper fixture field in raw reports is not source-content provenance.', speed='Real time1x. Original epoch PTS must be normalized with setpts=PTS-STARTPTS, never interpreted as container duration.', cursor='The GDI observer draws the actual guest cursor. Additional rings, if any, must be labelled Recorded input position.', limitation='Whole-display observer load differs, observed17.7–22.7fps. One gesture per app/tool, application defaults. No input-to-visible or speed ranking from these clips. ShareX Space did not produce a PNG in the task save directory, so only its visible drawing is verified.', runs=runs)
    args.docs.mkdir(parents=True, exist_ok=True)
    target = args.docs/'editor_region_runs.json'
    target.write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(runs=len(runs), manifest=str(target), folders=[run['source_folder'] for run in runs]), indent=2))


if __name__ == '__main__':
    main()
