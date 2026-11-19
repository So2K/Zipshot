# Final image performance verification

Production PNG file save is3.09–9.65× faster than official ShareX22.4 in the12-case generated corpus (20warm measurements/case). All10same-format automatic-save cases are faster here. This is not an application-wide or universal-decoding multiplier. See [production results](image_production_results.md) for complete file-save timing, sizes and hashes.

Lossless PNG pixels, dimensions, transparency and canonical color/DPI metadata are preserved. Opaque AutoJPEG retains its old decision and JPEG bytes. Encoder quality33SIMD+33scalar, immutable packet/freshness15 and threshold40 assertions passed. The SSE2 transparency helper passed295,300 exact checks.

The final trusted Windows WIC loader was compared against the literal original1fdf0a7 loader on49known-good inputs,20warm trials/engine,2058total rows. Every pixel, dimension and alpha flag matched. No new parser or third-party reader is connected. BOTH resolutions and all cases follow.

| Input | Original median ms | Final median ms | Ratio |
|---|---:|---:|---:|
| 1920x1080-alpha/engine0.png | 24.192 | 30.173 | 0.80× |
| 1920x1080-alpha/fast1.png | 23.004 | 29.026 | 0.79× |
| 1920x1080-alpha/sharex.png | 34.163 | 37.472 | 0.91× |
| 1920x1080-alpha-ui/engine0.png | 4.602 | 4.582 | 1.00× |
| 1920x1080-alpha-ui/fast1.png | 4.725 | 5.105 | 0.93× |
| 1920x1080-alpha-ui/sharex.png | 6.604 | 6.832 | 0.97× |
| 1920x1080-entropy/engine0.png | 18.556 | 17.910 | 1.04× |
| 1920x1080-entropy/fast1.png | 6.200 | 5.699 | 1.09× |
| 1920x1080-entropy/sharex.png | 35.278 | 33.311 | 1.06× |
| 1920x1080-entropy/engine0.jpg | 15.410 | 14.884 | 1.04× |
| 1920x1080-photo/engine0.png | 18.897 | 18.628 | 1.01× |
| 1920x1080-photo/fast1.png | 20.247 | 19.676 | 1.03× |
| 1920x1080-photo/sharex.png | 24.015 | 23.431 | 1.02× |
| 1920x1080-photo/engine0.jpg | 8.650 | 8.128 | 1.06× |
| 1920x1080-text/engine0.png | 6.075 | 5.362 | 1.13× |
| 1920x1080-text/fast1.png | 8.291 | 6.929 | 1.20× |
| 1920x1080-text/sharex.png | 9.115 | 8.717 | 1.05× |
| 1920x1080-text/engine0.jpg | 6.452 | 5.814 | 1.11× |
| 1920x1080-ui/engine0.png | 4.581 | 4.063 | 1.13× |
| 1920x1080-ui/fast1.png | 6.338 | 5.859 | 1.08× |
| 1920x1080-ui/sharex.png | 7.233 | 6.644 | 1.09× |
| 1920x1080-ui/engine0.jpg | 5.883 | 4.955 | 1.19× |
| 3840x2160-alpha/engine0.png | 109.910 | 104.094 | 1.06× |
| 3840x2160-alpha/fast1.png | 123.626 | 122.616 | 1.01× |
| 3840x2160-alpha/sharex.png | 148.518 | 149.063 | 1.00× |
| 3840x2160-alpha-ui/engine0.png | 19.905 | 19.812 | 1.00× |
| 3840x2160-alpha-ui/fast1.png | 17.762 | 17.768 | 1.00× |
| 3840x2160-alpha-ui/sharex.png | 25.084 | 25.224 | 0.99× |
| 3840x2160-entropy/engine0.png | 44.013 | 42.129 | 1.04× |
| 3840x2160-entropy/fast1.png | 23.243 | 21.335 | 1.09× |
| 3840x2160-entropy/sharex.png | 133.198 | 130.891 | 1.02× |
| 3840x2160-entropy/engine0.jpg | 64.773 | 61.635 | 1.05× |
| 3840x2160-photo/engine0.png | 65.710 | 64.602 | 1.02× |
| 3840x2160-photo/fast1.png | 74.748 | 72.250 | 1.03× |
| 3840x2160-photo/sharex.png | 74.478 | 73.122 | 1.02× |
| 3840x2160-photo/engine0.jpg | 35.021 | 32.807 | 1.07× |
| 3840x2160-text/engine0.png | 19.477 | 17.127 | 1.14× |
| 3840x2160-text/fast1.png | 25.868 | 24.013 | 1.08× |
| 3840x2160-text/sharex.png | 35.044 | 33.863 | 1.03× |
| 3840x2160-text/engine0.jpg | 23.349 | 20.030 | 1.17× |
| 3840x2160-ui/engine0.png | 20.414 | 17.881 | 1.14× |
| 3840x2160-ui/fast1.png | 26.474 | 23.885 | 1.11× |
| 3840x2160-ui/sharex.png | 27.218 | 25.430 | 1.07× |
| 3840x2160-ui/engine0.jpg | 20.137 | 16.164 | 1.25× |
| extra/gray-trns.png | 0.096 | 0.094 | 1.02× |
| extra/gray16.png | 0.084 | 0.083 | 1.02× |
| extra/hidden-rgb.png | 0.066 | 0.062 | 1.07× |
| extra/palette-alpha.png | 0.075 | 0.077 | 0.97× |
| extra/rgb-trns.png | 0.070 | 0.070 | 1.00× |

The49-input run showed an approximately25% slower1080p high-entropy-alpha decode while the build was active. A bounded follow-up with the build idle compared original WIC, the final direct-BGRA/SIMD path, and the original converter with SIMD, cyclically alternating9trials. It did not reproduce that regression: original/final1080p alpha24.397/24.435ms and new-file alpha22.424/22.349ms. The converter-only alternative was not consistently faster; production was retained. Raw original and follow-up results remain available, rather than discarding the slower rows. This points to environmental variability but does not prove its cause.

| Follow-up input | Original | Direct BGRA+SIMD | Original converter+SIMD |
|---|---:|---:|---:|
| 1920x1080-alpha/engine0.png | 24.397 | 24.435 | 24.860 |
| 1920x1080-alpha/fast1.png | 22.424 | 22.349 | 22.529 |
| 3840x2160-alpha/engine0.png | 97.371 | 95.862 | 97.208 |
| 3840x2160-alpha/fast1.png | 94.385 | 95.509 | 94.352 |
| 1920x1080-ui/sharex.png | 6.185 | 5.885 | 5.921 |
| 3840x2160-ui/sharex.png | 29.873 | 24.680 | 27.131 |
| 1920x1080-alpha-ui/engine0.png | 5.062 | 5.303 | 4.917 |
| 3840x2160-alpha-ui/engine0.png | 18.792 | 18.848 | 19.202 |

Limits: ambient background load is uncontrolled. First timed calls are not independent cold-process startup. Some PNG decoding cases remain faster in ShareX; fastPNG file size/reopen tradeoffs are disclosed. A third-party-reader experiment was stopped by automatic approval review and was not promoted. The application retains trusted WIC decoding. JPEG/video quality and user configuration were not silently changed.

File-save provenance is frozen in [production JSON](image_production_summary.json). Final loader source hashes and every timing are in [final WIC JSON](image_wic_final.json), [raw trials](image_wic_final_trials.csv) and [follow-up](image_wic_followup.json). The loader-only edit followed file-save timing and does not alter encoder/save paths. Optional benchmark reproduction is documented in [image_reproduce.md](image_reproduce.md).
