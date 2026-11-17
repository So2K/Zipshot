"""Render original 1080p comparison footage, using measured JSON and real editor PNGs.

python tools/make_video.py --fixture
build/zipshot.exe --test-demo media/fixture.png media/demo
python tools/make_video.py
Requires Python, Pillow, NumPy, FFmpeg. Never captures the desktop.
"""
from pathlib import Path
import argparse
import json
import math
import subprocess
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
MEDIA = ROOT / 'media'
W, H, FPS = 1920, 1080, 30
LENGTH = 68
BG, PANEL, LINE = '#0b111b', '#121e2c', '#29394b'
WHITE, MUTED, TEAL, ORANGE = '#eff5fa', '#9cacc0', '#52e1c3', '#ffac74'
FONT = Path('C:/Windows/Fonts')
FONTS = {}


def font(size, bold=False):
    key = size, bold
    if key not in FONTS:
        FONTS[key] = ImageFont.truetype(str(FONT / ('segoeuib.ttf' if bold else 'segoeui.ttf')), size)
    return FONTS[key]


def txt(d, xy, text, size=30, fill=WHITE, bold=False):
    d.text(xy, text, font=font(size, bold), fill=fill, spacing=int(size*.2))


def box(d, rect, fill=PANEL, outline=LINE, radius=24, width=2):
    d.rounded_rectangle(rect, radius=radius, fill=fill, outline=outline, width=width)


def pill(d, x, y, label, fill=TEAL, size=23):
    f = font(size, True)
    width = d.textlength(label, font=f)+40
    box(d, (x, y, x+width, y+size+24), fill, fill, 12)
    txt(d, (x+20, y+8), label, size, BG, True)
    return width


def fixture():
    MEDIA.mkdir(exist_ok=True)
    im = Image.new('RGB', (1280, 720), '#111b29')
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 198, 720), fill='#172334')
    txt(d, (30, 28), 'Orbit', 32, TEAL, True)
    for i, name in enumerate(['Overview', 'Projects', 'Messages', 'Reports', 'Settings']):
        y = 124+i*58
        if i == 0: box(d, (18, y-4, 179, y+40), '#254239', '#254239', 8)
        txt(d, (34, y), name, 20, WHITE if i == 0 else MUTED)
    txt(d, (240, 30), 'Project overview', 30, WHITE, True)
    txt(d, (240, 80), 'A synthetic workspace for the Zipshot demo', 19, MUTED)
    for i, (label, value, sub) in enumerate([('Completed', '24', '+8 this week'), ('In progress', '08', 'Across 3 teams'), ('Response', '12 min', 'Average time')]):
        x = 242+i*322
        box(d, (x, 164, x+294, 318), '#1c2b3d', '#2e4053', 15)
        txt(d, (x+22, 182), label, 18, MUTED)
        txt(d, (x+22, 212), value, 44, TEAL if i == 0 else WHITE, True)
        txt(d, (x+22, 277), sub, 17, MUTED)
    box(d, (242, 346, 1200, 520), '#1c2b3d', '#2e4053', 15)
    txt(d, (264, 360), 'Weekly activity', 20, WHITE, True)
    pts = [(270+i*73, 482-int(v)) for i, v in enumerate([6, 17, 12, 31, 26, 64, 53, 78, 69, 92, 91, 104, 96])]
    for y in [410, 447, 484]: d.line((270, y, 1170, y), fill='#304050', width=1)
    d.line(pts, fill=TEAL, width=4)
    box(d, (242, 545, 1200, 658), '#1c2b3d', '#2e4053', 15)
    txt(d, (264, 561), 'Share the weekly report', 22, WHITE, True)
    txt(d, (264, 603), 'Review with the team', 18, MUTED)
    txt(d, (922, 562), 'demo@orbit.test', 19, ORANGE)
    pill(d, 1052, 605, 'Ready', TEAL, 17)
    txt(d, (242, 682), 'DEMO DATA ONLY  /  NO PERSONAL CONTENT', 14, MUTED)
    im.save(MEDIA/'fixture.png')
    return im


def background():
    y, x = np.mgrid[0:H, 0:W]
    a = np.exp(-((x-1480)**2+(y-150)**2)/(2*650**2))
    b = np.exp(-((x-120)**2+(y-1030)**2)/(2*560**2))
    data = np.zeros((H, W, 3), dtype=np.uint8)
    for k, base in enumerate([11, 17, 27]):
        data[:, :, k] = np.clip(base+a*[2, 13, 15][k]+b*[9, 4, 10][k], 0, 255)
    im = Image.fromarray(data)
    d = ImageDraw.Draw(im)
    for xx in range(0, W, 80): d.line((xx, 0, xx, H), fill='#162331')
    for yy in range(0, H, 80): d.line((0, yy, W, yy), fill='#162331')
    return im


def header(im, chapter, index):
    d = ImageDraw.Draw(im)
    txt(d, (80, 44), 'ZIPSHOT', 27, WHITE, True)
    txt(d, (244, 48), '/  SHAREX', 23, MUTED)
    txt(d, (1470, 48), chapter, 22, MUTED)
    txt(d, (1780, 44), f'0{index}', 27, TEAL, True)
    d.line((80, 103, 1840, 103), fill=LINE, width=2)
    txt(d, (80, 1020), 'So2K / Zipshot', 21, MUTED)
    txt(d, (1350, 1020), 'Реальные замеры · открытый код', 21, MUTED)


def camera(d, x, y, color):
    box(d, (x, y+40, x+180, y+170), None, color, 24, 6)
    d.ellipse((x+56, y+67, x+126, y+137), outline=color, width=6)
    d.line((x+46, y+38, x+60, y+16, x+112, y+16, x+126, y+38), fill=color, width=6)


def scene_intro(base, data):
    im = base.copy(); d = ImageDraw.Draw(im); header(im, 'СРАВНЕНИЕ', 1)
    pill(d, 82, 174, 'NATIVE C++  /  WINDOWS')
    txt(d, (78, 270), 'Захват.\nПравка. Готово.', 104, WHITE, True)
    txt(d, (82, 570), 'Zipshot против ShareX', 41, MUTED)
    txt(d, (82, 644), 'Один сценарий. Измеримая разница.', 29, MUTED)
    box(d, (1190, 205, 1810, 805), '#142736', '#36574f', 40)
    camera(d, 1403, 287, TEAL)
    txt(d, (1324, 505), 'Zipshot', 66, WHITE, True)
    txt(d, (1350, 609), 'vs ShareX 18.0.1', 28, MUTED)
    pill(d, 1325, 698, 'СМОТРИМ НА ПРАКТИКЕ', TEAL, 20)
    txt(d, (82, 890), 'Скриншоты  /  Аннотации  /  GIF и видео', 30, WHITE)
    return im


def scene_method(base, data):
    im = base.copy(); d = ImageDraw.Draw(im); header(im, 'УСЛОВИЯ', 2)
    txt(d, (80, 166), 'Сравниваем одинаковое', 70, WHITE, True)
    txt(d, (84, 278), 'Только вызов API захвата кадра. Без интерфейса и сохранения.', 31, MUTED)
    labels = [('01', 'Один компьютер', 'Ryzen 9 7950X / RTX 4070 Ti'), ('02', 'Одна область', '5760 × 2160 px · курсор выключен'), ('03', '60 замеров на приложение', '3 блока · чередование · прогрев')]
    for i, (n, title, sub) in enumerate(labels):
        y=382+i*172
        box(d, (82, y, 1838, y+142))
        txt(d, (116, y+34), n, 48, TEAL, True)
        txt(d, (240, y+25), title, 36, WHITE, True)
        txt(d, (240, y+82), sub, 26, MUTED)
    txt(d, (84, 940), 'ShareX 18.0.1 — установленная версия. Результат зависит от машины и нагрузки.', 26, MUTED)
    return im


def scene_speed(base, data):
    im = base.copy(); d = ImageDraw.Draw(im); header(im, 'ЗАМЕР', 3)
    txt(d, (80, 160), 'Меньше времени на захват', 68, WHITE, True)
    s=data['summary']; ratio=s['median_speedup']
    pill(d, 83, 275, f"≈ {ratio:.1f}× БЫСТРЕЕ В ЭТОМ ТЕСТЕ".replace('.', ','), TEAL, 27)
    for i, (key, name, color) in enumerate([('zipshot','Zipshot',TEAL), ('sharex','ShareX 18.0.1',ORANGE)]):
        x=82+i*900
        box(d, (x, 385, x+856, 845))
        txt(d, (x+36, 415), name, 39, color, True)
        txt(d, (x+32, 485), f"{s[key]['median_ms']:.1f}".replace('.', ','), 128, WHITE, True)
        txt(d, (x+411, 573), 'мс', 43, MUTED)
        txt(d, (x+40, 665), 'медиана · 60 измерений', 25, MUTED)
        txt(d, (x+40, 785), f"p95: {s[key]['p95_ms']:.1f} мс".replace('.', ','), 24, MUTED)
    txt(d, (84, 885), 'Ниже — лучше. Это скорость API захвата, а не всего рабочего процесса.', 27, WHITE)
    txt(d, (84, 943), 'Данные, код замера и ограничения доступны в репозитории.', 26, MUTED)
    return im


def scene_workflow(base, data, step, editor):
    im = base.copy(); d=ImageDraw.Draw(im); header(im, 'РЕАЛЬНЫЙ РЕДАКТОР', 4)
    titles=['Открываем\nснимок', 'Выделяем\nглавное', 'Добавляем\nнаправление', 'Размываем\nфрагмент', 'Подписываем.\nГотово.']
    hints=['Синтетический рабочий экран', 'Прямоугольник · R', 'Стрелка · A', 'Размытие · B', 'Текст · T  /  сохранение']
    pill(d, 82, 166, f'ШАГ {step+1} / 5', TEAL, 23)
    txt(d, (78, 292), titles[step], 55, WHITE, True)
    txt(d, (82, 490), hints[step], 23, MUTED)
    for k in range(5):
        d.ellipse((83+k*53, 588, 100+k*53, 605), fill=TEAL if k<=step else LINE)
    txt(d, (82, 694), 'Рендер настоящего\nредактора Zipshot.\nДействия заданы сценарием.', 24, MUTED)
    target=editor.resize((1255, 787), Image.Resampling.LANCZOS)
    box(d, (550, 170, 1830, 985), '#050a10', '#36574f', 24)
    im.paste(target, (562, 183))
    return im


def scene_audit(base, data):
    im=base.copy(); d=ImageDraw.Draw(im); header(im, 'ПРОВЕРКА КОДА', 5)
    txt(d, (80, 162), 'Быстрее — и аккуратнее', 69, WHITE, True)
    txt(d, (82, 276), 'Исправлены реальные ошибки. Проверки воспроизводятся локально.', 29, MUTED)
    pill(d, 84, 367, '35 РЕГРЕССИОННЫХ ПРОВЕРОК — PASS', TEAL, 29)
    fixes=[('01', 'Прозрачность', 'Обрезка и объединение изображений'), ('02', 'Сохранение файлов', 'Защита от перезаписи PNG / JPEG'), ('03', 'Повреждённые данные', 'Проверка размеров DIB и изображений'), ('04', 'Границы записи', 'Область остаётся внутри монитора')]
    for i,(n,title,sub) in enumerate(fixes):
        x=84+(i%2)*900; y=484+(i//2)*195
        box(d,(x,y,x+856,y+165))
        txt(d,(x+27,y+34),n,46,TEAL,True)
        txt(d,(x+118,y+27),title,33,WHITE,True)
        txt(d,(x+118,y+91),sub,23,MUTED)
    txt(d, (84, 940), 'Проверка ограничена описанными сценариями. Подробности и ограничения — в AUDIT.md.', 25, MUTED)
    return im


def scene_end(base, data, result):
    im=base.copy(); d=ImageDraw.Draw(im); header(im, 'ОТКРЫТЫЙ ПРОЕКТ', 6)
    pill(d, 82, 176, 'ZIPSHOT 1.0.1')
    txt(d, (78, 291), 'Быстрый захват.\nОткрытый код.', 76, WHITE, True)
    txt(d, (84, 538), 'Код. Видео. Методика. Результаты.', 28, MUTED)
    box(d, (84, 651, 1030, 768), TEAL, TEAL, 20)
    txt(d, (117, 672), 'github.com/So2K/Zipshot', 46, BG, True)
    txt(d, (84, 829), 'Windows x64  /  Native C++  /  GPLv3+', 27, MUTED)
    small=result.resize((705,397),Image.Resampling.LANCZOS)
    box(d,(1117,318,1840,735),'#050a10','#36574f',22)
    im.paste(small,(1126,328))
    txt(d,(1144,783),'Результат работы редактора',25,TEAL)
    return im


def soundtrack(path):
    sr=48000
    t=np.arange(sr*LENGTH,dtype=np.float64)/sr
    audio=np.zeros_like(t)
    # Original understated electronic bed; no samples or external music.
    for a,b,notes in [(0,17,[110,164.81,220]), (17,34,[130.81,196,261.63]), (34,51,[98,146.83,196]), (51,68,[110,164.81,220])]:
        m=(t>=a)&(t<b); local=t[m]-a
        env=np.minimum(local/1.5,1)*np.minimum((b-a-local)/1.5,1)
        for f in notes:
            audio[m]+=0.011*np.sin(2*np.pi*f*local)*env
            audio[m]+=0.004*np.sin(2*np.pi*f*2*local)*env
    for beat in np.arange(0,LENGTH,60/108):
        start=int(beat*sr); size=min(sr//5,len(audio)-start); bt=np.arange(size)/sr
        audio[start:start+size]+=0.025*np.sin(2*np.pi*(52*bt+35*bt*np.exp(-bt*32)))*np.exp(-bt*30)
    audio*=np.minimum(t/2,1)*np.minimum((LENGTH-t)/2,1)
    with wave.open(str(path),'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        w.writeframes((np.clip(audio,-1,1)*32767).astype('<i2').tobytes())


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--fixture',action='store_true'); ap.add_argument('--fps',type=int,default=30)
    args=ap.parse_args()
    if args.fixture: fixture(); return
    fps=args.fps
    data=json.loads((ROOT/'docs/benchmark/capture-results.json').read_text(encoding='utf-8-sig'))
    base=background()
    editors=[Image.open(MEDIA/f'demo/editor-{k}.png').convert('RGB') for k in range(5)]
    result=Image.open(MEDIA/'demo/result.png').convert('RGB')
    starts=[0,8,16,29,48,58]
    scenes=[scene_intro(base,data),scene_method(base,data),scene_speed(base,data)]
    scenes += [scene_workflow(base,data,k,e) for k,e in enumerate(editors)]
    scenes += [scene_audit(base,data),scene_end(base,data,result)]
    timings=[0,8,16,29,32.8,36.6,40.4,44.2,48,58]
    (MEDIA/'qa').mkdir(exist_ok=True)
    for i,im in enumerate(scenes): im.resize((1280,720),Image.Resampling.LANCZOS).save(MEDIA/f'qa/scene-{i:02}.jpg',quality=90)
    silent=ROOT/'build/comparison-silent.mp4'
    proc=subprocess.Popen(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s',f'{W}x{H}','-r',str(fps),'-i','-','-an','-c:v','libx264','-preset','fast','-crf','20','-pix_fmt','yuv420p','-threads','6',str(silent)],stdin=subprocess.PIPE)
    black=Image.new('RGB',(W,H),BG)
    for k in range(LENGTH*fps):
        t=k/fps
        i=max(i for i,start in enumerate(timings) if start<=t)
        frame=scenes[i].copy(); d=ImageDraw.Draw(frame)
        if i==2:
            p=min(1,(t-16)/2.5)
            maxval=max(data['summary'][app]['median_ms'] for app in ['zipshot','sharex'])
            for j,app in enumerate(['zipshot','sharex']):
                x=122+j*900; length=760*data['summary'][app]['median_ms']/maxval*p
                box(d,(x,737,x+760,752),'#28384b','#28384b',7)
                if length>14: box(d,(x,737,x+length,752),TEAL if j==0 else ORANGE,TEAL if j==0 else ORANGE,7)
        # Progress line and a short crossfade at each section/step change.
        d.line((80,995,1840,995),fill=LINE,width=3)
        d.line((80,995,80+1760*t/LENGTH,995),fill=TEAL,width=3)
        local=t-timings[i]
        if local<.28:
            previous=scenes[i-1] if i else black
            frame=Image.blend(previous,frame,max(0,local/.28))
        if t>LENGTH-.65: frame=Image.blend(frame,black,(t-(LENGTH-.65))/.65)
        proc.stdin.write(frame.tobytes())
        if k%(fps*8)==0: print(f'Rendered {t:.0f}s / {LENGTH}s',flush=True)
    proc.stdin.close()
    if proc.wait(): raise SystemExit('FFmpeg rendering failed')
    music=ROOT/'build/original-music.wav'; soundtrack(music)
    cmd=['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(silent),'-i',str(music)]
    workflow=[(f'workflow{k}',offset) for k,offset in enumerate([29.4,33,36.8,40.6,44.4])]
    if not all((MEDIA/f'narration/{name}.mp3').exists() for name,_ in workflow): workflow=[('workflow',29.4)]
    clips=[('intro',.7),('method',8.4),('speed',16.5)]+workflow+[('audit',48.4),('end',58.4)]
    found=[]
    for name,offset in clips:
        path=MEDIA/f'narration/{name}.mp3'
        if path.exists(): cmd+=['-i',str(path)]; found.append((name,offset))
    filters=['[1:a]volume=0.70[bed]']; streams=['[bed]']
    for j,(name,offset) in enumerate(found,2):
        filters.append(f'[{j}:a]aresample=48000,volume=1.25,adelay={int(offset*1000)}:all=1[v{j}]'); streams.append(f'[v{j}]')
    filters.append(''.join(streams)+f'amix=inputs={len(streams)}:duration=longest:normalize=0,alimiter=limit=0.9[a]')
    cmd+=['-filter_complex',';'.join(filters),'-map','0:v','-map','[a]','-c:v','copy','-c:a','aac','-b:a','192k','-t',str(LENGTH),'-movflags','+faststart',str(MEDIA/'zipshot-vs-sharex.mp4')]
    subprocess.run(cmd,check=True)
    poster=scenes[2].resize((1280,720),Image.Resampling.LANCZOS)
    poster.save(MEDIA/'comparison-poster.jpg',quality=94)
    (MEDIA/'video-manifest.json').write_text(json.dumps({'duration_seconds':LENGTH,'width':W,'height':H,'fps':fps,'language':'ru','capture_footage':'Real offscreen Zipshot editor renders; scripted actions; synthetic input only','benchmark_source':'docs/benchmark/capture-results.json','median_speedup':data['summary']['median_speedup'],'chapters_seconds':dict(zip(['intro','method','benchmark','editor','audit','github'],starts)),'narration_clips':[x[0] for x in found]},ensure_ascii=False,indent=2),encoding='utf-8')
    print('Complete:',MEDIA/'zipshot-vs-sharex.mp4',flush=True)


if __name__=='__main__': main()
