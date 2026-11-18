# GitHub page media

The page uses the actual Zipshot editor layout, toolbar, resources and native
fonts. Its input is a generated review document containing an excerpt of the
public crop implementation, rather than an invented dashboard or personal file.
The blue annotations come from the editor's own shape tools.

The documentation helper renders the production Direct2D drawing methods to a
software WIC surface, then reads its pixels and the real Win32 window frame.
This avoids unreliable GPU `PrintWindow` readback from an offscreen window.
It does not change the production source, installed settings, mouse, keyboard,
focus, clipboard or desktop contents. These are scripted documentation renders,
not recordings or performance measurements of interactive editing.

* `editor-overview.png`: full editor window, 1500×940.
* `annotation-flow.png`: four real editor states, 1200×752, displayed for
  1.6 / 1.4 / 1.4 / 3.0 seconds. It is an APNG with an ordinary `.png` extension
  so GitHub can embed it directly.
* `capture-review.png`: browser-rendered source document used as the input.
* `annotated-result.png`: actual editor export.
* `capture-benchmark.png`: a separate numerical figure from the recorded JSON,
  using Ubuntu Regular/Medium/Bold, neutral Fluent surfaces and one blue accent.
  It is not a screenshot. Font files are not bundled.

Rebuild on Windows with Python, Pillow, Pygments, Chrome and MSVC:

```bat
tools\readme_capture.bat
python tools\render_readme_media.py
python tools\render_readme_benchmark.py
```

The benchmark generator expects installed Ubuntu fonts in `C:/Windows/Fonts`;
`--font-dir` can select another local directory. Manifests record image hashes,
source provenance, font choices and animation timing.
