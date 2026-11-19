# Full-application hotkey capture baseline

These measurements compare the unchanged **Zipshot 1.0.1** release and official **ShareX 22.4.0** in an isolated Windows Sandbox guest. They describe this virtual RDP/vGPU environment. They are separate from the earlier physical-host capture API benchmark against ShareX 18.0.1.

| Measured endpoint | Zipshot median | ShareX 22.4.0 median |
|---|---:|---:|
| First target-confirmed change in the centre capture-overlay ROI | 167.5–216.7 ms interval | 258.8–316.7 ms interval |
| PrintScreen → first observed valid clipboard image | 300.4 ms | 447.6 ms |
| PrintScreen → first observed closed, WIC-readable image file | 341.3 ms | 529.5 ms |

There are **30 measured trials per application and scenario**, collected as three alternating-order blocks of ten. Each process had three excluded warmup trials. All 120 measured trials passed their primary endpoint checks. Every full-capture clipboard image and file had the actual desktop dimensions, **2982×1672**.

The overlay row is interval-censored: its lower endpoint is the start of the last unchanged desktop readback, and its upper endpoint is the end of the first changed readback. The median interval widths were 53.5 ms for Zipshot and 56.5 ms for ShareX. The 95th-percentile observed upper bounds were 306.2 ms and 349.4 ms. These are first changes in a defined ROI with product-window evidence; they do not establish when every toolbar control is fully ready or when physical monitor photons change.

The clipboard 95th percentiles were 372.9 ms and 549.8 ms; file 95th percentiles were 403.3 ms and 622.7 ms. These endpoints include polling and image validation overhead.

## Scope and conditions

- Genuine global application hotkeys were triggered through **SendInput inside the guest**. These are synthetic OS keyboard events, not hardware keypresses. Neither product's capture test command was used.
- Ctrl+PrintScreen opened region capture; Escape cancelled after the observed response. PrintScreen copied and saved a PNG. Uploads, sound and cursor capture were disabled for both products.
- The guest used a static generated wallpaper, Windows 11 build 26100, a virtual GPU reporting RTX 4070 Ti and Microsoft Remote Display Adapter. No host keyboard, mouse, clipboard or private desktop pixels were used.
- The pixel observer used a persistent 1280×720 GDI ROI: centre for region capture, bottom-right for the full-capture notification. No FFmpeg recorder ran during these statistical baseline blocks.
- Application order was Zipshot/ShareX in blocks 1 and 3, reversed in block 2. Applications started fresh for each block. Normal host work and background workloads continued.
- Recorded demonstrations are separate workloads, retain their own actual timestamps and frame-rate evidence, and are not silently pooled with the unrecorded baseline.

Zipshot deliberately marks its notification with `WDA_EXCLUDEFROMCAPTURE` in [toast.cpp](../../src/toast.cpp). Consequently its full-capture toast was not observable in the screen-capture ROI in all 30 trials. No zero-latency value or comparative toast-speed claim is assigned. Clipboard and file completion remain independently verified.

## Audit data

- [Grouped results and source hashes](capture_baseline.json).
- [All 156 trial records, including 36 excluded warmups](capture_trials.csv).
- [All 2,402 observer readbacks and their QPC bounds](capture_frames.csv).

The JSON manifest identifies each original per-job result and metadata file by SHA256, along with application and controller identities. `capture_export.py` regenerates these derived files from the preserved isolated-session outputs. Failed early harness smoke runs are preserved separately and are not benchmark samples.

## Static versus moving desktop diagnostic

A separate API-only diagnostic used a known blue fixture and compared a stationary scene with a 16 ms moving marker. Untouched production `CaptureRect` code returned valid fixture pixels in every call. After one excluded warmup, the three static samples were 211.5, 217.9 and 227.2 ms; the moving samples were 210.1, 223.2 and 230.0 ms. This small diagnostic did not demonstrate a static-only extra delay.

A trace-only **copy** of `capture.cpp` logged acquired-frame metadata and sampled a texture pixel before the existing zero-LastPresentTime retry branch. In this guest, the acquired zero-time DDA texture was black while the known desktop was blue; subsequent acquisitions produced invalid-call/access-lost errors, and GDI returned the correct pixels. This did not confirm the hypothesis of a valid image being discarded solely because its timestamp was zero. The trace instrumentation is not used as a performance benchmark. No production code was changed on the strength of this diagnostic.

An additional 64×64 top-toolbar ROI diagnostic did not materially reduce readback uncertainty: median interval widths remained about 51–53 ms. Its three-trial results overlap and describe a different ROI, so they are not pooled with the baseline or used to declare a toolbar-speed winner.
