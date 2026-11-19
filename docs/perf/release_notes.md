# Zipshot 1.3.0

This local release improves measured image-save, decoding, editor and gallery
paths without reducing image or recording quality.

* Lossless PNG encoding uses pinned fpng with runtime CPU dispatch and a scalar
  fallback. Decoded pixels, straight alpha and color interpretation are preserved.
* Automatic JPEG saving stops discarded PNG work once the existing byte limit is
  definitely exceeded. JPEG settings and output bytes remain the same.
* Transparent captures share one encoded PNG between the clipboard and file.
  Encoding runs on a worker; a newer capture or user copy takes precedence.
* Image loading keeps WIC, skips identity conversion and unnecessary opacity
  scans, and uses an exact SSE2 opacity predicate on x64.
* Large gallery selection filtering uses a path set. Blur walks rows sequentially
  with exact integer averages. Unedited opaque export avoids a renderer roundtrip.
* Automatic cursor-follow processing reuses its already verified source and
  track, holding the source against modification until rendering finishes.
* Frequent forced working-set eviction is removed. Windows can reclaim resident
  pages under memory pressure without repeatedly faulting hot application code.

Fast PNG can be larger than WIC PNG and can decode more slowly on some files.
The existing automatic JPEG threshold and user configuration are retained.
Capture backends, video encoder settings, resolution and frame rate are unchanged.
All current search, filter, sorting, clipboard-shelf, continuous-brush and
cursor-follow behavior remains covered by native regression checks.

Measured scopes, sample counts, comparisons and remaining limitations are in
[the performance report](README.md). A component speedup is not a universal
application or input-to-display latency multiplier.
