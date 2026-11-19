"""Cover and end card for the real runtime comparison. No UI is fabricated."""
from pathlib import Path
import argparse
import hashlib
import json

from PIL import Image,ImageDraw,ImageFont,ImageFilter

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'media/runtime'
FONTS=Path('C:/Windows/Fonts')

def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=Path('D:/ZipshotRuntimeCompare-20261118/session02/output'));a=p.parse_args()
 runs=json.loads((ROOT/'docs/runtime-comparison/editor_ui_runs.json').read_text(encoding='utf-8-sig'))['runs']
 selected=next(r for r in runs if r['engine']=='native' and r['case']=='freehand')
 photo=a.source_root/selected['source_folder']/'video-last-frame.png'
 if not photo.exists():raise RuntimeError('The genuine editor frame is required')
 im=Image.new('RGB',(1920,1080),'#13161d');d=ImageDraw.Draw(im)
 d.polygon([(1310,0),(1920,0),(1920,870),(1020,870)],fill='#18345a')
 def text(x,y,value,size=30,color='#f4f6fa',weight='Regular'):
  f=ImageFont.truetype(str(FONTS/f'Ubuntu-{weight}.ttf'),size)
  d.text((x,y),value,font=f,fill=color,anchor='lt')
 icon=Image.open(ROOT/'res/app.png').convert('RGBA').resize((84,84),Image.Resampling.LANCZOS);im.paste(icon,(70,60),icon)
 text(175,68,'Zipshot',74,weight='Bold')
 text(73,234,'Real input.',90,weight='Bold');text(73,337,'Real capture.',90,'#3a8dff','Bold')
 text(77,487,'ShareX 22.4.0 and Zipshot 1.0.1',32)
 text(77,541,'Same actions. Measured results.',32,'#acb5c6')
 text(77,616,'Clean Windows Sandbox',27,'#acb5c6')
 text(77,657,'Original playback. Source data included.',27,'#acb5c6')
 image=Image.open(photo).convert('RGB');image.thumbnail((860,632),Image.Resampling.LANCZOS)
 x,y=990,199
 im.paste(image,(x,y))
 d.rounded_rectangle((x-1,y-1,x+image.width,y+image.height),radius=9,outline='#566274',width=2)
 d.line((70,863,1850,863),fill='#343c49',width=2)
 for x,value,label in [(73,'120','measured keypresses'),(710,'20','filmed editor runs'),(1344,'MP4 + GIF','moving output checked')]:
  text(x,898,value,52,'#3a8dff' if x==73 else '#f4f6fa','Medium');text(x,965,label,25,'#acb5c6')
 text(73,1025,'github.com/So2K/Zipshot',30,'#3a8dff','Medium')
 OUT.mkdir(parents=True,exist_ok=True);target=OUT/'runtime-poster.png';im.save(target,optimize=True)
 (OUT/'poster-manifest.json').write_text(json.dumps({'source_run':selected['source_folder'],'source_frame_sha256':hashlib.sha256(photo.read_bytes()).hexdigest(),
  'scope':'Genuine final editor frame with separate cover typography, not a recreated application UI.',
  'capture_measured_calls':120,'editor_runs':20,'output_sha256':hashlib.sha256(target.read_bytes()).hexdigest()},indent=2)+'\n',encoding='utf-8')
 print(target)

if __name__=='__main__':main()
