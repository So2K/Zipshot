"""Render the README capture figure from the recorded benchmark JSON.

Requires Python 3.10+, Pillow, and locally installed Ubuntu Regular/Medium/Bold
TTF fonts. The default font directory is C:/Windows/Fonts; use --font-dir to
select another local directory. Fonts are used only for rasterization: this
script does not copy or embed any font binaries in the repository.

Example, from any directory:
    python tools/render_readme_benchmark.py --font-dir C:/Windows/Fonts
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, SCALE = 1400, 650, 2
SURFACE, BORDER = "#27282B", "#3D3E42"
TEXT, MUTED, DIM = "#EEF0F3", "#B8BDC5", "#969DA8"
BLUE, NEUTRAL_BAR, TRACK = "#3A8DFF", "#8E96A3", "#34373D"


def render(data_path: Path, output: Path, metadata: Path, font_dir: Path) -> None:
    raw_data = data_path.read_bytes()
    data = json.loads(raw_data)
    zipshot = data["summary"]["zipshot"]
    sharex = data["summary"]["sharex"]
    version = data["provenance"]["sharex_version"]
    ratio = sharex["median_ms"] / zipshot["median_ms"]
    if not math.isclose(ratio, data["summary"]["median_speedup"], rel_tol=1e-12):
        raise ValueError("The reported ratio does not match the recorded medians.")
    if zipshot["n"] != sharex["n"]:
        raise ValueError("The figure expects the same sample count for both apps.")
    dimensions = {(r["result"]["width"], r["result"]["height"]) for r in data["runs"]}
    if len(dimensions) != 1 or any(r["result"]["cursor"] for r in data["runs"]):
        raise ValueError("The figure expects matching dimensions and cursor capture off.")
    screen_w, screen_h = next(iter(dimensions))
    fonts = {
        weight: font_dir / f"Ubuntu-{weight}.ttf"
        for weight in ("Regular", "Medium", "Bold")
    }
    for path in fonts.values():
        if not path.is_file():
            raise FileNotFoundError(f"Install Ubuntu fonts or set --font-dir: {path}")

    image = Image.new("RGB", (WIDTH * SCALE, HEIGHT * SCALE), SURFACE)
    draw = ImageDraw.Draw(image)
    font_cache: dict[tuple[int, str], ImageFont.FreeTypeFont] = {}
    text_boxes: list[dict] = []

    def font(size: int, weight: str = "Regular") -> ImageFont.FreeTypeFont:
        key = size, weight
        if key not in font_cache:
            font_cache[key] = ImageFont.truetype(str(fonts[weight]), size * SCALE)
        return font_cache[key]

    def text(x: int, y: int, content: str, size: int = 24, color: str = TEXT,
             weight: str = "Regular", right: bool = False) -> None:
        face = font(size, weight)
        anchor = "rt" if right else "lt"
        box = draw.textbbox((x * SCALE, y * SCALE), content, font=face, anchor=anchor)
        if box[0] < 24 * SCALE or box[1] < 20 * SCALE or box[2] > (WIDTH - 24) * SCALE or box[3] > (HEIGHT - 20) * SCALE:
            raise ValueError(f"Text would leave the figure's safe area: {content}")
        for previous in text_boxes:
            old = previous["bounds_px"]
            if max(box[0] / SCALE, old[0]) < min(box[2] / SCALE, old[2]) and max(box[1] / SCALE, old[1]) < min(box[3] / SCALE, old[3]):
                raise ValueError(f"Text overlaps: {content!r} / {previous['text']!r}")
        draw.text((x * SCALE, y * SCALE), content, fill=color, font=face, anchor=anchor)
        text_boxes.append({"text": content, "font": f"Ubuntu {weight}", "size_px": size,
                           "bounds_px": [v / SCALE for v in box]})

    def line(y: int) -> None:
        draw.line((52 * SCALE, y * SCALE, 1348 * SCALE, y * SCALE), fill=BORDER, width=SCALE)

    def rounded(rect: tuple[float, float, float, float], color: str, radius: int = 6) -> None:
        draw.rounded_rectangle(tuple(round(v * SCALE) for v in rect), radius=radius * SCALE, fill=color)

    draw.rounded_rectangle((0, 0, WIDTH * SCALE - 1, HEIGHT * SCALE - 1), radius=16 * SCALE,
                           fill=SURFACE, outline=BORDER, width=SCALE)
    rounded((52, 44, 56, 84), BLUE, 2)
    text(76, 43, "Capture API latency", 42, weight="Medium")
    text(52, 108, f"Same {screen_w} × {screen_h} desktop · {zipshot['n']} measured captures per app · Lower is faster", 24, MUTED)
    line(158)
    text(52, 185, "APPLICATION", 20, DIM, "Medium")
    text(440, 185, "MEDIAN CAPTURE TIME", 20, DIM, "Medium")
    text(1348, 185, "95TH PERCENTILE", 20, DIM, "Medium", right=True)

    maximum = max(zipshot["median_ms"], sharex["median_ms"])
    scale_ceiling = math.ceil(maximum / 10) * 10
    bar_start, bar_width = 440, 570
    bar_geometry = []
    for name, hint, values, y, color in (
        ("Zipshot", "Warm capture API", zipshot, 247, BLUE),
        (f"ShareX {version}", "Installed version", sharex, 365, NEUTRAL_BAR),
    ):
        text(52, y, name, 32, weight="Medium")
        text(52, y + 44, hint, 21, MUTED)
        rounded((bar_start, y + 15, bar_start + bar_width, y + 27), TRACK)
        actual_width = bar_width * values["median_ms"] / scale_ceiling
        rounded((bar_start, y + 15, bar_start + actual_width, y + 27), color)
        text(1132, y + 2, f"{values['median_ms']:.1f} ms", 30, weight="Medium", right=True)
        text(1348, y + 2, f"{values['p95_ms']:.1f} ms", 30, MUTED, right=True)
        bar_geometry.append({"application": name, "median_ms": values["median_ms"],
                             "bar_width_px": actual_width, "scale_ceiling_ms": scale_ceiling})

    line(449)
    text(52, 478, f"Measured median ratio: {ratio:.1f}× in this test", 27, weight="Medium")
    text(52, 532, f"Installed ShareX {version} · Warm API only; excludes hotkeys, UI, encoding and saving.", 22, MUTED)
    cpu = re.sub(r"\s+\d+-Core Processor$", "", data["machine"]["cpu"].strip()).removeprefix("AMD ")
    gpu = data["machine"]["gpu"][0].removeprefix("NVIDIA GeForce ")
    os_match = re.search(r"Windows \d+", data["machine"]["os"])
    os_name = os_match.group() if os_match else data["machine"]["os"]
    text(52, 570, f"{cpu} · {gpu} · {os_name}, build {data['machine']['build']} · Cursor capture off", 21, MUTED)
    text(52, 608, "Source: docs/benchmark/capture-results.json", 20, DIM)

    output.parent.mkdir(parents=True, exist_ok=True)
    image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS).save(output, optimize=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    metadata.write_text(json.dumps({
        "schema_version": 1,
        "figure": "capture-benchmark.png",
        "dimensions_px": [WIDTH, HEIGHT],
        "design": "Single neutral Fluent-style surface; Ubuntu labels; native Zipshot blue accent; no app UI recreation.",
        "palette": {"surface": SURFACE, "border": BORDER, "accent": BLUE},
        "fonts": [p.name for p in fonts.values()],
        "font_policy": "Use locally installed fonts for rasterization; no font files copied or embedded.",
        "source_data_sha256": hashlib.sha256(raw_data).hexdigest(),
        "figure_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "benchmark_utc": data["utc"],
        "summary": data["summary"],
        "bar_geometry": bar_geometry,
        "validation": {"all_text_in_safe_area": True, "no_text_box_overlap": True,
                       "shared_linear_bar_scale": True, "source_medians_and_sample_counts_used": True},
        "text_boxes": text_boxes,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Rendered {output.name}: {WIDTH}x{HEIGHT}; {ratio:.4f}x measured median ratio.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, default=ROOT / "docs/benchmark/capture-results.json")
    parser.add_argument("--output", type=Path, default=ROOT / "media/page/capture-benchmark.png")
    parser.add_argument("--metadata", type=Path, default=ROOT / "media/page/benchmark-design.json")
    parser.add_argument("--font-dir", type=Path, default=Path("C:/Windows/Fonts"))
    args = parser.parse_args()
    render(args.data, args.output, args.metadata, args.font_dir)


if __name__ == "__main__":
    main()
