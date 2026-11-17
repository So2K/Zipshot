# Comparison video

`zipshot-vs-sharex.mp4` — 1920×1080, 30 fps, 68 seconds, H.264/AAC.
Russian narration, original synthesized music, and a separate `narration.srt`.

The comparison measures **warm capture API latency only** against the installed
ShareX 18.0.1. Its chart is driven by `docs/benchmark/capture-results.json`.
The editor footage shows actual offscreen Zipshot renders from the original
`fixture.png`. Actions are scripted and presented at an explanatory pace; this
sequence does not measure interaction speed. No private desktop content appears.

Chapters: 0:00 introduction, 0:08 protocol, 0:16 measurements,
0:29 real editor, 0:48 code checks, 0:58 repository.

To reproduce on Windows with Python, Pillow, NumPy, gTTS and FFmpeg:

```bat
python tools/make_video.py --fixture
build\zipshot.exe --test-demo media/fixture.png media/demo
python tools/make_narration.py
python tools/make_video.py
```

Narration generation submits only the public text in `make_narration.py` to
Google Translate TTS through gTTS. Rendered editor frames, music generation and
video assembly are local. Captions use estimated sentence timing. Existing audio
clips are included so watching or rebuilding the video needs no TTS request.

`qa/` contains selected static frames and the sanitized outcome of an optional
Jev check. That service returned HTTP 402 and supplied no judgment. The frames
were inspected directly; no automated visual-score success is claimed.
