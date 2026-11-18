"""Render a mobile-readable capture benchmark figure from the measured JSON.

Requires Python 3.10+, Pillow, and locally installed Ubuntu Regular and Medium
TTF fonts. Use --font-dir to override C:/Windows/Fonts. Font binaries are used
for rasterization only and are never copied into the repository.

    python tools/render_benchmark_mobile.py

The 840x1000 figure is designed for a 320 CSS-pixel display width. Its smallest
text is 36 source pixels (approximately 13.7 CSS pixels at that display size).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, SCALE = 840, 1000, 2
BG, BORDER, TRACK = "#181A1E", "#3D3E42", "#30343B"
INK, MUTED, BLUE, NEUTRAL = "#EEF0F3", "#B8BDC5", "#3A8DFF", "#8E96A3"


def render(data_path: Path, output: Path, metadata_path: Path, font_dir: Path) -> None:
    source = data_path.read_bytes()
    data = json.loads(source)
    zipshot, sharex = data["summary"]["zipshot"], data["summary"]["sharex"]
    version = data["provenance"]["sharex_version"]
    ratio = sharex["median_ms"] / zipshot["median_ms"]
    if not math.isclose(ratio, data["summary"]["median_speedup"], rel_tol=1e-12):
        raise ValueError("Recorded ratio does not match the measured medians.")
    if zipshot["n"] != sharex["n"]:
        raise ValueError("Both applications must have the same sample count.")
    dimensions = {(run["result"]["width"], run["result"]["height"]) for run in data["runs"]}
    if len(dimensions) != 1 or any(run["result"]["cursor"] for run in data["runs"]):
        raise ValueError("Expected a common desktop size with cursor capture off.")
    screen_w, screen_h = next(iter(dimensions))
    font_files = {weight: font_dir / f"Ubuntu-{weight}.ttf" for weight in ("Regular", "Medium")}
    for path in font_files.values():
        if not path.is_file():
            raise FileNotFoundError(f"Install Ubuntu fonts or provide --font-dir: {path}")

    image = Image.new("RGB", (WIDTH * SCALE, HEIGHT * SCALE), BG)
    draw = ImageDraw.Draw(image)
    font_cache: dict[tuple[int, str], ImageFont.FreeTypeFont] = {}
    boxes: list[dict] = []

    def text(x: int, y: int, content: str, size: int, *, color: str = INK,
             weight: str = "Regular", right: bool = False) -> None:
        key = size, weight
        if key not in font_cache:
            font_cache[key] = ImageFont.truetype(str(font_files[weight]), size * SCALE)
        face = font_cache[key]
        anchor = "rt" if right else "lt"
        bounds = tuple(v / SCALE for v in draw.textbbox((x * SCALE, y * SCALE), content, font=face, anchor=anchor))
        if bounds[0] < 32 or bounds[1] < 28 or bounds[2] > WIDTH - 32 or bounds[3] > HEIGHT - 28:
            raise ValueError(f"Text leaves the safe area: {content}")
        for old in boxes:
            b = old["bounds_px"]
            if max(bounds[0], b[0]) < min(bounds[2], b[2]) and max(bounds[1], b[1]) < min(bounds[3], b[3]):
                raise ValueError(f"Text overlaps: {content!r} / {old['text']!r}")
        draw.text((x * SCALE, y * SCALE), content, font=face, fill=color, anchor=anchor)
        boxes.append({"text": content, "font": f"Ubuntu {weight}", "size_px": size,
                      "size_at_320_css_px": size * 320 / WIDTH, "bounds_px": list(bounds)})

    def line(y: int) -> None:
        draw.line((58 * SCALE, y * SCALE, 782 * SCALE, y * SCALE), fill=BORDER, width=SCALE)

    def pill(bounds: tuple[float, float, float, float], color: str, radius: int = 8) -> None:
        draw.rounded_rectangle(tuple(round(v * SCALE) for v in bounds), radius=radius * SCALE, fill=color)

    draw.rounded_rectangle((0, 0, WIDTH * SCALE - 1, HEIGHT * SCALE - 1), radius=20 * SCALE,
                           fill=BG, outline=BORDER, width=SCALE)
    pill((58, 54, 62, 98), BLUE, 2)
    text(82, 54, "Capture API latency", 46, weight="Medium")
    text(58, 126, f"{ratio:.1f}×", 136, weight="Medium")
    text(58, 281, "faster in this test", 40, color=MUTED)
    line(342)

    scale_ceiling = math.ceil(max(zipshot["median_ms"], sharex["median_ms"]) / 10) * 10
    bar_width = 724
    geometry = []
    for name, values, y, color in (
        ("Zipshot", zipshot, 394, BLUE),
        (f"ShareX {version}", sharex, 566, NEUTRAL),
    ):
        text(58, y, name, 40, weight="Medium")
        text(782, y - 16, f"{values['median_ms']:.1f} ms", 70, weight="Medium", right=True)
        bar_y = y + 78
        actual_width = bar_width * values["median_ms"] / scale_ceiling
        pill((58, bar_y, 58 + bar_width, bar_y + 18), TRACK)
        pill((58, bar_y, 58 + actual_width, bar_y + 18), color)
        geometry.append({"application": name, "median_ms": values["median_ms"],
                         "bar_width_px": actual_width, "scale_ceiling_ms": scale_ceiling})

    line(714)
    text(58, 752, f"Medians · {zipshot['n']} captures per app", 36, color=MUTED)
    text(58, 806, f"{screen_w} × {screen_h} · Cursor off", 36, color=MUTED)
    text(58, 860, "Warm API only; excludes hotkeys,", 36, color=MUTED)
    text(58, 910, "UI, editing, encoding and saving.", 36, color=MUTED)

    output.parent.mkdir(parents=True, exist_ok=True)
    image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS).save(output, optimize=True)
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    metadata_path.write_text(json.dumps({
        "schema_version": 1,
        "figure": "benchmark-mobile.png",
        "dimensions_px": [WIDTH, HEIGHT],
        "target_display_width_css_px": 320,
        "design": "Vertical neutral Fluent-style data figure; Ubuntu typography; native Zipshot blue; no recreated app UI.",
        "palette": {"background": BG, "border": BORDER, "accent": BLUE},
        "fonts": [p.name for p in font_files.values()],
        "font_policy": "Locally installed fonts used only for rasterization; no font binaries copied or embedded.",
        "source": "docs/benchmark/capture-results.json",
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "figure_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "benchmark_utc": data["utc"],
        "sharex_version": version,
        "summary": data["summary"],
        "machine": data["machine"],
        "bar_geometry": geometry,
        "validation": {"all_text_in_safe_area": True, "no_text_box_overlap": True,
                       "shared_linear_bar_scale": True, "minimum_text_at_320_css_px": min(b["size_at_320_css_px"] for b in boxes)},
        "text_boxes": boxes,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Rendered {output.name}: {WIDTH}x{HEIGHT}; {ratio:.4f}x measured median ratio.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, default=ROOT / "docs/benchmark/capture-results.json")
    parser.add_argument("--output", type=Path, default=ROOT / "media/showcase/benchmark-mobile.png")
    parser.add_argument("--metadata", type=Path, default=ROOT / "media/showcase/benchmark-mobile.json")
    parser.add_argument("--font-dir", type=Path, default=Path("C:/Windows/Fonts"))
    args = parser.parse_args()
    render(args.data, args.output, args.metadata, args.font_dir)


if __name__ == "__main__":
    main()
