"""Fetch the emoji sticker packs Zipshot offers in the sticker picker.

Reproducible: run it again to rebuild <dest>/<Pack>/ from the official sources only.

  python -I tools/fetch_stickers.py --dest C:/Users/User/Tools/Zipshot/Stickers --work <scratch dir>
         [--packs fluent3d,fluentflat,noto,twemoji,notoanim] [--chrome <chrome.exe>] [--ffmpeg <ffmpeg.exe>]

Packs (each folder gets the pack's LICENSE, index.tsv and pack.ini):
  Fluent3D      microsoft/fluentui-emoji, MIT          3D PNGs (256 px)
  FluentFlat    microsoft/fluentui-emoji, MIT          "Color" SVGs rendered to 256 px PNGs
  Noto          googlefonts/noto-emoji, Apache-2.0     png/512 downscaled to 256 px
  Twemoji       jdecked/twemoji, graphics CC-BY 4.0    assets/svg rendered to 256 px PNGs
  NotoAnimated  googlefonts noto-emoji-animation, CC-BY 4.0   512.gif downscaled to 160 px GIFs

index.tsv: file <TAB> name <TAB> group <TAB> keywords (comma separated). Groups use the Fluent (CLDR) group
names for every pack so the picker can show the same category tabs. Skin-tone variants are skipped.

Downloaded files are treated as data: only images and JSON are read; nothing from them is executed.
SVGs are rendered by headless Chrome as <img> elements (no script runs inside an <img> SVG).
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.request
from pathlib import Path

from PIL import Image

SKIN = {"1f3fb", "1f3fc", "1f3fd", "1f3fe", "1f3ff"}
GROUP_ORDER = ["Smileys & Emotion", "People & Body", "Animals & Nature", "Food & Drink", "Travel & Places",
               "Activities", "Objects", "Symbols", "Flags"]
ANIM_GROUPS = {"Smileys and emotions": "Smileys & Emotion", "People": "People & Body",
               "Animals and nature": "Animals & Nature", "Food and drink": "Food & Drink",
               "Travel and places": "Travel & Places", "Activities and events": "Activities",
               "Objects": "Objects", "Symbols": "Symbols", "Flags": "Flags"}
UA = "Zipshot-fetch-stickers/1.0 (+https://github.com)"


def log(*a):
    print(*a, flush=True)


def run(cmd, cwd=None):
    log("  $", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def sparse_clone(url, dst: Path, patterns):
    if INDEX_ONLY and (dst / ".git").exists():
        return
    if not (dst / ".git").exists():
        if dst.exists():
            shutil.rmtree(dst)
        run(["git", "clone", "--depth", "1", "--filter=blob:none", "--no-checkout", url, dst])
    run(["git", "-C", dst, "sparse-checkout", "set", "--no-cone", *patterns])
    run(["git", "-C", dst, "checkout"])


def key(seq):
    """Normalised codepoint key: lowercase hex joined by '-', without fe0f."""
    parts = [p.lower().lstrip("0") or "0" for p in seq.replace("_", " ").replace("-", " ").split()]
    return "-".join(p for p in parts if p != "fe0f")


def has_skin(k):
    return any(p in SKIN for p in k.split("-"))


def uname(k):
    try:
        return " ".join(unicodedata.name(chr(int(p, 16))).lower() for p in k.split("-") if p not in ("200d",))
    except (ValueError, KeyError):
        return k


def write_index(folder: Path, rows):
    rows.sort(key=lambda r: (GROUP_ORDER.index(r[2]) if r[2] in GROUP_ORDER else 99, r[3]))
    with open(folder / "index.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write("# file\tname\tgroup\tkeywords\n")
        for file, name, group, _order, kw in rows:
            clean = lambda s: s.replace("\t", " ").replace("\n", " ").strip()
            f.write(f"{file}\t{clean(name)}\t{clean(group)}\t{clean(','.join(kw))}\n")


def write_ini(folder: Path, name, name_ru, order, animated=False, source="", licence="", short=""):
    with open(folder / "pack.ini", "w", encoding="utf-16") as f:  # GetPrivateProfileString reads UTF-16 ini files
        f.write(f"[pack]\nname={name}\nname_ru={name_ru}\nshort={short or name}\norder={order}\nanimated={1 if animated else 0}\n"
                f"source={source}\nlicense={licence}\n")


INDEX_ONLY = False


def fresh_dir(p: Path):
    if INDEX_ONLY:
        return
    if p.exists():
        shutil.rmtree(p)
    p.mkdir(parents=True)


# ----------------------------------------------------------------------------- Fluent metadata (shared names)

def fluent_meta(repo: Path, pop):
    """codepoint key -> (name, group, order, keywords, folder)"""
    meta = {}
    for d in sorted((repo / "assets").iterdir()):
        mf = d / "metadata.json"
        if not mf.is_file():
            continue
        try:
            m = json.loads(mf.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            continue
        uni = m.get("unicode") or ""
        if not uni:
            continue
        k = key(uni)
        kw = [w for w in m.get("keywords", []) if isinstance(w, str)]
        # Popular first (Noto animation popularity), then codepoint order, which keeps related emoji together.
        order = -pop[k] if k in pop else 1_000_000 + int(k.split("-")[0], 16) * 4 + len(k.split("-"))
        meta[k] = (str(m.get("cldr") or d.name).strip(), str(m.get("group") or ""), order, kw, d)
    return meta


def lookup(meta, k):
    if k in meta:
        return meta[k]
    if k + "-20e3" in meta:
        return meta[k + "-20e3"]
    return None


# ----------------------------------------------------------------------------- SVG rendering with headless Chrome

def render_svgs(chrome, jobs, size, work: Path):
    """jobs: list of (svg Path, out png Path). Renders sheets of 8x8 and slices them."""
    jobs = [j for j in jobs if not j[1].exists()]
    if INDEX_ONLY:
        return
    if not jobs:
        return
    sheet_dir = work / "_sheets"
    fresh_dir(sheet_dir)
    profile = Path(tempfile.mkdtemp(prefix="zs-chrome-"))
    n = 8
    for start in range(0, len(jobs), n * n):
        batch = jobs[start:start + n * n]
        html = ["<!doctype html><html><head><style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}"
                f"img{{position:absolute;width:{size}px;height:{size}px}}</style></head><body>"]
        for i, (svg, _out) in enumerate(batch):
            x, y = (i % n) * size, (i // n) * size
            html.append(f'<img style="left:{x}px;top:{y}px" src="{svg.resolve().as_uri()}">')
        html.append("</body></html>")
        page = sheet_dir / f"s{start}.html"
        page.write_text("".join(html), encoding="utf-8")
        shot = sheet_dir / f"s{start}.png"
        cmd = [chrome, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
               "--hide-scrollbars", "--force-device-scale-factor=1", f"--user-data-dir={profile}",
               "--allow-file-access-from-files", "--default-background-color=00000000",
               f"--window-size={n * size},{n * size}", "--virtual-time-budget=8000",
               f"--screenshot={shot}", page.resolve().as_uri()]
        subprocess.run([str(c) for c in cmd], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=180)
        if not shot.exists():
            log("  sheet failed", start)
            continue
        im = Image.open(shot).convert("RGBA")
        for i, (_svg, out) in enumerate(batch):
            x, y = (i % n) * size, (i // n) * size
            cell = im.crop((x, y, x + size, y + size))
            if cell.getbbox():
                cell.save(out, optimize=True)
        log(f"  rendered {min(start + n * n, len(jobs))}/{len(jobs)}")
    shutil.rmtree(profile, ignore_errors=True)
    shutil.rmtree(sheet_dir, ignore_errors=True)


def save_png(src: Path, out: Path, size):
    if INDEX_ONLY:
        return
    im = Image.open(src).convert("RGBA")
    if max(im.size) > size:
        im.thumbnail((size, size), Image.LANCZOS)
    im.save(out, optimize=True)


# ----------------------------------------------------------------------------- packs

def pack_fluent(args, meta, repo, style):
    flat = style == "flat"
    folder = args.dest / ("FluentFlat" if flat else "Fluent3D")
    fresh_dir(folder)
    if not INDEX_ONLY:
        shutil.copy(repo / "LICENSE", folder / "LICENSE")
    rows, jobs = [], []
    for k, (name, group, order, kw, d) in meta.items():
        if group == "Component":
            continue
        sub = "Color" if flat else "3D"
        ext = ".svg" if flat else ".png"
        cands = list((d / sub).glob("*" + ext)) or list((d / "Default" / sub).glob("*" + ext))
        if not cands:
            continue
        fname = k + ".png"
        if flat:
            jobs.append((cands[0], folder / fname))
        else:
            save_png(cands[0], folder / fname, 256)
        rows.append((fname, name, group, order, kw))
    if flat:
        render_svgs(args.chrome, jobs, 256, args.work)
    rows = [r for r in rows if (folder / r[0]).exists()]
    write_index(folder, rows)
    write_ini(folder, "Fluent Flat" if flat else "Fluent 3D", "Fluent плоские" if flat else "Fluent 3D",
              30 if flat else 20, source="https://github.com/microsoft/fluentui-emoji", licence="MIT",
              short="Fluent" if flat else "Fluent 3D")
    log(f"{folder.name}: {len(rows)}")


def pack_noto(args, meta, repo):
    folder = args.dest / "Noto"
    fresh_dir(folder)
    if not INDEX_ONLY:
        shutil.copy(repo / "LICENSE", folder / "LICENSE")
    src = next((d for d in (repo / "2D" / "png" / "512", repo / "png" / "512", repo / "png" / "128") if d.is_dir()), None)
    if src is None:
        raise SystemExit("noto-emoji: no png folder found")
    rows = []
    for f in sorted(src.glob("emoji_u*.png")):
        k = key(f.stem[len("emoji_u"):])
        if has_skin(k):
            continue
        m = lookup(meta, k)
        name, group, order, kw = (m[0], m[1], m[2], m[3]) if m else (uname(k), "Symbols", 9_000_000, [])
        if group == "Component":
            continue
        fname = k + ".png"
        save_png(f, folder / fname, 256)
        rows.append((fname, name, group, order, kw))
    write_index(folder, rows)
    write_ini(folder, "Noto Emoji", "Noto Emoji", 40, source="https://github.com/googlefonts/noto-emoji",
              licence="Apache-2.0", short="Noto")
    log(f"Noto: {len(rows)}")


def pack_twemoji(args, meta, repo):
    folder = args.dest / "Twemoji"
    fresh_dir(folder)
    for lic in ("LICENSE-GRAPHICS", "LICENSE"):
        if (repo / lic).exists() and not INDEX_ONLY:
            shutil.copy(repo / lic, folder / lic)
    rows, jobs = [], []
    for f in sorted((repo / "assets" / "svg").glob("*.svg")):
        k = key(f.stem)
        if has_skin(k):
            continue
        m = lookup(meta, k)
        if not m and len(k.split("-")) == 2 and all(0x1f1e6 <= int(p, 16) <= 0x1f1ff for p in k.split("-")):
            letters = "".join(chr(int(p, 16) - 0x1f1e6 + 65) for p in k.split("-"))
            m = ("flag " + letters, "Flags", 8_000_000, [letters.lower()])
        name, group, order, kw = (m[0], m[1], m[2], m[3]) if m else (uname(k), "Symbols", 9_000_000, [])
        if group == "Component":
            continue
        fname = k + ".png"
        jobs.append((f, folder / fname))
        rows.append((fname, name, group, order, kw))
    render_svgs(args.chrome, jobs, 256, args.work)
    rows = [r for r in rows if (folder / r[0]).exists()]
    write_index(folder, rows)
    write_ini(folder, "Twemoji", "Twemoji", 50, source="https://github.com/jdecked/twemoji",
              licence="CC-BY-4.0 (graphics), MIT (code)", short="Twemoji")
    log(f"Twemoji: {len(rows)}")


CC_BY = """Noto Animated Emoji by Google (https://googlefonts.github.io/noto-emoji-animation/)
are licensed under the Creative Commons Attribution 4.0 International License (CC BY 4.0):
https://creativecommons.org/licenses/by/4.0/legalcode

Changes: the 512 px GIFs were downscaled to 160 px for use as stickers in Zipshot.
"""


def anim_api(work: Path):
    dl = work / "notoanim_dl"
    dl.mkdir(parents=True, exist_ok=True)
    api = dl / "api.json"
    if not api.exists():
        req = urllib.request.Request("https://googlefonts.github.io/noto-emoji-animation/data/api.json",
                                     headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=60) as r:
            api.write_bytes(r.read())
    return json.loads(api.read_text(encoding="utf-8")).get("icons", [])


def popularity(work: Path):
    pop = {}
    for ic in anim_api(work):
        cp = str(ic.get("codepoint", ""))
        p = ic.get("popularity", 0)
        if cp and isinstance(p, int):
            pop[key(cp)] = p
    return pop


def fetch(url, dst: Path):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
            if not data.startswith(b"GIF8"):
                return False
            dst.write_bytes(data)
            return True
        except Exception as e:  # noqa: BLE001
            log("  retry", url, e)
            time.sleep(2 + attempt * 3)
    return False


def pack_noto_animated(args, meta):
    folder = args.dest / "NotoAnimated"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "LICENSE").write_text(CC_BY, encoding="utf-8")
    dl = args.work / "notoanim_dl"
    icons = anim_api(args.work)
    todo = []
    rows = []
    for ic in icons:
        cp = str(ic.get("codepoint", ""))
        if not cp or not all(c in "0123456789abcdef_" for c in cp.lower()):
            continue
        k = key(cp)
        if has_skin(k):
            continue
        m = lookup(meta, k)
        group = ANIM_GROUPS.get((ic.get("categories") or [""])[0], m[1] if m else "Symbols")
        tags = [t.strip(":").replace("-", " ") for t in ic.get("tags", []) if isinstance(t, str)]
        name = m[0] if m else (tags[0] if tags else uname(k))
        kw = (m[3] if m else []) + tags
        fname = k + ".gif"
        rows.append((fname, name, group, -int(ic.get("popularity", 0) or 0), kw))
        if not (folder / fname).exists() and not INDEX_ONLY:
            todo.append((cp, fname))

    def one(item):
        cp, fname = item
        raw = dl / (fname + ".512.gif")
        if not raw.exists() and not fetch(f"https://fonts.gstatic.com/s/e/notoemoji/latest/{cp}/512.gif", raw):
            return fname, False
        out = folder / fname
        tmp = folder / (fname + ".tmp.gif")
        vf = (f"scale={args.anim_size}:{args.anim_size}:flags=lanczos,split[a][b];"
              "[a]palettegen=reserve_transparent=1:stats_mode=full[p];[b][p]paletteuse=alpha_threshold=128:dither=none")
        r = subprocess.run([str(args.ffmpeg), "-v", "error", "-y", "-i", str(raw), "-filter_complex", vf, "-loop", "0",
                            "-f", "gif", str(tmp)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0 or not tmp.exists():
            log("  ffmpeg failed", fname, r.stderr[-300:])
            return fname, False
        os.replace(tmp, out)
        raw.unlink()
        return fname, True

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        for fname, ok in ex.map(one, todo):
            done += 1
            if done % 50 == 0 or not ok:
                log(f"  anim {done}/{len(todo)} {fname} {'ok' if ok else 'FAILED'}")
    rows = [r for r in rows if (folder / r[0]).exists()]
    write_index(folder, rows)
    write_ini(folder, "Noto Animated", "Noto анимированные", 60, animated=True,
              source="https://googlefonts.github.io/noto-emoji-animation/", licence="CC-BY-4.0", short="GIF")
    log(f"NotoAnimated: {len(rows)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--packs", default="fluent3d,fluentflat,noto,twemoji,notoanim")
    ap.add_argument("--chrome", default=r"C:\Program Files\Google\Chrome\Application\chrome.exe")
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    ap.add_argument("--anim-size", type=int, default=160)
    ap.add_argument("--index-only", action="store_true", help="rebuild index.tsv/pack.ini from the files already there")
    args = ap.parse_args()
    global INDEX_ONLY
    INDEX_ONLY = args.index_only
    args.dest.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    packs = set(args.packs.split(","))

    fluent = args.work / "fluentui-emoji"
    sparse_clone("https://github.com/microsoft/fluentui-emoji", fluent,
                 ["/LICENSE", "/assets/*/metadata.json", "/assets/*/3D/*.png", "/assets/*/Default/3D/*.png",
                  "/assets/*/Color/*.svg", "/assets/*/Default/Color/*.svg"])
    meta = fluent_meta(fluent, popularity(args.work))
    log(f"fluent metadata: {len(meta)}")
    if "fluent3d" in packs:
        pack_fluent(args, meta, fluent, "3d")
    if "fluentflat" in packs:
        pack_fluent(args, meta, fluent, "flat")
    if "noto" in packs:
        noto = args.work / "noto-emoji"
        sparse_clone("https://github.com/googlefonts/noto-emoji", noto, ["/LICENSE", "/2D/png/512/*.png"])
        pack_noto(args, meta, noto)
    if "twemoji" in packs:
        tw = args.work / "twemoji"
        sparse_clone("https://github.com/jdecked/twemoji", tw, ["/LICENSE", "/LICENSE-GRAPHICS", "/assets/svg/*.svg"])
        pack_twemoji(args, meta, tw)
    if "notoanim" in packs:
        pack_noto_animated(args, meta)


if __name__ == "__main__":
    sys.exit(main())
