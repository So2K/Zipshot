"""Russian narration for the measured runtime film. Only public script text is sent.

Requires gTTS and FFprobe. No microphone, account credentials or user recordings.
Each network request is bounded; completed clips are reusable without a request.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
if (ROOT/'build/narration-deps').exists():sys.path.insert(0,str(ROOT/'build/narration-deps'))
SCENES=[
 ('intro','Сравним Zipshot и актуальный ShareX. Чистая Windows Sandbox, одинаковые клавиши и одни исходные изображения. Нажатия отправляет тестовый скрипт.'),
 ('region','Сначала контрол принтскрин. Нажатие и таймер привязаны к сохранённым меткам времени. Здесь показан один записанный пример.'),
 ('png','Теперь принтскрин. Во втором замере ждём готового PNG. Файл должен закрыться и успешно пройти проверку изображения.'),
 ('arrow','Одна стрелка, одинаковая траектория курсора и две секунды на действие.'),
 ('pencil','Теперь карандаш. Скрипт отправляет одинаковые сто двадцать позиций курсора. Сравниваем настоящие кадры редакторов.'),
 ('tools','Отдельно показываем масштабирование, отмену действий и работу с изображением. Короткие ролики можно открыть в полном разрешении.'),
 ('capture_data','Для графиков нажатия повторены по тридцать раз. Медиана готового PNG у Zipshot составила триста сорок одну миллисекунду. У ShareX пятьсот тридцать. Это наблюдаемое время в этой среде.'),
 ('recording','Запись тоже проверена. Клип полностью декодирован, а движение и число кадров сверены со счётчиком на сцене.'),
 ('resources','Расход памяти и процессора зависит от кодировщика и пресета. У Zipshot значительную часть памяти в этом тесте занимал FFmpeg. Поэтому настройки и исходные результаты приложены к сравнению.'),
 ('outro','Код, ролики, графики и исходные замеры лежат на GitHub.')
]

def seconds(path):return float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration','-of','csv=p=0',str(path)],text=True,timeout=15).strip())

def main():
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=ROOT/'build/runtime-media/narration');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
 from gtts import gTTS
 def build(scene):
  name,text=scene;target=a.out/(name+'.mp3')
  if not target.exists():gTTS(text,lang='ru',lang_check=False,timeout=(8,20)).save(str(target))
  duration=seconds(target)
  print(name,round(duration,3),flush=True)
  return {'name':name,'text':text,'duration_seconds':duration,'file':target.name}
 with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:clips=list(pool.map(build,SCENES))
 (a.out/'manifest.json').write_text(json.dumps({'language':'ru','voice':'Google Translate TTS through gTTS','clips':clips},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
