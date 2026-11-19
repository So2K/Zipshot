# Recording comparison protocol

This protocol separates actual product recording from synthetic encoder checks.
No recording of the user's Windows desktop is permitted. Tests run only in a clean
Windows Sandbox session as `WDAGUtilityAccount`, using an original moving fixture.

## Scope and provenance

The initial harness targets the installed ShareX 18.0.1 as a legacy comparison.
It loads the untouched application assembly, starts its real `Program.Main` and
GUI, then calls its existing `TaskHelpers.StartScreenRecording` with a custom
region. The helper controls the application; it does not implement capture or
encoding. Installed files and settings remain untouched. The original assembly,
release Zipshot executable, helper and shared FFmpeg binaries are hashed.

ShareX 22.4.0 is the current stable version found during this task. Its defaults
and native recording pipeline require a separate current-version test. Findings
about the legacy FFmpeg recorder must not be presented as findings about 22.4.0.

## Profiles

- Video uses 1920 x 1080 at 30 or 60 fps, audio and cursor disabled.
- A two-frame `lavfi color` probe checks whether NVENC initializes in the guest.
  This is a capability check, not a recording benchmark.
- If NVENC works, Zipshot's actual preferred path is compared with ShareX's
  supported custom-command path using NVENC `p4`, `ll`, `vbr`, CQ 23, `b:v 0`.
- Otherwise, both use the available capture path and x264 `veryfast`, CRF 23,
  yuv420p. The actual FFmpeg child commands must establish that a match occurred.
- Zipshot 1.0.1 has no encoder/backend selector. A forced x264 comparison is not
  run when its native NVENC path works. No artificial encoder failure is induced.
- GIF uses 1920 x 1080 at 15 fps, full palette statistics and `sierra2_4a` dither.
  Zipshot uses a lossless RGB intermediate and two-pass palette generation;
  ShareX 18.0.1 uses its own lossless x264 and palette pipeline. Matching output
  settings do not make these intermediate pipelines identical.

## Sampling and validation

The standard clip request is 30 seconds. CPU sampling retains process handles
through exit and separates parent CPU from FFmpeg descendants. Samples every
approximately 200 ms are deep snapshots. Concurrent tree private bytes and
working set sum only alive processes in the same sample; individual process
peak working sets are not added together. Very short children missed between
samples remain a measurement limitation.

The steady capture interval starts two seconds after the successful capture
FFmpeg child is first observed. Report its actual sampled duration. Whole-process
CPU and elapsed time also include startup, failed fallback attempts, conversion
and shutdown. GIF conversion costs must be distinguished from capture costs.

Media and measurement logs are written to the guest local work drive. Completed
cases are copied to the host's task output mapping after sampling has closed.
This keeps mapped-folder write performance out of the active encoding interval.

Per-process GPU Engine counters are attempted inside the guest. Retain raw
per-engine samples and filter to owned process IDs. Do not combine separate GPU
engines into one physical GPU utilization percentage. Missing counter data means
not measured, rather than zero GPU load.

`ffprobe -count_frames` checks codec, dimensions, frame rate, duration and decoded
frame count. The fixture includes a 16-bit 30 Hz binary tick; decoding this tick
can reveal repeated or skipped fixture presentations. At 60 fps repeated ticks
are expected, and at 15 fps tick jumps of two are expected. These observations
are not the encoder's internal dropped-frame counter. The helper does not claim
zero dropped frames from a successful file decode alone.

Use `recording_build.ps1` to stage inputs, `recording_guest.ps1` only in the clean
guest, and `recording_analyze.py --decode-ticks` on the resulting task files.
Until actual files and samples are validated, this document establishes a method,
not a measured recording-speed or recording-load advantage.
