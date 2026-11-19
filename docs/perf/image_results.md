# Image codec profile

Synthetic 1080p/4K corpus, 20 warm measurements per operation. Processes use below-normal priority and affinity mask 0xF. Timings include allocation and codec work; verification/hashing is outside the timed interval. Ambient user/background load is uncontrolled.

`first` means first **timed** call of that operation, not cold process startup: earlier pixel verification can warm a decoder. Fresh-process cold measurements, when available, are reported separately. PNG candidates preserve exact straight BGRA pixels, dimensions, alpha flag and the baseline IHDR/sRGB/gAMA metadata. Both baseline and candidate omit pHYs (DPI).

| Case | WIC PNG ms | fpng slower ms | WIC/fpng | ShareX PNG ms | fpng p95 ms | WIC → fpng bytes |
|---|---:|---:|---:|---:|---:|---:|
| 1920x1080-ui | 33.25 | 7.99 | 4.16× | 34.38 | 8.39 | 73,851 → 137,419 |
| 1920x1080-text | 29.25 | 9.71 | 3.01× | 33.25 | 12.31 | 186,490 → 334,812 |
| 1920x1080-alpha-ui | 34.69 | 9.52 | 3.64× | 33.91 | 9.66 | 107,142 → 158,794 |
| 1920x1080-alpha | 181.52 | 22.55 | 8.05× | 192.87 | 31.21 | 7,183,652 → 7,107,628 |
| 1920x1080-photo | 113.20 | 18.20 | 6.22× | 93.93 | 28.01 | 3,087,625 → 4,578,331 |
| 1920x1080-entropy | 152.56 | 19.05 | 8.01× | 188.64 | 20.99 | 6,230,693 → 6,222,464 |
| 3840x2160-ui | 104.89 | 41.84 | 2.51× | 129.69 | 47.84 | 171,764 → 328,554 |
| 3840x2160-text | 117.71 | 33.05 | 3.56× | 122.92 | 35.46 | 427,725 → 738,190 |
| 3840x2160-alpha-ui | 136.96 | 48.77 | 2.81× | 128.26 | 61.34 | 219,295 → 389,110 |
| 3840x2160-alpha | 831.53 | 100.37 | 8.28× | 813.04 | 118.84 | 28,571,195 → 28,428,603 |
| 3840x2160-photo | 405.72 | 61.39 | 6.61× | 341.67 | 72.80 | 11,239,684 → 18,316,868 |
| 3840x2160-entropy | 530.17 | 74.57 | 7.11× | 733.04 | 86.23 | 24,902,988 → 24,887,369 |

The full JSON and trial CSV include JPEG encode/decode, PNG decode, both stock save configurations, FPNGE alternatives and file sizes; this table does not imply that every operation is faster.

ShareX 22.4.0 is executed through its unmodified TaskHelpers.SaveImageAsStream(image, format, taskSettings), TaskHelpers.PrepareImage and ImageData.Write methods, using the vendor's .NET 10.0.14 runtime. Its default PNG compression level is 1 with AllFilters, PNGBitDepth.Default, JPEG quality 90/4:2:0 and automatic JPEG above 2048 **decimal** KB. Zipshot's default threshold is 2048 **binary** KiB. The separately labelled stock matched run uses 2,048,000 bytes; thresholds are not silently equated.

ShareX's native image representation is premultiplied BGRA; input alpha premultiplication is performed outside the codec timer. Alpha round-tripping can quantize invisible/partially transparent RGB. Zipshot and its candidates use straight BGRA, and candidate equality is checked against that exact source. Stock automatic save can flatten alpha to JPEG; those alpha save timings are not equal-output comparisons.

Quality/metadata limits: 8-bit generated UI, text, opacity gradients, procedural photo-like noise and entropy images only. No user gallery, desktop pixels or clipboard were used. No HDR, external ICC profile, EXIF orientation, palette or 16-bit corpus is covered here. The library-decoder prototype is not approved for production based on these fixtures alone.

Production policy proposal: fast lossless PNG for alpha or explicitly forced PNG, retain current WIC PNG-size decision for opaque automatic JPEG. Larger files and slower decoding on some generated PNGs are disclosed in the JSON. No JPEG/video quality change is implemented by this profile.

Sources: [ShareX v22.4.0 TaskHelpers](https://github.com/ShareX/ShareX/blob/08bfff15e748a945944e609aca7faffbe888c7af/ShareX/TaskHelpers.cs), [stock Skia helpers](https://github.com/ShareX/ShareX/blob/08bfff15e748a945944e609aca7faffbe888c7af/ShareX.HelpersLib/Helpers/SkiaImageHelpers.cs), [fpng source and Unlicense](https://github.com/richgel999/fpng/tree/925796543b9d26b8edfcdcecd94c1dac280f29fc), [FPNGE source and Apache-2.0](https://github.com/veluca93/fpnge/tree/097ebccf3d3db443d9a8f238b1deed010a82f961).
