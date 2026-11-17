# Zipshot 1.0.1 audit

Date: 2026-11-17. This is a targeted review and regression pass over the local capture, image output, recording geometry, image combination, clipboard parsing, IPC and build paths. It is not a security certification or a claim that the application has no remaining bugs.

## Fixed issues

| Area | Problem and change | Verification |
|---|---|---|
| Cropping | Crops lost the source's transparency flag. The crop now preserves alpha metadata and pixels. | Synthetic pixel assertions and PNG encoding check. |
| Saving captures | PNG-to-JPEG fallback could overwrite an existing JPEG; concurrent saves could select the same path. Final filenames are now claimed atomically with `CREATE_NEW`, with numbered alternatives. Failed writes remove their partial output. | Existing JPEG remains unchanged; eight concurrent PNG saves receive distinct paths and decode to the original pixels. |
| First-run destination | The default save path referred to one developer's ShareX installation. The fallback now uses the current user's Pictures\Zipshot folder. | Isolated first-run settings assertion. ShareX import may intentionally supply another directory. |
| Encoding/loading | Some WIC calls and image-buffer sizes were unchecked. Encoding validates dimensions/buffer length and checks stage failures. Failed loads preserve the caller's image; reopening transparent images now preserves their straight-alpha pixels instead of flattening them over white. | Invalid buffer rejection, missing-file handling, transparent PNG roundtrip and PNG/JPEG assertions. No exhaustive malformed-file fuzzing. |
| Capture failures | DXGI enumeration, output descriptions and acquired-resource conversion had unchecked failure paths; selected DIB allocations were dereferenced without checking. | Source review; invalid DIB-size regression. GPU-loss and allocation-failure injection were not performed. |
| Recording geometry | Tiny selections near a monitor's right/bottom edges could expand beyond the monitor. Normalization now keeps minimum/even dimensions inside the selected display and rejects disjoint selections. | Three synthetic geometry cases, including a monitor with negative coordinates. |
| Combining images | Result transparency depended on layout gaps, losing genuine source alpha. Opaque capture alpha bytes were also copied without normalization. The result now preserves transparent sources and normalizes opaque pixels/fill colors. | Synthetic combinations with/without gaps and automatic fill, plus wrapped/centered layout assertions. Seven assertions failed before the fix. |
| Clipboard DIB input | Header and palette sizes were used before validation, and bit depth was read after unlocking the clipboard handle. A bounded DIB wrapper validates offsets and caches metadata while memory is locked. | Pure parser tests for valid input, null/truncated/oversized headers, overflowing/truncated palettes and missing bitfields. The real system clipboard was not modified. |
| Clipboard text | Text scanning assumed a terminating NUL. Scanning is now bounded by the clipboard allocation's size. | Source review; no system clipboard mutation. |
| IPC/message loop | Command payloads lacked length/embedded-NUL validation; `GetMessage` errors were treated as messages. Both paths now reject errors. Command forwarding has a three-second timeout and returns failure for rejected commands or an unresponsive target. | Source review; no malicious-message or hung-instance integration test. |
| Release build | The build depended on a fixed Community installation path and disabled stack cookies. It now discovers VS through `vswhere`, honors an existing C++ developer environment, and enables stack cookies, CFG, ASLR and DEP. | Build configuration review and release build; regression scripts support the same toolchain discovery. |

The WIC factory lookup used by shape-image loading also now handles factory creation failure before dereferencing it.

## Automated regression evidence

Both runners compile and execute actual production pixel/output/geometry routines with synthetic inputs. Unused UI entry points are stubbed; accidental calls to these paths abort the tests.

| Runner | Assertions | Result |
|---|---:|---|
| `tests\run_capture_output_tests.bat` | 16 | PASS |
| `tools\test_image_core.bat` | 19 | PASS |
| Total | 35 | PASS |

The capture/output suite writes its own new per-run directory under `build\tests`. The image-core suite uses `build\tests-image-core`. Neither suite captures the user's desktop, accesses their installed settings or changes their clipboard. `git diff --check` passed; Git emitted line-ending conversion notices only.

The actual executable's `--test-demo` path ran successfully (exit code 0) against an original synthetic fixture, exporting five offscreen editor states and the resulting annotated image. This is separate from the 35 assertions and does not establish that every interactive editor flow works.

## Architecture and remaining limits

- `app.cpp` owns hotkeys, tray commands, main-thread dispatch and after-capture routing. Saving captures runs on background workers.
- `capture.cpp` provides DXGI Desktop Duplication with a GDI fallback. `output.cpp` uses Windows Imaging Component for PNG/JPEG and local file output.
- `editor.cpp` draws annotations through Direct2D/DirectWrite; image operations, combination and sticker/image helpers live in separate modules. `recorder.cpp` orchestrates an external FFmpeg process.
- Upload, destinations and workflows are unavailable placeholders, not a replacement for ShareX's full automation/upload feature set.
- Recording/video encoding, pause/resume, microphone/audio devices and encoder fallbacks were not exercised end to end by these regression suites. Supported code paths do not imply verified operation on every FFmpeg build or GPU.
- Multiple-monitor rotation/DPI changes, remote sessions, display/device removal, memory exhaustion and long-running capture stress were not comprehensively tested.
- Document image loading preserves straight-alpha transparency; shape/sticker images use premultiplied alpha for rendering.
- Large image/effect allocations and third-party decoder behavior still warrant dedicated resource-limit testing and fuzzing. No external penetration test, code-signing or exhaustive dependency audit was performed.
- Optional sticker downloads were not rebuilt or bundled. The Noto image pack's Apache metadata conflicts with its upstream root SIL OFL license file; see the third-party notices before redistributing downloaded assets.

Performance evidence and its narrower measurement scope are documented separately in the [benchmark report](benchmark/README.md), with [raw measurements](benchmark/capture-results.json).
