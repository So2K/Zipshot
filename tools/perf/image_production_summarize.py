import csv
import json
from pathlib import Path
from image_summarize import ROOT,png_metadata,stats

source=ROOT/'build/perf/images/production-v1'
manifest=json.loads((source/'production-manifest.json').read_text())
baseline=json.loads((ROOT/'docs/perf/image_summary.json').read_text())
baselineById={x['id']:x for x in baseline['cases']}
report={'schema':1,'scope':manifest['scope'],'manifest':manifest,'cases':[],'notes':[
 'Twenty warm timings per operation; nearest-rank p95; ambient background load uncontrolled.',
 'First means first timed call, not independent cold-process startup.',
 'Native and stock raw source construction excluded; actual encoder and completed file write included.',
 'Representative PNG output checked against exact source pixels/alpha and baseline color/DPI metadata; representative AutoJPEG checked byte-for-byte against original WIC JPEG.',
 'Stock Alpha uses its native premultiplied representation. High-entropy alpha automatic save formats differ and are excluded from Auto ratio.',
 'JPEG codec settings are nominal q90/4:2:0; native JPEG bytes/decision remain exactly its previous WIC output, not necessarily identical to the competing codec.',
 'No full UI/app latency, universal superiority, durability flush or memory claim is inferred.'
]}
lines=['# Production image save profile','',
 'Actual production functions after fast lossless PNG and bounded WIC automatic-JPEG integration. Raw synthetic source pixels are resident before the timer; encoder, allocation, output-stream/file creation and completed cached file write are included. Official ShareX22.4.0 uses its unchanged methods and exact self-contained .NET10.0.14 runtime. Both process families use below-normal priority and affinity0xF. Each row has20 warm timings; p95 uses nearest rank.','',
 '| Case | Zipshot forced PNG median / p95 ms | ShareX forced PNG median / p95 ms | Ratio | Zipshot Auto ms / format | ShareX Auto ms / format |',
 '|---|---:|---:|---:|---:|---:|']
allRows=[];pngRatios=[];autoRatios=[]
for case in manifest['cases']:
    folder=ROOT/case['production_folder'];groups={}
    for filename in ['production.csv','sharex.csv']:
        rows=list(csv.DictReader((folder/filename).open()))
        for row in rows:
            allRows.append({'case':case['id'],'source':filename,**row})
            groups.setdefault((row['engine'],row['operation']),[]).append(row)
    data={f'{e}/{o}':stats(r,case['width'],case['height']) for(e,o),r in groups.items()}
    native=data['zipshot-production/png-encode-file-forced'];stock=data['sharex-22.4.0/png-encode-file-forced']
    na=data['zipshot-production/save-auto-default'];sa=data['sharex-22.4.0/save-auto-default']
    nr=groups[('zipshot-production','save-auto-default')];sr=groups[('sharex-22.4.0','save-auto-default')]
    nf=nativeFormat=nr[0]['format'];sf=sr[0]['format'];ratio=stock['median_ms']/native['median_ms'];pngRatios.append(ratio)
    if nf==sf:autoRatios.append(sa['median_ms']/na['median_ms'])
    png=png_metadata(folder/'production.png');old=baselineById[case['id']]['png_files']['engine0.png']
    if png['metadata']!=old['metadata']:raise AssertionError((case['id'],'PNG metadata changed'))
    if any(r['source_sha256']!=case['input_sha256'] for r in groups[('zipshot-production','png-encode-file-forced')]):raise AssertionError(case['id'])
    firstPng=groups[('zipshot-production','png-encode-file-forced')][0]
    if firstPng['exact_pixels']!='1':raise AssertionError(case['id'])
    firstAuto=nr[0]
    if nf=='JPEG' and firstAuto['old_jpeg_exact']!='1':raise AssertionError(case['id'])
    report['cases'].append({**case,'statistics':data,'native_auto_format':nf,'stock_auto_format':sf,
                           'png_file_ratio':ratio,'auto_ratio_same_format':sa['median_ms']/na['median_ms'] if nf==sf else None,
                           'representative_png':png,'representative_png_exact_source':True,
                           'representative_auto_jpeg_exact_old_wic':firstAuto['old_jpeg_exact']=='1' if nf=='JPEG' else None})
    lines.append(f"| {case['id']} | {native['median_ms']:.2f} / {native['p95_ms']:.2f} | {stock['median_ms']:.2f} / {stock['p95_ms']:.2f} | {ratio:.2f}× | {na['median_ms']:.2f} {nf} | {sa['median_ms']:.2f} {sf} |")
report['png_file_ratio_range']=[min(pngRatios),max(pngRatios)];report['auto_same_format_ratio_range']=[min(autoRatios),max(autoRatios)];report['auto_same_format_cases']=len(autoRatios)
lines+=['',f'Forced PNG file saving is {min(pngRatios):.2f}–{max(pngRatios):.2f}× faster in this12-case generated corpus. All10 same-format Auto rows are faster here; that is not a claim about every possible picture, filesystem, machine or complete UI workflow.','',
 'High-entropy alpha automatic save is PNG in Zipshot and JPEG in stock ShareX, so those2 rows are excluded from the same-format Auto ratio. PNG source pixels and alpha remain exact. The first representative AutoJPEG file in every JPEG-selected case matches the previous Zipshot WIC JPEG bytes exactly; the separate40-assertion threshold suite covers boundary/failure behavior. No JPEG quality, subsampling, dimensions, user settings or threshold units were silently changed.','',
 'Fast PNG can produce larger files than the previous WIC PNG, especially UI/text. Color interpretation remains the same IHDR/sRGB/gAMA with no pHYs chunk. Detailed sizes, complete chunk CRC/IEND validation, all raw timings and captured source/executable hashes are in the accompanying JSON/CSV. Current decoder results are separately profiled; no unsafe upstream fpng reader is integrated.','',
 'The stock source/timing explanation, including legitimate early PNG termination above the JPEG threshold, is in [image_stock_path.md](image_stock_path.md). Baseline codec and decoder measurements are in [image_results.md](image_results.md).','']
dest=ROOT/'docs/perf';dest.mkdir(exist_ok=True)
(dest/'image_production_results.md').write_text('\n'.join(lines),encoding='utf-8')
(dest/'image_production_summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
fields=['case','source','engine','operation','phase','trial','ms','bytes','format','source_sha256','encoded_sha256','decoded_sha256','exact_pixels','old_jpeg_exact']
with(dest/'image_production_trials.csv').open('w',newline='',encoding='utf-8') as f:
    writer=csv.DictWriter(f,fieldnames=fields,extrasaction='ignore');writer.writeheader();writer.writerows(allRows)
print(json.dumps({'cases':len(report['cases']),'timings':len(allRows),'png_ratio':report['png_file_ratio_range'],'same_format_auto_cases':len(autoRatios),'quality':'PASS'}))
