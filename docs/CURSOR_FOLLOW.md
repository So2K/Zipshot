# Cursor focus for screen recordings

For MP4 recordings, the recording bar has a Follow button. Turn it on before starting or during a recording. Ctrl+Alt+F8 toggles it while that recording bar is open, when the shortcut is available. The button shows the active state.

Zipshot records the selected region normally. After stopping, it keeps the original and makes a separate `_focus.mp4` copy for intervals where Follow was enabled. The processed copy uses a smooth 1.6× camera zoom and follows the pointer within the recorded region. Small pointer movements do not constantly shift the camera. Turning Follow off eases back to the full frame.

New MP4 recordings also save an adjacent `.cursor.json` file, even when Follow is off. Right-click the original recording in the gallery and choose the cursor-follow action to make a focused copy afterward. Keep that sidecar beside the original video. Imported or older videos without a recorded cursor track cannot use this action.

Cursor positions are sampled only during an active MP4 recording. Pauses are excluded from its time base. Hidden pointers or positions outside the chosen region cause the camera to hold briefly and then return to the full frame. It never reveals pixels outside the recorded region or changes the real desktop zoom or mouse movement.

Processing runs in the background and requires FFmpeg. Source audio is retained when present; Zipshot does not add narration or music. Source files and existing destination files are never overwritten. If processing fails, the original recording remains available.

The track uses the recorder's active timer rather than encoder presentation timestamps. Encoder startup and fallback can introduce a small alignment difference. This is a presentation effect rather than a physical cursor-latency measurement.

The design follows the established pattern of a separate cursor track, a smoothed bounded camera and an untouched original, as demonstrated by [Screen Studio](https://screen.studio/) and [Cap](https://cap.so/). The motion implementation uses independent critically damped spring mathematics and FFmpeg's documented runtime crop commands. No third-party zoom-engine source was copied.

## Validation

The camera and recorder tests use synthetic cursor positions, clocks and process boundaries. The gallery tests render a real hidden native window with a fake clipboard. The video integration suite creates its own moving fixture and checks actual FFmpeg output, source hashes, frame count, duration, cropping and preserved audio packets without playback.

```cmd
tests\run_cursor_follow_tests.bat
tests\run_recorder_cursor_tests.bat
tests\run_mainwindow_gallery_tests.bat
tests\run_cursor_video_tests.bat
```

The last command requires FFmpeg on PATH. Production processing also works with FFmpeg alone when FFprobe is unavailable.
