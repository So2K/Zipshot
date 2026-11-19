"""Shared synthetic editor fixtures and timestamped input paths. No desktop access."""
from pathlib import Path
import hashlib
import json
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/runtime-compare/editor-fixtures'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    entries = []
    for width, height in [(1920, 1080), (3840, 2160)]:
        # Fine texture inside a readable review sheet makes blur visibly verifiable.
        yy, xx = np.mgrid[0:height, 0:width]
        rgb = np.empty((height, width, 3), np.uint8)
        rgb[:, :, 0] = 26 + ((xx // 16 + yy // 16) % 2) * 6
        rgb[:, :, 1] = 32 + ((xx * 3 + yy * 7) % 31)
        rgb[:, :, 2] = 44 + ((xx * 5 + yy * 3) % 37)
        im = Image.fromarray(rgb)
        d = ImageDraw.Draw(im)
        scale = width / 1920
        font = lambda size: ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', round(size * scale))
        f = font(28)
        d.rectangle((round(80*scale), round(70*scale), round(1840*scale), round(1010*scale)), fill='#e9edf2')
        d.text((110*scale, 94*scale), 'Editor performance review', font=font(48), fill='#182333')
        d.text((110*scale, 162*scale), 'Synthetic data only  |  Identical PNG input for both applications', font=font(26), fill='#526278')
        for i in range(12):
            y = round((240 + i * 55) * scale)
            d.line((110*scale, y+41*scale, 1810*scale, y+41*scale), fill='#c6cdd6', width=max(1,round(scale)))
            d.text((110*scale, y), f'{i+1:02}  Review item {i+1:02}    Pending', font=f, fill='#24334a')
            d.text((820*scale, y), f'Example value {137 + i*29}', font=f, fill='#386d9c')
            for j in range(20):
                x = round((1350+j*20)*scale)
                d.rectangle((x,y+8*scale,x+10*scale,y+30*scale), fill=('#2470b6' if (i+j)%3 else '#c4d8e9'))
        d.text((110*scale, 940*scale), 'No account names, private files, clipboard contents or desktop pixels.', font=font(24), fill='#526278')
        path = OUT / f'editor-fixture-{width}x{height}.png'
        im.save(path, optimize=True)
        points = [{'time_ms': round(i*2000/119, 6), 'x': round(width*(.15+.7*i/119),6),
                   'y': round(height*(.5+.18*np.sin(i*6*np.pi/119)),6)} for i in range(120)]
        entries.append({'width':width,'height':height,'file':path.name,'bytes':path.stat().st_size,
                        'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                        'arrow':{'from':[width*.2,height*.75],'to':[width*.8,height*.25],'duration_ms':2000,
                                 'width_px':4,'argb':'FFFF0000','shadow':False},
                        'freehand':{'duration_ms':2000,'width_px':4,'argb':'FFFF0000','shadow':False,'points':points},
                        'blur':{'rectangle':[width*.25,height*.3,width*.5,height*.4],'radius_setting':15},
                        'zoom':{'factors':[1.0,1.2,1.44,1.728,1.44,1.2,1.0],'anchor':[width*.5,height*.5]}})
    manifest = {'schema_version':1,'source':'Locally generated review sheet, no private content.',
                'fixture_generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'fixtures':entries}
    (OUT/'editor-fixture-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps([{k:e[k] for k in ['file','width','height','bytes','sha256']} for e in entries],indent=2))


if __name__ == '__main__':
    main()
