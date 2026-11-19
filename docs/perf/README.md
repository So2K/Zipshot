# Zipshot 1.3 performance and result preservation

This pass improves image saving, selected-gallery rebuilds, editor effects and focus-copy processing. It compares different components under their stated conditions. It does not establish that every Zipshot operation is faster than ShareX or measure complete physical keypress-to-display latency.

The reference for Zipshot changes is source `1fdf0a7`. Official comparisons use the unchanged ShareX 22.4.0 portable assemblies and their shipped .NET 10.0.14 runtime. All benchmark fixtures are synthetic; the timed helpers do not read the user's desktop, clipboard or gallery. Timed helpers used below-normal priority and a four-logical-processor affinity budget while ordinary host work continued.

| Area | Measured result | What the result covers |
|---|---|---|
| Forced PNG file saving versus official ShareX | 3.09 to 9.65 times faster across the 12 generated 1080p/4K cases | Allocation, encoding and ordinary cached file writing from resident source pixels |
| Automatic image saving | All 10 same-format rows were faster in this corpus | Actual default preparation and file-writing paths; two PNG/JPEG format-mismatch rows are excluded |
| Image decoding | Opaque cases improve; alpha cases are mostly neutral with some losses | Same-file WIC comparisons, including the initial regression and its non-reproducing idle follow-up |
| Gallery with 10,199 selected items | Median 175.70 ms to 15.07 ms | Actual gallery rebuild CPU work on an owned hidden native window |
| Gallery Unicode filter after selecting all | Median 198.11 ms to 15.32 ms | Same selected paths, order, focus and anchors after filtering |
| Uncached 4K editor blur | Median 329.71 ms to 172.06 ms | Complete six-pass effect computation, with exact output pixels |
| Plain opaque 4K editor export | Median 23.10 ms to 6.45 ms | Document export with no annotations, mask or explicit crop |
| Verified-source focus-copy control | Median 468.51 ms to 366.61 ms | Actual production API path on a deliberately padded synthetic video, isolating duplicate hashing |

## Image encoding and automatic format choice

[Production image results](image_production_results.md) contain all 12 forced-PNG and automatic-save rows, p95 values, formats and file-size observations. [The official ShareX path](image_stock_path.md) identifies the exact methods, source revision and settings used. The timer includes fresh preparation and file writing rather than resaving an already encoded input.

Zipshot's fast PNG path remains lossless. Dimensions, straight BGRA pixels, transparency and the baseline IHDR/sRGB/gAMA interpretation are checked. It can produce larger files, especially for UI/text images, and some generated PNG files take longer to decode. Faster encoding is therefore not a claim that every part of an image round trip improves.

Opaque automatic saving retains the previous WIC PNG-size decision, JPEG quality and threshold units. A bounded stream can stop the verified append-only Microsoft PNG writer after it has certainly crossed the threshold, avoiding work on a PNG that will be discarded. Observed stream rewrites or preallocation disable that shortcut. Boundary and failure regressions check the same PNG/JPEG decision and complete selected output bytes against the old full-PNG path.

ShareX's default threshold is expressed in decimal KB; Zipshot's existing threshold uses binary KiB. The detailed report labels configurations rather than silently treating these units as equal. Stock ShareX can flatten high-entropy alpha images to JPEG, while Zipshot retains PNG and alpha. Those two automatic-save rows are different outputs and are excluded from the same-format comparison. ShareX also uses premultiplied internal pixels, so same-format stock timing is not proof that its decoded alpha RGB values equal Zipshot's straight-alpha source.

Transparent after-capture PNG preparation now runs once on a worker and supplies the same immutable packet to disk and clipboard consumers. The clipboard freshness check runs with the clipboard open, before it is emptied, and verifies both capture order and the OS clipboard sequence. A newer capture or manual copy wins over an older delayed result. Native clipboard publication remains on the UI thread; the result is not an assertion that every clipboard operation is instantaneous. Graceful quit waits for pending capture saves.

Raw codec data and exploratory context are in [the baseline image report](image_results.md), [production JSON](image_production_summary.json) and [production trial CSV](image_production_trials.csv).

## Image decoding

The production reader remains WIC. The narrow native-format path avoids an unnecessary BGRA converter when the frame is already straight BGRA, and known opaque native formats avoid a redundant transparency scan. Other formats retain the existing conversion and decoded-pixel checks.

The completed [image report](image_final.md) and [49-input matrix](image_wic_final.json) compare the literal original reader with actual production. Twenty warm alternating trials per engine produced 2,058 decoded images, all with identical pixels, dimensions and alpha flags. Inputs include old/new PNG, JPEG, stock RGBA PNG, palette/color-key transparency, grayscale16 and hidden RGB. Experimental library readers were not adopted.

An initial 1080p alpha example changed from 24.19 ms to 30.17 ms, about 25% slower. A bounded [idle follow-up](image_wic_followup.json) compared three trusted-WIC paths on eight inputs, nine cyclically ordered repetitions each. That large regression did not reproduce, with 24.40 ms original versus 24.43 ms production on the same file. The stock opaque RGBA 4K control improved from 29.87 to 24.68 ms. A smaller alpha-UI loss remained in one control, and returning to the converter was not consistently better. Both result sets are retained. Some PNG decoding remains faster in ShareX; there is no universal decode-speed claim.

## Gallery and editor work

[UI/editor results](ui_report.md) and [all paired samples](ui_results.json) compare the old Zipshot code with the production changes. The gallery previously scanned every visible path for each selected path. It now builds an exact visible-path membership set only when selection exists. The complete catalog, Unicode matching, sorting, selected-path order, thumbnail ownership and focused item remain unchanged. Without a selection, measured rebuild medians were 9.35 and 9.55 ms, within ordinary run variation.

Blur retains all six passes, replicated edge handling and integer averages. Sequential row access and an exact bounded division table reduce repeated memory and division costs. Tiny images and unusually large radii keep the previous loop. Cached effect reuse remains intact.

The opaque export shortcut applies only to a valid exact-size buffer without shapes, a mask or an explicit crop. It copies source pixels and normalizes unused opaque alpha just as the old renderer did. Transparent, annotated, cropped, fractional-area and masked paths retain the renderer. The focused checks include 164 exact pixel-equivalence cases, cache invalidation, image replacement and undo/redo. Existing continuous-brush and Select-tool behavior remains covered by the 86 brush assertions; the native gallery suite passes 73 assertions.

Forced working-set eviction was removed from application hot-path cleanup. In a separate probe affecting only the profiler process, the next 4K opaque export took 16.27 ms after eviction versus 7.04 ms without it, with roughly 16,000 additional page faults. This explains the change but is not an installed-application end-to-end latency measurement. Image, thumbnail and GPU-buffer owners still release their allocations normally.

## Focus-copy video processing

[Video findings](video_findings.md) and [compact result data](video_results.json) describe the retained verified-source path. Automatic processing hashes the source once, keeps it open against replacement or writes, and retains its track. Manual application still verifies the complete source against its sidecar. Crop commands, resolution, cadence, x264 preset/quality and audio stream copying remain unchanged.

The 21.75% timing reduction comes from four counterbalanced repetitions on a valid four-second, 640x360 video padded with a legal 128 MiB ISO `free` box. Padding isolates file-byte hashing; it does not represent an ordinary compression workload. All eight complete decoded frame hashes match and the input is unchanged. This does not predict that every video becomes 21.75% faster.

Thread-count changes, reduced crop commands and reduced ffprobe discovery were tested and rejected where timing gains were absent, output validation was insufficient, or stream timestamp fields became unavailable. The detailed report retains these negative controls.

## Capture core and unadopted experiments

Desktop acquisition in `src/capture.cpp` is unchanged in this version. No new native GPU capture-speed or capture-pixel-equivalence claim is made. A guarded retry-policy pilot was staged but not run while host RAM was constrained. Its model tests are not actual latency or image-quality evidence.

Dirty-geometry stroke coalescing and unused-arrow-cache experiments remain unadopted. Their software-renderer proxy is not a hardware presentation benchmark. Manual SIMD opaque-copy controls showed no meaningful general gain and were not integrated.

Unadopted source experiments and their original hashes are preserved under ignored `build/perf/experiments-source`; they are not production build dependencies. The production reports above retain conditions, all recorded repetitions, quality checks and limits.

## Release verification

All 15 native regression suites passed. [The verification record](verification.json)
retains individual results, exact final source hashes and installed executable
identity. Version 1.3.0 was installed with the previous executable and settings
backed up. Its actual gallery confirmed a visible rendered window and search
control; configuration bytes remained unchanged. These readiness checks are
separate from synthetic benchmark timings.
