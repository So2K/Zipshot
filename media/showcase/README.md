# Launch artwork

The launch layout uses Ubuntu, the existing Zipshot icon and its blue accent.
All application pixels come from the documented editor renders in
`media/page`. Interface controls are not generated or redrawn. The capture
region and keyboard illustrations are diagrams of the existing workflow.

The desktop and mobile covers are composed independently. GitHub selects the
appropriate image through `<picture>` at 640 CSS pixels. The feature panel and
benchmark have mobile variants for readable text without a wide table layout.

`workflow.png` is an APNG made from four genuine scripted editor states,
cropped and enlarged to show the tools and annotations. State durations are
1.6, 1.4, 1.4 and 3.0 seconds. Its playback is not a latency measurement.

The numerical figure reads the recorded benchmark JSON. Cover facts refer to
the published 1.0.1 Windows x64 release. The executable is 1,912,320 bytes,
rounded to 1.9 decimal MB. FFmpeg and sticker packs are separate dependencies.

`social-card.png` is a 1280×640 cover available for social posting or a custom
repository preview. Merely storing it here does not change GitHub's repository
Open Graph image setting.

Regenerate with Pillow and locally installed Ubuntu fonts.

```bat
python tools\render_launch_showcase.py
python tools\render_benchmark_mobile.py
```

Font binaries are neither copied nor embedded. The JSON manifests preserve
source paths, measured values, geometry, image hashes and text bounds.
