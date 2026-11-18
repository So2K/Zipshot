"""Build the GitHub launch graphics from genuine editor renders and measured data.

Requires Pillow and installed Ubuntu Regular/Medium/Bold in C:/Windows/Fonts.
This is deterministic product artwork, not a recreation of the application UI.
All interface pixels come from media/page. No desktop or user input is accessed.
"""
from pathlib import Path
import hashlib
import json
import math

from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'media/showcase'
FONTS = Path('C:/Windows/Fonts')
BG, PANEL, BORDER = '#13161d', '#1c2029', '#343c49'
TEXT, MUTED, BLUE = '#f4f6fa', '#acb5c6', '#3a8dff'
FONT_CACHE = {}
BOUNDS = {}
CAPTURE_DATA = json.loads((ROOT/'docs/benchmark/capture-results.json').read_text(encoding='utf-8-sig'))
CAPTURE_MS = CAPTURE_DATA['summary']['zipshot']['median_ms']


def font(size, weight='Regular'):
    key = size, weight
    if key not in FONT_CACHE:
        FONT_CACHE[key] = ImageFont.truetype(str(FONTS/f'Ubuntu-{weight}.ttf'), size)
    return FONT_CACHE[key]


def text(image, xy, value, size, color=TEXT, weight='Regular', tracking=0):
    draw=ImageDraw.Draw(image)
    face=font(size,weight)
    x,y=xy
    if tracking:
        for letter in value:
            draw.text((x,y),letter,font=face,fill=color,anchor='lt')
            x+=draw.textlength(letter,font=face)+tracking
        box=(xy[0],xy[1],x-tracking,xy[1]+size)
    else:
        box=draw.textbbox(xy,value,font=face,anchor='lt')
        draw.text(xy,value,font=face,fill=color,anchor='lt')
    if min(box)<0 or box[2]>image.width or box[3]>image.height:
        raise ValueError(f'Text outside canvas: {value}')
    BOUNDS.setdefault(image.info.get('name','image'),[]).append({'text':value,'bounds':list(box),'font':f'Ubuntu {weight}','size':size})


def canvas(name,size):
    im=Image.new('RGB',size,BG)
    im.info['name']=name
    return im


def native_icon(im,xy,size):
    icon=Image.open(ROOT/'res/app.png').convert('RGBA').resize((size,size),Image.Resampling.LANCZOS)
    im.paste(icon,xy,icon)


def editor_source():
    return Image.open(ROOT/'media/page/editor-overview.png').convert('RGB').crop((8,0,1492,932))


def capture_corners(draw,rect,length=30,width=4,color=BLUE):
    x0,y0,x1,y1=rect
    for x,dx in [(x0,1),(x1,-1)]:
        for y,dy in [(y0,1),(y1,-1)]:
            draw.line((x,y,x+dx*length,y),fill=color,width=width)
            draw.line((x,y,x,y+dy*length),fill=color,width=width)


def screenshot(im,rect):
    x,y,w,h=rect
    source=editor_source().resize((w,h),Image.Resampling.LANCZOS)
    shadow=Image.new('RGBA',im.size)
    d=ImageDraw.Draw(shadow)
    d.rounded_rectangle((x-8,y+9,x+w+9,y+h+22),radius=12,fill=(0,0,0,130))
    shadow=shadow.filter(ImageFilter.GaussianBlur(15))
    im.paste(shadow,(0,0),shadow)
    im.paste(source,(x,y))
    d=ImageDraw.Draw(im)
    d.rounded_rectangle((x-1,y-1,x+w,y+h),radius=8,outline='#566274',width=1)


def hero(mobile=False):
    name='hero-mobile.png' if mobile else 'hero-desktop.png'
    im=canvas(name,(840,1240) if mobile else (1600,1010))
    d=ImageDraw.Draw(im)
    if mobile:
        d.polygon([(650,0),(840,0),(840,1050),(470,1050)],fill='#18345a')
        native_icon(im,(42,40),64)
        text(im,(126,45),'Zipshot',51,weight='Bold')
        text(im,(598,61),'Windows x64',21,MUTED)
        for y,word in [(156,'Capture.'),(248,'Annotate.'),(340,'Done.')]:
            text(im,(42,y),word,87,BLUE if word=='Done.' else TEXT,'Bold')
        text(im,(46,458),'Native screenshots and annotations.',29,MUTED)
        screenshot(im,(38,542,764,480))
        capture_corners(d,(28,531,812,1032),24,3)
        d.line((42,1071,798,1071),fill=BORDER,width=2)
        stats=[(f'{CAPTURE_MS:.1f} ms','warm API test'),('1.9 MB','Windows EXE'),('GPLv3+','open source')]
        for i,(value,label) in enumerate(stats):
            x=44+i*265
            text(im,(x,1111),value,39,BLUE if i==0 else TEXT,'Medium')
            text(im,(x,1170),label,32,MUTED)
    else:
        d.polygon([(1070,0),(1600,0),(1600,792),(855,792)],fill='#18345a')
        d.line((1040,0,808,792),fill='#244d83',width=2)
        native_icon(im,(66,43),68)
        text(im,(154,49),'Zipshot',55,weight='Bold')
        text(im,(1120,69),'WINDOWS x64  /  FREE & OPEN SOURCE',17,MUTED,tracking=1)
        for y,word in [(214,'Capture.'),(326,'Annotate.'),(438,'Done.')]:
            text(im,(64,y),word,102,BLUE if word=='Done.' else TEXT,'Bold')
        text(im,(68,582),'Screenshots and annotations.',29,MUTED)
        text(im,(68,627),'One native Windows app.',29,MUTED)
        text(im,(614,155),'THE EDITOR',18,MUTED,'Medium',tracking=2)
        screenshot(im,(610,197,928,583))
        capture_corners(d,(598,184,1550,793),30,4)
        d.line((64,838,1536,838),fill=BORDER,width=2)
        stats=[(f'{CAPTURE_MS:.1f} ms','warm capture API'),('1.9 MB','Windows executable'),('GPLv3+','free and open source')]
        for i,(value,label) in enumerate(stats):
            x=68+i*520
            text(im,(x,871),value,49,BLUE if i==0 else TEXT,'Medium')
            text(im,(x,939),label,23,MUTED)
        text(im,(68,982),'Capture timing is from one machine. Optional FFmpeg is separate. Details below.',17,MUTED)
    im.save(OUT/name,optimize=True)
    return im


def keycap(im,x,y,label,width=None,size=28):
    draw=ImageDraw.Draw(im)
    width=width or int(draw.textlength(label,font=font(size,'Medium'))+44)
    height=size+38
    draw.rounded_rectangle((x,y,x+width,y+height),radius=11,fill='#303744',outline='#5d687b',width=2)
    draw.line((x+13,y+height-7,x+width-13,y+height-7),fill='#151a22',width=2)
    text(im,(x+22,y+13),label,size,TEXT,'Medium')
    return width,height


def region_diagram(im,x,y,w,h):
    draw=ImageDraw.Draw(im)
    draw.rounded_rectangle((x,y,x+w,y+h),radius=10,fill='#11151c',outline=BORDER,width=2)
    for k in range(4):
        draw.rounded_rectangle((x+24,y+28+k*24,x+w*(.74 if k%2 else .86),y+34+k*24),radius=3,fill='#333d4b')
    draw.rectangle((x+48,y+42,x+w-53,y+h-26),fill='#193354',outline=BLUE,width=3)
    capture_corners(draw,(x+47,y+41,x+w-52,y+h-25),16,3)
    px,py=x+w-60,y+h-31
    draw.polygon([(px,py),(px+2,py+37),(px+12,py+27),(px+22,py+44),(px+31,py+39),(px+21,py+22),(px+36,py+20)],fill=TEXT)


def feature(mobile=False):
    name='feature-mobile.png' if mobile else 'feature-desktop.png'
    im=canvas(name,(840,1770) if mobile else (1600,620))
    d=ImageDraw.Draw(im)
    titles=['Pick a region.','Make the point.','Copy or save.']
    lines=[['A window, one monitor','or exactly the area you need.'],['Arrows, text and highlights.','Keep the explanation in the image.'],['Use the shortcuts you know.','Take the result to your next task.']]
    for i in range(3):
        if mobile:
            x,y,w=42,35+i*570,756
            if i: d.line((42,y-20,798,y-20),fill=BORDER,width=2)
            text(im,(x,y),f'0{i+1}',25,BLUE,'Medium')
            text(im,(x,y+49),titles[i],49,weight='Bold')
            for j,line in enumerate(lines[i]): text(im,(x,y+121+j*38),line,29,MUTED)
            dx,dy,dw,dh=x,y+224,w,270
        else:
            x,y,w=50+i*530,52,470
            if i: d.line((x-25,42,x-25,570),fill=BORDER,width=2)
            text(im,(x,y),f'0{i+1}',22,BLUE,'Medium')
            text(im,(x,y+55),titles[i],40,weight='Bold')
            for j,line in enumerate(lines[i]): text(im,(x,y+125+j*33),line,24,MUTED)
            dx,dy,dw,dh=x,y+238,w,198
        if i==0:
            region_diagram(im,dx,dy,dw,dh)
            keycap(im,dx+18,dy+dh-7,'Ctrl + PrtScn',size=27 if not mobile else 29)
        elif i==1:
            source=Image.open(ROOT/'media/page/annotated-result.png').convert('RGB').crop((140,390,600,525))
            crop_h=round(dw/source.width*source.height)
            source=source.resize((dw,crop_h),Image.Resampling.LANCZOS)
            im.paste(source,(dx,dy+12))
            keycap(im,dx+24,dy+dh-12,'R',size=27)
            keycap(im,dx+121,dy+dh-12,'A',size=27)
            keycap(im,dx+218,dy+dh-12,'T',size=27)
        else:
            ksize=36 if mobile else 31
            keycap(im,dx+20,dy+18,'Ctrl + C',size=ksize)
            keycap(im,dx+(390 if mobile else 233),dy+18,'Ctrl + S',size=ksize)
            text(im,(dx+26,dy+137),'clipboard',26 if mobile else 23,MUTED)
            text(im,(dx+(396 if mobile else 239),dy+137),'local file',26 if mobile else 23,MUTED)
    im.save(OUT/name,optimize=True)


def workflow():
    source=Image.open(ROOT/'media/page/annotation-flow.png')
    titles=['Open the capture.','Frame the detail.','Add direction.','Leave a short note.']
    output=[]
    for k in range(source.n_frames):
        source.seek(k)
        frame=source.convert('RGB').crop((56,20,1144,640))
        im=canvas(f'workflow-{k}',(1600,1100))
        text(im,(54,39),'IN THE EDITOR',19,MUTED,'Medium',tracking=2)
        text(im,(51,79),titles[k],47,weight='Bold')
        text(im,(1458,62),f'0{k+1} / 04',23,BLUE,'Medium')
        rendered=frame.resize((1492,850),Image.Resampling.LANCZOS)
        im.paste(rendered,(54,162))
        d=ImageDraw.Draw(im)
        d.rounded_rectangle((53,161,1547,1013),radius=8,outline=BORDER,width=2)
        for j in range(4):
            x=54+j*382
            d.rounded_rectangle((x,1047,x+348,1053),radius=3,fill=BLUE if j<=k else BORDER)
        output.append(im)
    output[0].save(OUT/'workflow.png',save_all=True,append_images=output[1:],duration=[1600,1400,1400,3000],loop=0,disposal=0,blend=0,optimize=True)


def button():
    im=Image.new('RGBA',(540,116),(0,0,0,0)); im.info['name']='download-button.png'
    d=ImageDraw.Draw(im)
    d.rounded_rectangle((0,0,539,115),radius=20,fill=BLUE)
    d.line((48,26,48,65),fill='white',width=5)
    d.line((33,51,48,66,63,51),fill='white',width=5)
    d.line((28,78,28,86,70,86,70,78),fill='white',width=4)
    text(im,(103,24),'Download for Windows',33,'white','Medium')
    text(im,(104,74),'x64  ·  v1.0.1  ·  free',21,'#e6f0ff')
    im.save(OUT/'download-button.png',optimize=True)


def social():
    im=canvas('social-card.png',(1280,640)); d=ImageDraw.Draw(im)
    d.polygon([(850,0),(1280,0),(1280,640),(590,640)],fill='#18345a')
    native_icon(im,(50,40),62)
    text(im,(133,48),'Zipshot',48,weight='Bold')
    text(im,(49,175),'Capture.',77,weight='Bold')
    text(im,(49,264),'Annotate.',77,weight='Bold')
    text(im,(49,353),'Done.',77,BLUE,'Bold')
    screenshot(im,(574,143,662,416))
    capture_corners(d,(564,133,1247,569),22,3)
    text(im,(52,527),'Native Windows screenshots.',25,MUTED)
    text(im,(52,580),'Free. Open source. GPLv3+.',24,MUTED)
    im.save(OUT/'social-card.png',optimize=True)


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    for weight in ['Regular','Medium','Bold']:
        if not (FONTS/f'Ubuntu-{weight}.ttf').exists(): raise RuntimeError('Install Ubuntu fonts before rebuilding')
    hero(); hero(True); feature(); feature(True); workflow(); button(); social()
    paths=['hero-desktop.png','hero-mobile.png','feature-desktop.png','feature-mobile.png','workflow.png','download-button.png','social-card.png']
    manifest={'design':'Ubuntu typography, native Zipshot blue, flat capture geometry and genuine editor crops. No generated/fabricated UI.',
              'source_editor':'media/page/editor-overview.png','source_workflow':'media/page/annotation-flow.png',
              'source_result':'media/page/annotated-result.png','source_icon':'res/app.png',
              'claims':{'warm_capture_median_ms':CAPTURE_MS,'exe_bytes':1912320,'exe_decimal_mb':1.9,'license':'GPLv3+'},
              'font_policy':'Installed Ubuntu font files used for rasterization only. No font binaries distributed.',
              'workflow_timing_ms':[1600,1400,1400,3000],
              'images':{name:{'sha256':hashlib.sha256((OUT/name).read_bytes()).hexdigest(),'dimensions':Image.open(OUT/name).size} for name in paths},
              'text_bounds':BOUNDS}
    (OUT/'showcase-design.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({name:(OUT/name).stat().st_size for name in paths},indent=2))


if __name__=='__main__': main()
