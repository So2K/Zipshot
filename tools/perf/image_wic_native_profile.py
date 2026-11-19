from pathlib import Path
import json
import subprocess
from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
out=ROOT/'build/perf/images/wic-final-v1';out.mkdir(parents=True,exist_ok=True)
fixtures=out/'extra';fixtures.mkdir(exist_ok=True)
rgb=Image.new('RGB',(64,64),(12,34,56));rgb.putpixel((0,0),(255,0,0));rgb.save(fixtures/'rgb-trns.png',transparency=(12,34,56))
gray=Image.new('L',(64,64),77);gray.putpixel((0,0),0);gray.save(fixtures/'gray-trns.png',transparency=77)
palette=Image.new('P',(64,64),0);palette.putpalette([0,0,0,255,0,0]+[0,0,0]*254);palette.putpixel((0,0),1);palette.save(fixtures/'palette-alpha.png',transparency=bytes([0,128]+[255]*254))
gray16=Image.new('I;16',(64,64),32000);gray16.save(fixtures/'gray16.png')
rgba=Image.new('RGBA',(64,64),(200,100,50,0));rgba.putpixel((0,0),(10,20,30,128));rgba.save(fixtures/'hidden-rgb.png')
paths=[]
for folder in sorted((ROOT/'build/perf/images/corpus-v1').iterdir()):
    if not(folder/'complete.json').exists():continue
    paths += [folder/'engine0.png',folder/'fast1.png']
    paths += [folder/'sharex.png']
    if(folder/'engine0.jpg').exists():paths.append(folder/'engine0.jpg')
paths += sorted(fixtures.glob('*.png'))
result=subprocess.run([str(ROOT/'build/perf/images/image_wic_native.exe'),str(out),'20',*[str(p) for p in paths]],cwd=ROOT,text=True,capture_output=True)
(out/'run.log').write_text(result.stdout+result.stderr,encoding='utf-8')
print(json.dumps({'exit_code':result.returncode,'inputs':len(paths),'log':str(out/'run.log')}))
raise SystemExit(result.returncode)
