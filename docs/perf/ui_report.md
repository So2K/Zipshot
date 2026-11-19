# UI and editor performance,2026-11-19

This measures Zipshot before and after targeted changes against source1fdf0a7. The measurements are CPU components on synthetic data. They do not rank ShareX, describe physical key-to-monitor latency or prove a universal speedup.

| Component | Before median,ms | After median,ms | Before/after ratio | Measured calls each |
|---|---:|---:|---:|---:|
| Gallery,10,199picked | 175.70 | 15.07 | 11.66× | 20 |
| Gallery,Unicodefilter after selecting all | 198.11 | 15.32 | 12.93× | 20 |
| Gallery,no picked items | 9.35 | 9.55 | 0.98× | 20 |
| Uncached blur35,1920x1080 | 79.34 | 43.99 | 1.80× | 20 |
| Plain opaque export,1920x1080 | 3.89 | 1.14 | 3.42× | 20 |
| Uncached blur35,3840x2160 | 329.71 | 172.06 | 1.92× | 20 |
| Plain opaque export,3840x2160 | 23.10 | 6.45 | 3.58× | 20 |

The gallery uses10,199 synthetic metadata entries and an owned hidden native window. Before and after methods alternate, with three excluded warmups and20 measured calls each. Its large-selection filter previously scanned every visible filename for every selected path. Production now uses an exact,case-sensitive visible-path hash set only when selection is nonempty. Search,sort,catalog size,thumbnail transfer,pick order,focus and anchors remain unchanged. The no-selection row is within ordinary timing variation and is not a claimed improvement. Sorted-order caching and a broader rebuild prototype remain experiments only.

The editor compares actual FxImage and Export against frozen pre-change bodies. Each measured uncached blur clears the image-effect cache outside the timer,then includes allocation,copies and all six passes. Verification copies and complete-pixel comparisons occur outside the timed interval. Code and the small exact division table are warm after three excluded calls. Before/after order alternates over20 accepted calls each. Ordinary host and other agent work continued on this shared machine,so p95 and all raw samples are retained in ui_results.json rather than interpreting small differences as wins.

Blur preserves its six passes,integer division,replicated edges and opaque result alpha. The vertical rolling sums now visit rows sequentially. The division table is bounded and thread-local; tiny images and radii above256 retain the old loop. The opaque export shortcut applies only to a valid,exact-size source buffer with no shapes,mask or explicit crop. It copies pixels and normalizes unused opaque alpha bytes just as the old D2D upload/output did. Transparent,annotated,cropped,fractional-area and masked exports retain the renderer path.

The focused regressions passed73 native gallery assertions,66 effect/export assertions with164 exact pixel-equivalence cases,and86 existing brush assertions. They cover large selection/filter integrity,odd and tiny images,zero/partial alpha,edge and fractional crops,outside-canvas areas,actual ellipse masking,large-radius fallback,buffer guards,thread-local independence,cache reuse/invalidation and undo/redo. Continuous brush drawing and Select-only pen movement remain intact. Profiling also compared132 full Images exactly,including dimensions,transparency metadata and all pixels.

A secondary probe trimmed only the profiler's own working set. The next4K opaque export took16.27ms median versus7.04ms without that trim,with roughly16,000 additional page faults. This supports investigating forced trimming on hot paths. It does not by itself measure an installed-application improvement,and no application TrimMemory change was made by this profiling task.

Raw timings and all repetitions are in ui_results.json. The two tools/perf/ui_*profile.bat scripts build and run the profilers. Frozen reference bodies remain in tools/perf/ui_gallery_baseline.h and ui_editor_reference.h; ignored build/perf/ui contains source hashes,full original editor source,validation logs and verification.json. Both profilers run below normal priority with affinity to four logical CPUs. No host screen,clipboard,input,focus,installed app or user settings were used.
