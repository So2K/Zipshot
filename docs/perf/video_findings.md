# Cursor-video performance controls

The automatic focus-copy path now hashes its source once, then keeps that verified source open against writes and deletion until processing finishes. The saved track stays in memory. Manual application still opens and hashes the complete source against its sidecar. Encoder settings, crop commands, resolution, frame cadence and audio copying are unchanged.

On a synthetic large-file control, the actual compatible `SaveTrack` → `LoadTrack` → `Render` path took a median **468.51 ms**, versus **366.61 ms** for `SaveTrackPrepared` → `RenderPrepared`, a **21.75% reduction**. Four counterbalanced repetitions per path produced identical complete decoded frame hashes. This is a control of duplicate file hashing, not a prediction that every video becomes 21.75% faster.

| Actual production API path | Median | Observed p95, nearest rank |
|---|---:|---:|
| Compatible separate calls | 468.51 ms | 474.45 ms |
| Retained verified source and track | 366.61 ms | 378.28 ms |

The input is a valid 640×360, 30 fps, four-second/120-frame H.264 MP4 with a legal 128 MiB trailing ISO `free` box. Its total size is 136,568,578 bytes. Padding deliberately isolates file-byte hashing from additional encoding work. It does not represent ordinary video compression. Input SHA-256 is `c3b684d5fadebbf055be776aee818ea1f00dd26773d645462485e84690031272`; all eight output framemd5 files have SHA-256 `8863c7335329967e99debc7004a4e3c44c902c71e3002183d1e6dd3b0b041d87`. The input remained unchanged. An earlier benchmark-only retained-handle shim measured a 17.86% median reduction on the same fixture design before the production change.

## Controls that were not adopted

The table gives median FFmpeg render wall time, excluding metadata probes and source hashing. Each short control uses four seconds of synthetic `testsrc2`, three measured repetitions after one warm run. All short-control variants matched the baseline's full decoded framemd5.

| Render settings | 640×360, 30 fps | 1920×1080, 30 fps |
|---|---:|---:|
| Current filter thread 1, encoder threads 4 | 201.02 ms | 1322.65 ms |
| Filter threads 2 | 201.23 ms | 1374.87 ms |
| Filter threads 4 | 201.73 ms | 1356.30 ms |
| Explicit decoder thread 1 | 191.17 ms | 1365.86 ms |
| Explicit decoder threads 4 | 200.35 ms | 1345.18 ms |
| Send only changed crop scalar commands | 200.65 ms | 1376.58 ms |

These results do not justify changing thread counts or the command generator. In a separate 60-second/3600-frame 320×180 control, removing redundant scalar commands reduced 4684 commands to 1297, but median render time changed only from 1513.58 to 1486.30 ms. The four-repetition baseline itself produced different decoded hashes between identical runs. Before encoding, both command variants matched every filtered frame in two repetitions each, SHA-256 `46c0f3c18eaa8555e9a429d073e770ff76dac65dc2420736b2f87b8bae64a528`. That is filter-pixel equivalence, not a general encoded-output equivalence result. No command or aspect-ratio change was adopted.

Skipping ffprobe stream discovery reduced one 360p metadata control from 43.94 to 32.88 ms, but video-stream start time and duration became `N/A`. Format-level fallback is insufficient to establish every stream's timestamp offset. The shortcut was rejected. A direct cached 128 MiB SHA-256 control took a median 76.79 ms across five runs, supporting investigation of duplicate reads rather than weaker identity checks.

## Method and limits

The base was `1fdf0a7`; the actual API result records the exact modified source hashes. FFmpeg was the installed 8.1.2 full build, executable SHA-256 `ad8f211bc894755e0061c55ab280ae00e8d3d4f15a8cc4372b24cfa247b5942e`. Every measured helper/encoder used below-normal priority and a four-logical-processor affinity budget. Automatic decoder choices are therefore scoped to that budget. The affinity is applied immediately after process creation, so very early initialization is not guaranteed to be constrained. Only owned generated fixtures were used; there was no screen capture, system-cursor query, clipboard access or playback.

Raw direct-child controls retain exact command lines, whole-process CPU time, nominal 10 ms private-memory/working-set samples, medians, nearest-rank p95 and hashes. A sampled peak can miss shorter allocations. Four observations make nearest-rank p95 the observed maximum, not a population-tail estimate. The full API comparison reports wall time; its helper-process counters exclude FFmpeg child CPU/memory and must not be read as process-tree resource totals.

The focus renderer remains x264 `veryfast`, CRF 18, four encoder threads and one filter thread. Input EOF determines the processed duration; audio is stream-copied. The engine regression suite passes 35 checks, including source locking, one identity computation, move/failure invalidation, same-size tampering rejection in the manual path, full decoded equivalence, dimensions/frame cadence and byte-identical AAC packets. The recorder suite passes 35 checks using fake cursor/process APIs and hidden windows, including automatic prepared-API use without a sidecar reload. Its foreground window remained unchanged.

ShareX 22.4's native recorder and the older FFmpeg profiles use different encoding controls. These synthetic renderer controls do not establish a universal Zipshot/ShareX winner. The separate [actual recording comparison](../runtime-comparison/recording_results.md) retains those profile and environment limits.

## Reproduction and primary references

Build with `tools/perf/video_build.bat`. Run `python tools/perf/video_profile.py --reps 3`, `python tools/perf/video_long_control.py`, and `python tools/perf/video_exact_control.py` for the exploratory controls. Run `python tools/perf/video_prepared_control.py --actual` for the actual API comparison. Prepared-control reruns create a new owned artifact directory. Generated binaries and fixtures stay under ignored `build/perf/video`.

Local raw reports are `build/perf/video/video-profile.json`, `video-long-control.json`, `video-exact-control.json`, `video-pipeline.json`, `video-prepared-control.json` and `video-prepared-actual.json`. [The compact result data](video_results.json) retains the measured values and source/decoded hashes.

FFmpeg documents [runtime filter commands](https://ffmpeg.org/ffmpeg-filters.html#sendcmd_002c-asendcmd), [crop command parameters](https://ffmpeg.org/ffmpeg-filters.html#crop) and [stream copying](https://ffmpeg.org/ffmpeg.html#Streamcopy). Its [ffprobe implementation](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/fftools/ffprobe.c) separates selected display fields from stream discovery. The [sendcmd implementation](https://github.com/FFmpeg/FFmpeg/blob/n8.1.2/libavfilter/f_sendcmd.c) visits command intervals for each frame; removing redundant scalar commands does not remove that interval traversal. None of these source observations was substituted for measured timing or output validation.
