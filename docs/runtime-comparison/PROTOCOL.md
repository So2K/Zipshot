# Interactive comparison protocol

This work measures actual workflows separately from the older warm capture API
microbenchmark. Results are not interchangeable. The current official ShareX
release is tested as a separate portable app; the installed 18.0.1 build is a
legacy baseline. Neither installed app nor its settings are changed.

## Isolation and evidence

Use a dedicated Windows Sandbox guest with a clean generated desktop, vGPU,
identical display size and scaling, and no personal data. No host mouse or
keyboard input is injected. Network, microphone, camera and clipboard
redirection are disabled. Only task-owned input/output directories are mapped.
Verify actual guest identity, rendered pixels, input delivery and encoded video
before recording a measurement. A PID or a successful launch is insufficient.

Record the guest OS, hardware and virtual GPU, display size, DPI, encoder build,
actual application versions and binary hashes. VM measurements are labeled as
VM measurements. They are not presented as native-host hardware results.

## Capture workflow endpoints

Measure these separately after a real synthetic key is delivered through the
guest Windows input queue.

| Trigger | Endpoint |
|---|---|
| Ctrl+PrtScn | First visible region-overlay content |
| PrtScn | Clipboard contains the captured image |
| PrtScn | Saved image is closed and decodes correctly |
| Capture then edit | First visible editor content |

Use matching after-capture tasks, cursor setting and output format. Record
the exact options. Exclude upload tasks. Distinguish startup/cold calls from
warm calls, alternate app order, and retain all valid runs plus failures.
Target at least 30 warm samples per workflow/app. Report median and p95, the
sampling interval and video frame-rate uncertainty. Never claim sub-frame
video precision or a synthetic callback latency as an app's response latency.

## Editor operations

Use the exact same generated 1920×1080 and 3840×2160 PNGs and input trajectories
from `editor-fixture-manifest.json`. Test arrow/freehand drawing, text,
selection/resize, undo/redo, zoom, crop, blur and save/copy where supported.
Separate input-to-visible motion/frame pacing from CPU render/export timing.
Pixel-check the final output. Record blur kernel/region differences instead
of treating equal parameter numbers as equal image processing.

## Recording

Test MP4 and GIF. For comparable encoder experiments, use the same FFmpeg
binary, capture size, frame rate, encoder/preset/quality and audio setting.
Also show ordinary default pipelines separately. New ShareX versions may use
a different recorder backend; record it rather than assuming FFmpeg.

Measure the complete owned app process and encoder descendants over the same
motion fixture, with warmup separated. Report CPU time, CPU normalization,
private bytes, working set, sample interval and peak/median memory. GPU load is
reported only when a verified counter is available. Verify duration, decoded
frame count and actual motion/output quality. Parent-only memory is not
described as the recorder's total cost.

## Video and public graphics

Publish short MP4/GIF/APNG demonstrations plus a fuller comparison with an
input indicator and measured timer. Actual runtime footage runs at 1×. Any
shared slow-motion replay is labeled and applied equally. Never insert a
delay into the competing app or credit an earlier endpoint to one app.

Use a predeclared representative-run rule, such as the valid recorded run
closest to the median. Retain raw timestamps, data and representative source
clips. Charts show per-feature outcomes, including parity and cases where
ShareX is faster. The visual UI comparison lets viewers judge appearance;
there is no fabricated numerical beauty score or universal speed claim.
