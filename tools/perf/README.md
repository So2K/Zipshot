# Image performance tools

Production uses pinned `third_party/fpng` and trusted Windows WIC decoding. See [final results](../../docs/perf/image_final.md) and [reproduction/scope](../../docs/perf/image_reproduce.md). No retired experimental reader or malformed-file workflow is required.

The optional fpng/FPNGE **encoder** comparison expects sources under ignored `build/perf/images/vendor`:

- Copy the unmodified fpng files from `third_party/fpng` into `vendor/fpng`.
- Fetch `fpnge.cc`, `fpnge.h`, `README.md` and `LICENSE` from `veluca93/fpnge` commit `097ebccf3d3db443d9a8f238b1deed010a82f961` into `vendor/fpnge`.
- `image_vendor_pins.json` records exact expected hashes; `image_fetch_vendor.py` is an optional equivalent fetch helper.
- Run `image_fastpng_build.bat` only for the comparative encoder tool. FPNGE is Apache-2.0 and is not linked into the application. Production fpng is Unlicense, with original attribution retained.

`image_production*` measures actual file-save APIs; `image_wic_native*` compares a frozen original WIC reference against actual production. `image_fpng_quality.bat` and `--scalar` check exact output quality and CPU fallback. Measurements are synthetic-only, below-normal priority, bounded to four logical processors. The ShareX harness has its own CLI entry point against copied official runtime DLLs and does not start the installed application.
