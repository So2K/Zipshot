# Recording evidence and limitations

The baseline executable remains Zipshot 1.0.1, SHA-256
`0730b10f1f00295e32f603feb3c482fe2aa66da08b644bb5595592b0ef1ac783`.
No production changes were made to obtain these results.

The isolated Windows Sandbox has a remote display and a virtualized RTX 4070 Ti.
FFmpeg NVENC cannot load `nvcuda.dll`, and FFmpeg Desktop Duplication repeatedly
failed with `AcquireNextFrame failed: 887a0026`. Real GDI recording works. A later
factory-preset trial successfully used the DDA software-encoder variant after an
initial failure, so DDA is intermittent rather than categorically unavailable.
The actual successful backend is retained per case. These restrictions
are not claims about recording on the user's normal physical Windows desktop.

Zipshot's automatic fallback produces real MP4 and GIF files. In the initial
30-second request, its MP4 files contained approximately 22.2 seconds and its GIF
25.8 seconds. Initial unsupported capture attempts consume part of the timed
self-test. The successful capture interval is analyzed separately from startup,
failed attempts, conversion and shutdown. A three-second self-test also returned
exit code zero without producing a media file. This is a verified diagnostic
false positive, not a successful recording test. The validation tools check the
actual files rather than relying on that exit code.

The installed ShareX 18.0.1 legacy recorder works with the same FFmpeg binary and
GDI capture. Its unsupported DDA attempts were retained as failed environment
probes and are not used to rank performance. The matched legacy video profile
uses x264 veryfast, CRF 23, audio off, cursor off and 1920 x 1080.

The current official ShareX 22.4.0 has a separate native recording pipeline. Its
default requirement for D3D11 hardware encoding fails inside this Sandbox.
Changing the supported RequireHardwareEncoder preference to allow software
encoding produces valid native H.264 MP4. Native GIF also works. This is an
explicit software-allowed profile, not a successful default-hardware test.

The current native MP4 producer reports its own frame and drop counts. The
initial three-second test reported 90 video frames and zero dropped frames, and
all 90 frames decoded. The encoder's friendly name was empty; the product
reported hardware acceleration false. No encoder name is invented here.

The native adaptive GIF producer can emit fewer GIF frames than its recording
frame count by coalescing identical frames. Its first three-second test reported
45 producer frames, zero producer drops, and a valid 32-frame GIF lasting three
seconds. A smaller decoded GIF frame count alone is not evidence of dropped
recording frames.

Every accepted clip is fully decoded and checked against the moving fixture's
binary tick. The fixture changes at 30 Hz. It cannot establish 60 distinct visual
updates per second. Encoder frame count, distinct fixture ticks and encoded
duration are retained separately. The Sandbox remote display introduces repeated
or skipped fixture presentations, so these are scoped observations, not a normal
desktop 60 fps guarantee.

Recording memory includes FFmpeg children. In the first long run, the Zipshot
software FFmpeg tree peaked near 1.1 GiB for MP4 and 891 MiB for GIF. The current
ShareX native software-allowed tree peaked near 274 MiB for MP4 and 244 MiB for
GIF. These profiles use different encoders and quality controls. The data does
not support a blanket claim that Zipshot recording uses less memory.
The MP4 figure above is the explicitly controlled veryfast preset. Zipshot's
factory fallback preset is ultrafast; a separate trial omitted the X264Preset INI
key, verified the actual ultrafast command and embedded x264 settings, and peaked
at approximately 607 MiB. Its capture backend also differed, so the difference
must not be attributed to the preset alone. No video encoder thread limit was
changed in the product. A separate synthetic x264 control is an engineering
proposal, not a GIF memory result or an application performance claim.

GPU counters were queried inside the guest, but no samples could be attributed
to the owned recording process IDs. GPU load is not measured, rather than zero.
Private bytes and working set are peak concurrent sampled tree values, with the
product parent and children also retained separately.

An identical-binary `ffmpeg -version` control confirmed a major mapped-image
transport effect. Creating the 242,496,512-byte mapped executable took
2.66-2.78 seconds and 2.17-2.25 parent kernel CPU seconds on each attempt. Its
guest-local first load took 2.32 seconds; subsequent local creation took
1.2-1.4 milliseconds and zero observed parent kernel CPU ticks. This is an image
launch control, not an encoding benchmark. The earlier mapped-input recording
repetitions are retained as diagnostics. Canonical repeats use a hash-identical
guest-local FFmpeg initialized before the measured product lifecycle, and a
hash-checked guest-local official ShareX package when the host retains at least
1.2 GiB free. Mapped-image startup cost must not be attributed to app algorithms.

Raw nominal 200 ms samples retain their actual timestamps. Command-line CIM
lookup can lengthen some initial sampling intervals; the analysis reports the
actual median and maximum interval. First capture-child observation is derived
from the process snapshots, not the later completion of command-line lookup.

The `recording30_*.json` summaries, `recording_curves/` PNG and CSV files, and
original guest result files preserve the numerical evidence. A valid moving file
or a zero internal drop count does not establish equal codec quality, lower GPU
load, or a general performance win.
