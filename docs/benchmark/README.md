# Zipshot vs the installed ShareX: measured capture API latency

On this computer, the patched Zipshot capture API was **2.9× faster** than the installed **ShareX 18.0.1** capture API in this test. This is a warm capture microbenchmark, not a measurement of the complete screenshot workflow or a comparison against the latest ShareX release.

| Full virtual desktop, cursor off | Zipshot | ShareX 18.0.1 |
|---|---:|---:|
| Median capture latency | 19.6 ms | 57.3 ms |
| 95th percentile | 20.2 ms | 67.9 ms |
| Measured captures | 60 | 60 |

The median ratio is 57.2736 / 19.5986 = 2.9223. There is no evidence for a 1000× claim. The complete raw data, timestamps, source/library hashes and machine details are in [capture-results.json](capture-results.json).

## What is measured

The native harness directly includes the project's actual `src/capture.cpp` and calls `CaptureRect` on the full virtual desktop. The managed harness calls the actual installed `ShareX.ScreenCaptureLib.Screenshot.CaptureRectangle` method through reflection. Both request the same rectangle, `(-1920, 0, 5760, 2160)`, with cursor capture disabled. Both validate that the returned image has the requested dimensions. Each timer stops after the API returns its image; image disposal happens outside the timed interval.

There are three blocks per app, 20 measured captures per block, and three discarded warmups at the start of each process. Block order alternates: Zipshot/ShareX, ShareX/Zipshot, Zipshot/ShareX. The ShareX harness runs under the installed application's original self-contained .NET 9.0.8 runtime. It never starts the ShareX user interface. Native code is compiled with MSVC `/O2 /GL /MT`.

The computer is a Ryzen 9 7950X with an RTX 4070 Ti and AMD integrated graphics, Windows 11 build 26100. Normal desktop applications and background activity remained running. These are practical observations on this machine, not controlled laboratory results or a universal speed guarantee.

**Privacy:** desktop pixels exist only in temporary image objects in process memory and are discarded. The harnesses do not write screenshots, inspect titles, touch the clipboard, simulate input or display windows. Only numeric timings, image dimensions and machine/library metadata are recorded.

## Reproduce

Requirements: Windows, PowerShell 7, Visual Studio 2022 C++ tools, a .NET SDK, and a self-contained ShareX 18.0.1 installation. Do not substitute a newer library and retain the 18.0.1 label; the JSON records the actual library and runtime versions. The staged runtime is task-owned and must be outside the installed app folder. Same-volume files are hardlinked to avoid duplicating runtime storage; the script never modifies those linked runtime files. The newly compiled harness is a separate file.

From the repository root:

```powershell
.\tools\benchmark\build.bat
.\tools\benchmark\build-managed.ps1 -ShareXDir 'D:/Programs/ShareX' -RuntimeStage 'D:/Tools/ZipshotBenchmark-new-run'
.\tools\benchmark\run.ps1 -ShareXDir 'D:/Programs/ShareX' -RuntimeStage 'D:/Tools/ZipshotBenchmark-new-run' -Output 'docs/benchmark/capture-results.json'
```

The build also creates a framework-dependent managed harness for diagnostics. The recorded benchmark uses the staged original ShareX runtime, not that diagnostic executable. No installed application settings or binaries are changed.

## Limits and interpretation

- ShareX 18.0.1 is the version installed on this computer. These numbers do not describe the newest ShareX version.
- Hotkey dispatch, region overlay appearance, annotation, encoding, clipboard transfer, disk writing and uploads are excluded.
- Non-null output and dimensions are checked. No desktop content is persisted for pixel-by-pixel fidelity verification.
- Warm API calls exclude startup, JIT compilation and GPU initialization. They do not describe first-run latency.
- Reflection dispatch is included on the managed side; its small overhead has not been separately subtracted.
- The result file identifies the Git base and both the exact capture source hash (`zipshot_capture_sha256`) and the UTF-8 source hash after CRLF-to-LF normalization (`zipshot_capture_normalized_sha256`). The normalized hash supports verification against Git's LF-normalized public source. The measurements were generated after the capture fixes in this review; uncommitted patch identity is represented by these hashes.

## Source references

The benchmark calls the installed binary rather than a recreated ShareX algorithm. For implementation context, the official [ShareX 18.0.1 screenshot source](https://github.com/ShareX/ShareX/blob/v18.0.1/ShareX.ScreenCaptureLib/Screenshot.cs) uses a GDI capture path. The project remains credited at [ShareX/ShareX](https://github.com/ShareX/ShareX). No ShareX library or runtime binary is included in this repository's benchmark files.
