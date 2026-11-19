# Isolated Windows capture feasibility

This is an environment and harness qualification, **not a Zipshot-versus-ShareX performance result**. The existing host API benchmark remains separate.

## Verified environment

On 2026-11-18, the host already had Windows Sandbox enabled and virtualization running. No Windows features, accounts, security settings or host applications were changed. No pre-existing Sandbox instance was reused. The probe used a read-only task input folder and a task-only writable output folder on D:, with network, host clipboard, microphone, camera and printer redirection disabled.

Probe 3 completed successfully in the disposable `WDAGUtilityAccount` guest, session 1, Windows 11 Enterprise LTSC build 26100. The guest enumerated an NVIDIA RTX 4070 Ti virtual GPU and Microsoft Remote Display Adapter. Production Zipshot `CaptureRect` code captured the known red, green, blue and white fixture correctly. Its logs reported a GDI fallback in this RDP-backed environment. This does not establish host DXGI performance or guest NVENC availability.

Both PrintScreen and Ctrl+PrintScreen were inserted through **SendInput inside the guest** and received through the guest's normal registered-hotkey route. The video contains the corresponding yellow and magenta fixture-state changes. These are synthetic keyboard events, not physical hardware keypresses. The probe's sub-millisecond dispatch receipts are **not application response times**.

The host launcher sampled foreground ownership and recorded no focus takeover. After writing results, the guest shut itself down. The remaining owned Sandbox disconnect dialog was closed without sending feedback or using host keyboard/mouse input.

## Display and recording limits

The actual guest desktop remained **2982×1672**, despite a guest resolution request and a focus-preserving resize of the owned host Sandbox window. Report the observed size consistently for both products. A smaller 1280×720 recording ROI can be used independently of the full-desktop capture operation.

The four-second probe clip contains **122 decoded frames**, approximately **30.5 actual frames/s**. Its stream metadata advertises 60 fps; that metadata alone is insufficient to claim a 60 fps recording. The final tests must preserve source timestamps, report actual frame counts and gaps, and verify visible responses in recorded pixels. No clip has been retimed to imply faster interaction.

## Probe artifacts

Local, task-owned artifacts are under `D:/ZipshotRuntimeCompare-20261118/probe3/sandbox-output/`:

- `capture_guest_result.json`: guest identity, GPU, native checks and ffprobe data.
- `capture_native_result.json`: exact dimensions and input-event QPC receipts.
- `capture_native.log`: capture-path diagnostics.
- `capture_fixture.png`: verified known-colour screen capture.
- `capture_probe.mkv`: real guest desktop recording at original timestamps.
- `capture_video_printscreen.png` and `capture_video_ctrlprintscreen.png`: frames extracted from that recording, showing the input responses.

Earlier probe outputs were preserved separately. Their outer script failures were harness errors: a missing PowerShell 5 process exit code in probe 1 and treatment of FFmpeg informational stderr as an exception in probe 2. Both were corrected before probe 3. They were not product bugs.

## Full-application capture controller

`tools/runtime-compare/capture_hotkeys.cpp` observes the actual application hotkey path and refuses all input/screen access outside the disposable guest. It does not register a competing hotkey or call a product's capture test command. The caller starts one application with explicit, matched hotkey and after-capture settings over a static, generated scene.

The controller records SendInput/QPC timestamps, a first changed composited-desktop ROI interval, valid image clipboard arrival and closed WIC-readable image-file arrival as separate endpoints. A region trial also requires evidence of a target application window, preventing an unrelated OS overlay from being counted as the product response. Pixel detection uses at least 16 pixels changing by at least 8 channel levels. The visual lower bound is the **start** of the last unchanged readback; the upper bound is the **end** of the first changed readback. This includes uncertainty about when a readback sampled the compositor. Observer gaps are retained; report that uncertainty. A full-capture visual change without target-window evidence is a candidate for video review, not an automatically attributed product response.

The full application tests require a guest smoke run and sample video review before the planned 30 measured trials per application/scenario. No full-application latency or resource result is claimed by this feasibility document.
