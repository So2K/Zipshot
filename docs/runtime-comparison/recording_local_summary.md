# Local-image recording results

Canonical local-executable software recording in an isolated Windows Sandbox. No host user desktop was recorded.

All repeated observations are retained. Values below are median [min-max]. CPU seconds include the product parent and descendants over the whole product lifecycle. Memory is the peak concurrent sampled process tree.

| Case and profile | n | Encoded seconds | CPU seconds | Peak private MiB | Peak working set MiB | Whole lifecycle seconds |
|---|---:|---:|---:|---:|---:|---:|
| zipshot-video30 \| controlled veryfast CRF23 | 2 | 27.37 [27.37-27.37] | 15.91 [15.72-16.09] | 1096.38 [1096.38-1096.39] | 898.54 [898.44-898.65] | 30.96 [30.88-31.04] |
| zipshot-gif15 \| factory GIF preset, full palette / sierra2_4a; cursor off | 2 | 28.67 [28.40-28.94] | 31.64 [30.81-32.47] | 890.84 [890.80-890.87] | 634.76 [634.73-634.79] | 47.22 [47.11-47.32] |
| sharex22-video30-native-software-allowed \| native software allowed, 10000 kbps configured | 3 | 30.00 [30.00-30.00] | 16.59 [16.03-40.00] | 274.84 [273.67-293.05] | 285.79 [285.44-290.54] | 34.34 [34.31-43.23] |
| sharex22-gif15-native-hardware \| native adaptive GIF | 2 | 30.00 [30.00-30.00] | 42.12 [38.34-45.91] | 206.20 [202.98-209.41] | 284.52 [281.38-287.65] | 65.70 [62.43-68.98] |
| zipshot-video30 \| factory software preset, X264Preset omitted | 1 | 28.40 [28.40-28.40] | 16.92 [16.92-16.92] | 606.64 [606.64-606.64] | 478.96 [478.96-478.96] | 30.68 [30.68-30.68] |

The factory row is a single exploratory trial. It used a successful DDA fallback variant, while the controlled MP4 trials used GDI. Do not attribute their difference solely to the preset.

- Both products use 1920x1080, nominal video 30fps or GIF 15fps, cursor/audio disabled.
- Codecs, quality controls, decoded duration and visible fixture cadence differ. These are not equal-quality comparisons.
- Zipshot factory fallback uses ultrafast; the separate controlled software profile uses veryfast CRF23.
- Current ShareX native hardware requirement fails in this guest; MP4 trials explicitly allow software encoding.
- Keep every repetition. Current native MP4 includes one 40 CPU-second / 16-drop outlier alongside two approximately 16 CPU-second / zero-drop runs.
- 30Hz fixture state count is not a promise of 60 distinct visual frames. Adaptive GIF may coalesce identical frames.
- The native hardware flag describes encoder acceleration, not total GPU use. The GIF hardware-required profile label does not make its adaptive GIF encoder hardware accelerated.
- No GPU samples could be attributed to the recording process IDs. GPU load is not measured.
- Earlier mapped-input runs include a confirmed VM image-launch transport artifact and are retained as diagnostics, not primary startup comparisons.

Exact per-trial results, media SHA-256, codec settings and input summary hashes are in [recording_local_summary.json](recording_local_summary.json). Machine-readable table data is in [recording_local_summary.csv](recording_local_summary.csv). Raw CPU/RAM curves and their exact CSV samples are under [recording_curves/](recording_curves/).
