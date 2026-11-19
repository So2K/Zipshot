# Production image save profile

Actual production functions after fast lossless PNG and bounded WIC automatic-JPEG integration. Raw synthetic source pixels are resident before the timer; encoder, allocation, output-stream/file creation and completed cached file write are included. Official ShareX22.4.0 uses its unchanged methods and exact self-contained .NET10.0.14 runtime. Both process families use below-normal priority and affinity0xF. Each row has20 warm timings; p95 uses nearest rank.

| Case | Zipshot forced PNG median / p95 ms | ShareX forced PNG median / p95 ms | Ratio | Zipshot Auto ms / format | ShareX Auto ms / format |
|---|---:|---:|---:|---:|---:|
| 1920x1080-ui | 8.88 / 9.22 | 31.71 / 35.07 | 3.57× | 26.30 PNG | 31.91 PNG |
| 1920x1080-text | 9.52 / 10.21 | 31.97 / 33.07 | 3.36× | 27.72 PNG | 31.92 PNG |
| 1920x1080-alpha-ui | 10.72 / 11.48 | 33.15 / 53.09 | 3.09× | 8.77 PNG | 33.18 PNG |
| 1920x1080-alpha | 23.23 / 32.08 | 177.64 / 186.89 | 7.65× | 19.16 PNG | 96.92 JPEG |
| 1920x1080-photo | 18.56 / 19.79 | 88.95 / 93.61 | 4.79× | 80.46 JPEG | 90.04 JPEG |
| 1920x1080-entropy | 20.37 / 21.64 | 185.54 / 196.45 | 9.11× | 54.60 JPEG | 100.96 JPEG |
| 3840x2160-ui | 27.66 / 29.82 | 122.93 / 131.15 | 4.44× | 99.59 PNG | 121.80 PNG |
| 3840x2160-text | 29.03 / 31.26 | 116.21 / 139.24 | 4.00× | 109.92 PNG | 123.27 PNG |
| 3840x2160-alpha-ui | 34.23 / 36.40 | 128.97 / 133.30 | 3.77× | 33.32 PNG | 127.01 PNG |
| 3840x2160-alpha | 88.07 / 91.09 | 741.20 / 817.73 | 8.42× | 78.03 PNG | 228.05 JPEG |
| 3840x2160-photo | 68.93 / 73.21 | 338.30 / 369.44 | 4.91× | 101.80 JPEG | 195.92 JPEG |
| 3840x2160-entropy | 75.36 / 78.10 | 727.20 / 747.31 | 9.65× | 87.03 JPEG | 242.31 JPEG |

Forced PNG file saving is 3.09–9.65× faster in this12-case generated corpus. All10 same-format Auto rows are faster here; that is not a claim about every possible picture, filesystem, machine or complete UI workflow.

High-entropy alpha automatic save is PNG in Zipshot and JPEG in stock ShareX, so those2 rows are excluded from the same-format Auto ratio. PNG source pixels and alpha remain exact. The first representative AutoJPEG file in every JPEG-selected case matches the previous Zipshot WIC JPEG bytes exactly; the separate40-assertion threshold suite covers boundary/failure behavior. No JPEG quality, subsampling, dimensions, user settings or threshold units were silently changed.

Fast PNG can produce larger files than the previous WIC PNG, especially UI/text. Color interpretation remains the same IHDR/sRGB/gAMA with no pHYs chunk. Detailed sizes, complete chunk CRC/IEND validation, all raw timings and captured source/executable hashes are in the accompanying JSON/CSV. Current decoder results are separately profiled; no unsafe upstream fpng reader is integrated.

The stock source/timing explanation, including legitimate early PNG termination above the JPEG threshold, is in [image_stock_path.md](image_stock_path.md). Baseline codec and decoder measurements are in [image_results.md](image_results.md).
