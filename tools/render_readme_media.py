"""Capture real Zipshot documentation images without desktop/input access.

Requires Pillow, Pygments, Chrome and the MSVC toolchain used by build.bat.
Run `tools/readme_capture.bat`, then `python tools/render_readme_media.py`.
Only the documentation helper is executed; release/installed binaries are untouched.
"""
import argparse
import hashlib
import html
import json
import subprocess
from pathlib import Path

from PIL import Image
from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import CppLexer

ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / 'media/page'
WORK = ROOT / 'build/readme-media'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--chrome', type=Path, default=Path('C:/Program Files/Google/Chrome/Application/chrome.exe'))
    args = parser.parse_args()
    PAGE.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    source = (ROOT/'src/capture.cpp').read_text(encoding='utf-8').splitlines()
    excerpt = '\n'.join(source[9:21])
    formatter = HtmlFormatter(nowrap=True, style='native')
    code = highlight(excerpt, CppLexer(), formatter)
    rows = code.rstrip('\n').splitlines()
    numbered = ''.join(f'<div class="line"><span class="number">{i+10}</span><span>{line}</span></div>' for i,line in enumerate(rows))
    document = '''<!doctype html><html lang="en"><meta charset="utf-8"><title>Capture review</title>
<style>
*{box-sizing:border-box}html,body{margin:0;width:1280px;height:720px;overflow:hidden;background:#202124;color:#eee;font:19px "Segoe UI",sans-serif}
header{padding:36px 64px 25px;border-bottom:1px solid #383a3d}header h1{font-size:28px;font-weight:600;margin:0 0 9px}header p{font-size:16px;color:#aaa;margin:0}
main{padding:24px 64px}pre{margin:0;padding:8px 22px 24px;background:#18191b;border:1px solid #3b3d40;border-radius:5px;overflow:hidden;font:20px/30px Consolas,monospace}
.line{height:30px;display:flex;white-space:pre}.number{color:#7c818a;display:inline-block;flex:0 0 56px;user-select:none}footer{padding:26px 64px;color:#a7abb2;font-size:16px;border-top:1px solid #383a3d}
''' + formatter.get_style_defs('.code') + '''
</style><header><h1>Preserve transparency when cropping</h1><p>Zipshot / src/capture.cpp &nbsp; · &nbsp; Code review excerpt</p></header>
<main><pre class="code">''' + numbered + '''</pre></main>
<footer>Review note: a cropped image should keep its source alpha channel.</footer></html>'''
    local_html=WORK/'capture-review.html'
    local_html.write_text(document,encoding='utf-8')
    fixture=PAGE/'capture-review.png'
    command=[str(args.chrome),'--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--hide-scrollbars',
             '--force-device-scale-factor=1','--user-data-dir='+str(WORK/'source-profile'),'--screenshot='+str(fixture),
             '--window-size=1280,720','--virtual-time-budget=800',local_html.as_uri()]
    result=subprocess.run(command,capture_output=True,timeout=30,creationflags=0x08000000)
    if result.returncode or not fixture.exists(): raise RuntimeError('Headless document capture failed')
    native=ROOT/'build/readme-native/zipshot-demo.exe'
    if not native.exists(): raise RuntimeError('Build tools/readme_capture.bat first')
    (native.parent/'zipshot.ini').write_text('[main]\nLanguage=en\nShareXImported=1\nSound=0\n[hotkeys]\nRegion=\n',encoding='utf-16')
    frames=WORK/'editor-states'
    frames.mkdir(exist_ok=True)
    subprocess.run([str(native),'--test-demo',str(fixture),str(frames)],check=True,timeout=20,creationflags=0x08000000)
    states=[Image.open(frames/f'editor-{i}.png').convert('RGB') for i in range(4)]
    # No decorative shell, gradients, masks, UI repainting or text overlays on these real window captures.
    states[-1].save(PAGE/'editor-overview.png',optimize=True)
    frames_out=[image.resize((1200,752),Image.Resampling.LANCZOS) for image in states]
    frames_out[0].save(PAGE/'annotation-flow.png',save_all=True,append_images=frames_out[1:],duration=[1600,1400,1400,3000],loop=0,disposal=0,blend=0,optimize=True)
    Image.open(frames/'result.png').save(PAGE/'annotated-result.png',optimize=True)
    manifest={'source':'Generated review document containing the actual public Zipshot crop implementation. No user files, desktop pixels or clipboard data.',
              'source_file':'src/capture.cpp','source_excerpt_lines':[10,21],
              'source_sha256':sha(ROOT/'src/capture.cpp'),'fixture':'capture-review.png',
              'native_ui':'Production Direct2D editor drawing/layout/toolbar methods on an offscreen WIC surface, with a captured real Win32 non-client frame. Documentation helper only; no focus or input automation.',
              'native_font':'Segoe UI / production app fonts; external figure labels use Ubuntu.',
              'animation':'Four scripted actual editor states, explanatory timing; not a speed measurement.',
              'frame_durations_ms':[1600,1400,1400,3000],
              'images':{name:sha(PAGE/name) for name in ['capture-review.png','editor-overview.png','annotation-flow.png','annotated-result.png']}}
    (PAGE/'capture-design.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'captured':list(manifest['images']),'native_size':states[0].size,'apng_frames':len(frames_out)},indent=2))


if __name__=='__main__': main()
