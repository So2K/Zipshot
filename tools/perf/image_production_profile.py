import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def call(command,log):
    started=time.monotonic();p=subprocess.run([str(x) for x in command],cwd=ROOT,text=True,capture_output=True)
    log.write_text(p.stdout+p.stderr,encoding="utf-8")
    if p.returncode:raise RuntimeError(f"exit{p.returncode}; {log}")
    return time.monotonic()-started
def main():
    p=argparse.ArgumentParser();p.add_argument('--n',type=int,default=20);p.add_argument('--cases',default='');p.add_argument('--out',default='build/perf/images/production-v1');p.add_argument('--corpus',default='build/perf/images/corpus-v1');a=p.parse_args()
    corpus=json.loads((ROOT/a.corpus/'profile-manifest.json').read_text())
    out=(ROOT/a.out).resolve();allowed=(ROOT/'build/perf/images').resolve()
    if not out.is_relative_to(allowed):raise ValueError('outside task output root')
    out.mkdir(parents=True,exist_ok=True)
    manifest={'schema':1,'n_warm':a.n,'scope':'actual production forcedPNG and defaultAuto file APIs; not whole UI',
      'source_sha256':{f:sha(ROOT/f) for f in ['src/output.cpp','src/png_encode_fast.cpp','src/png_limit_stream.h','third_party/fpng/fpng.cpp','tools/perf/image_production.cpp','tools/perf/image_sharex.cs']},
      'exe_sha256':{f:sha(ROOT/f) for f in ['build/perf/images/image_production.exe','build/perf/images/sharex-runtime/image_sharex.dll']},'cases':[]}
    for case in corpus['cases']:
        if a.cases and case['id'] not in a.cases.split(','):continue
        folder=out/case['id'];folder.mkdir(parents=True,exist_ok=True);source=ROOT/case['folder']
        done=folder/'complete.json'
        if done.exists():manifest['cases'].append(json.loads(done.read_text()));continue
        timings={}
        timings['production']=call([ROOT/'build/perf/images/image_production.exe',source,folder,case['width'],case['height'],str(case['alpha']).lower(),a.n],folder/'production.log')
        # Stock raw-source constructor is outside timer, as in the original corpus.
        # Its forced-PNG and PrepareImage+Write paths include encode and file write.
        stockFolder=folder/'stock-input';stockFolder.mkdir(exist_ok=True)
        stockRaw=stockFolder/'fixture.bgra'
        if not stockRaw.exists():os.link(source/'fixture.bgra',stockRaw)
        timings['stock']=call([ROOT/'build/perf/images/sharex-runtime/image_sharex.exe',stockFolder,case['width'],case['height'],str(case['alpha']).lower(),a.n,'--file-only'],folder/'sharex.log')
        (folder/'sharex.csv').write_bytes((stockFolder/'sharex.csv').read_bytes())
        result={**case,'production_folder':str(folder.relative_to(ROOT)).replace('\\','/'),'elapsed_s':timings,
                'production_png_sha256':sha(folder/'production.png'),'csv_sha256':{x:sha(folder/x) for x in ['production.csv','sharex.csv']}}
        done.write_text(json.dumps(result,indent=2)+'\n');manifest['cases'].append(result)
        (out/'production-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        print(json.dumps({'case':case['id'],'status':'PASS','seconds':round(sum(timings.values()),2)}),flush=True)
    (out/'production-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__':main()
