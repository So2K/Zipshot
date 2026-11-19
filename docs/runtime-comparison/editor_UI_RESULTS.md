# Real editor and region drawing evidence

The current-version clips show real guest input and displayed application pixels. They do not establish an application-wide speed winner. Zipshot 1.0.1 and ShareX 22.4.0 both draw the requested arrows and freehand strokes. Their smoothing, tool defaults and zoom behavior differ.

## What ran

All interaction runs used the same disposable Windows Sandbox desktop at 2982×1672 and 144 DPI. Its vGPU was an NVIDIA GeForce RTX 4070 Ti. Host input, installed applications and their settings were untouched. The guest had networking, clipboard redirection, microphone and camera access disabled.

Zipshot's standalone harness compiles its unchanged production editor with the real `ID2D1HwndRenderTarget`. No documentation WIC target replacement is used. ShareX runs as the unmodified official 22.4.0 portable application on its shipped .NET 10.0.14 runtime. The attempted ShareX library-window adapter never passed canvas readiness and contributes no published UI results.

The standalone set contains 16 recordings, one run per app for eight tools or actions. [editor_ui_runs.json](editor_ui_runs.json) gives the original source video hash, actual PTS, viewport mapping, input coordinates, source offset and saved output hash for every run.

| Action | Actual visual verification | Scope |
|---|---|---|
| Arrow | Real red arrow and saved edited PNG in both apps | Same four-pixel width, shadow off, 120 points over two seconds |
| Freehand | Real three-wave stroke and saved edited PNG in both apps | Same points and input schedule, different smoothing |
| Blur | Central 4K region visibly blurred, repeated after undo | Both settings at 15, different kernels and output strength |
| Undo/redo | Exactly 30 decoded red on/off transitions in each app | Same alternating Ctrl+Z/Ctrl+Y input at 100 ms intervals |
| Zoom | Actual image magnification changes in both apps | Same alternating Ctrl+wheel input, different behavior |
| Text | “Check this value” appears in both apps | Application default text styles differ |
| Pixelate | Actual central pixelation in both apps | Application default block sizes and appearance differ |
| Color picker | Real native picker popup visible in both recordings | Popup demonstration, no selection-speed comparison |

The native zoom recording alternates two visible scales and returns to its starting view. The ShareX recording shows 300% mid-run and ends at 25% after the same alternating wheel inputs. Its endpoint is therefore not described as a restored or equivalent viewport. The pixel check verifies changes, not an explanation of ShareX's zoom implementation.

The arrow and pencil use the identical synthetic 1920×1080 PNG. The 4K blur uses the matching 3840×2160 fixture. Native and stock fitted-image scales differ by 0.68%, with the measured transforms retained. Text, pixelate, blur, zoom and picker clips are feature demonstrations rather than quality-matched speed tests. Cropping, every tool variant and audio are not covered by this editor set.

## What the sampled timings mean

The cursor-free standalone observer requested 60 fps but captured about 32 frames per second. [editor_ui_summary.csv](editor_ui_summary.csv) reports actual video-sampled first-red and path estimates. Each arrow or pencil is one gesture, not 30 independent trials. The undo setup arrow is explicitly labelled `setup_arrow` in the CSV.

| Two-second gesture | Zipshot first red sample | ShareX 22.4.0 first red sample |
|---|---:|---:|
| Arrow | 20.7 ms | 24.8 ms |
| Freehand | 25.5 ms | 62.0 ms |

These are the first recorded red samples after the first scheduled stroke movement, index 1. Index 0 positions the cursor before button-down and is excluded from this timing origin. The observer interval is approximately 33 ms, with additional injection, capture and tip-localization uncertainty. Renderer and smoothing differences affect red-tip localization. These values are not physical input latency, UI refresh rates, or evidence that one app wins a single-frame difference.

[editor_ui_progress.csv](editor_ui_progress.csv) preserves the decoded visible-tip progression against actual timestamped sender progression. Each point comes from a captured video frame. Blank observed values indicate that the tip could not be localized reliably. `time_s` uses the recorded phase start, which precedes the button-down call by the controller's own function-entry overhead. It must not be presented as an exact hardware-input origin.

The original videos carry absolute epoch timestamps. Normalize only their origin with `setpts=PTS-STARTPTS`. Their real duration is `last_pts - first_pts`; container `format.duration` must not be used as elapsed length. Frame repetition required by a display format is not evidence of additional captured or presented UI frames. Any neutral input ring added to the standalone montage must be labelled “Recorded input position”. Its coordinates come from the real sender log.

## Drawing immediately after Ctrl+PrintScreen

The four additional region recordings use the complete unmodified Zipshot and ShareX applications. Real guest `Ctrl+PrintScreen` opens each actual region overlay. The controller verifies an owned foreground full-screen HWND, presses `A` or `F`, and draws the same 120 screen-coordinate points over two seconds. The output is a direct arrow or freehand mark on the frozen guest desktop, rather than a standalone editor window or a still-image animation.

[editor_region_runs.json](editor_region_runs.json) contains all four original video hashes, real hotkey/tool/button offsets and annotated before/after PNG proof. The GDI observer includes the actual guest cursor. Both applications use their defaults, including green for Zipshot and red for ShareX, so color, width, shadow and smoothing are not quality-matched.

Full-display observation produced about 17.7–22.7 recorded frames per second, with differing capture overhead. These clips demonstrate the requested region workflow and support no new latency or UI-frame-rate ranking. The native full-screen save outputs contain the annotations. ShareX's subsequent Space action did not create a PNG in the task save directory, so its visible drawing is verified without claiming a successful region save.

The exported PNG microbenchmark against the older installed ShareX 18.0.1 remains a separate [legacy component baseline](editor_PROTOCOL.md). It is not reused as a claim about the current editor's interactive performance.
