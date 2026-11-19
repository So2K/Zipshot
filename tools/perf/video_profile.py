"""Bounded exact-output FFmpeg controls. Synthetic inputs only; no capture or production edits."""
import argparse
import ctypes
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import threading
import time
import shutil

ROOT=Path(__file__).resolve().parents[2]
K=ctypes.WinDLL('kernel32',use_last_error=True);P=ctypes.WinDLL('psapi',use_last_error=True)
K.SetProcessAffinityMask.argtypes=[W.HANDLE,ctypes.c_size_t];K.SetProcessAffinityMask.restype=W.BOOL
K.GetProcessAffinityMask.argtypes=[W.HANDLE,ctypes.POINTER(ctypes.c_size_t),ctypes.POINTER(ctypes.c_size_t)]
K.GetCurrentProcess.restype=W.HANDLE
K.GetProcessTimes.argtypes=[W.HANDLE,ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint64)]
class Memory(ctypes.Structure):
    _fields_=[('cb',W.DWORD),('faults',W.DWORD)]+[(n,ctypes.c_size_t) for n in ('peak_ws','ws','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile','private')]
P.GetProcessMemoryInfo.argtypes=[W.HANDLE,ctypes.POINTER(Memory),W.DWORD]
mask=ctypes.c_size_t();system=ctypes.c_size_t();K.GetProcessAffinityMask(K.GetCurrentProcess(),ctypes.byref(mask),ctypes.byref(system))
CORES=[i for i in range(64) if mask.value&(1<<i)][:4];AFFINITY=sum(1<<i for i in CORES)

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def execute(args,cwd=None,timeout=60):
    start=time.perf_counter();p=subprocess.Popen([str(x) for x in args],cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=0x08000000|0x4000)
    if not K.SetProcessAffinityMask(W.HANDLE(int(p._handle)),AFFINITY):p.kill();raise OSError('Cannot enforce four-core budget')
    collected=[b'',b'']
    def drain(stream,index):collected[index]=stream.read()
    readers=[threading.Thread(target=drain,args=(p.stdout,0)),threading.Thread(target=drain,args=(p.stderr,1))]
    for reader in readers:reader.start()
    peak_private=peak_ws=0;samples=[]
    while p.poll() is None:
        m=Memory();m.cb=ctypes.sizeof(m)
        if P.GetProcessMemoryInfo(W.HANDLE(int(p._handle)),ctypes.byref(m),m.cb):
            peak_private=max(peak_private,m.private);peak_ws=max(peak_ws,m.ws)
        samples.append({'t':time.perf_counter()-start,'private':m.private,'ws':m.ws})
        if time.perf_counter()-start>timeout:p.kill();p.wait();raise TimeoutError('Owned bounded FFmpeg control timed out')
        time.sleep(.01)
    for reader in readers:reader.join()
    stdout,stderr=collected;c=ctypes.c_uint64();e=ctypes.c_uint64();kernel=ctypes.c_uint64();user=ctypes.c_uint64()
    K.GetProcessTimes(W.HANDLE(int(p._handle)),ctypes.byref(c),ctypes.byref(e),ctypes.byref(kernel),ctypes.byref(user))
    result={'command':[str(x) for x in args],'wall_s':time.perf_counter()-start,'cpu_s':(kernel.value+user.value)/1e7,'kernel_s':kernel.value/1e7,'private_peak_sampled':peak_private,'ws_peak_sampled':peak_ws,'exit':p.returncode,'samples':samples}
    if p.returncode:raise RuntimeError(stderr.decode(errors='replace')[-2500:])
    return result,stdout

def percent(values,q):return sorted(values)[max(0,math.ceil(len(values)*q)-1)]
def stats(runs):
    return {k:{'median':statistics.median(r[k] for r in runs),'p95_nearest_rank':percent([r[k] for r in runs],.95)} for k in ('wall_s','cpu_s','private_peak_sampled','ws_peak_sampled')}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--reps',type=int,default=5);parser.add_argument('--out',type=Path,default=ROOT/'build/perf/video');args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    ffmpeg=Path(shutil.which('ffmpeg'));ffprobe=Path(shutil.which('ffprobe'))
    report={'schema':1,'scope':'Synthetic controls with four logical-processor affinity and below-normal priority. Not a desktop recording or universal ShareX comparison. Automatic codec thread choices are scoped to this CPU budget.','affinity_logical_processors':CORES,'ffmpeg_sha256':sha(ffmpeg),'source_base':'1fdf0a7','production_modified':False,'resolutions':[]}
    execute([ffmpeg,'-version'])
    for width,height in ((640,360),(1920,1080)):
        folder=out/f'{width}x{height}';folder.mkdir(exist_ok=True)
        command_plan=json.loads(subprocess.check_output([str(ROOT/'build/perf/video/video_commands.exe'),str(width),str(height),str(folder)],text=True))
        source=folder/'original.mp4'
        execute([ffmpeg,'-v','error','-y','-f','lavfi','-i',f'testsrc2=size={width}x{height}:rate=30:duration=4','-an','-c:v','libx264','-preset','ultrafast','-crf','0','-threads','4',source])
        item={'dimensions':[width,height],'input_sha256':sha(source),'commands':command_plan,'metadata':{},'renders':{}}
        base_probe=['-v','error','-show_entries','stream=codec_type,width,height,r_frame_rate,avg_frame_rate,nb_frames,duration,start_time:format=duration,start_time','-of','default',source]
        baseline_metadata=None
        for name,prefix in (('baseline',[]),('threads1',['-threads','1']),('no_stream_discovery',['-nofind_stream_info']),('no_discovery_threads1',['-nofind_stream_info','-threads','1'])):
            runs=[];responses=[]
            for i in range(args.reps+1):
                metric,data=execute([ffprobe,*prefix,*base_probe]);responses.append(data)
                if i:runs.append(metric)
            if baseline_metadata is None:baseline_metadata=responses[0]
            item['metadata'][name]={'runs':runs,'summary':stats(runs),'selected_metadata_byte_exact':all(x==baseline_metadata for x in responses)}
        baseline_md5=None
        cases=(('baseline',[],1,'all.cmd'),('filter2',[],2,'all.cmd'),('filter4',[],4,'all.cmd'),('decode1',['-threads','1'],1,'all.cmd'),('decode4',['-threads','4'],1,'all.cmd'),('changed_scalars_only',[],1,'delta.cmd'))
        for name,decoder,filters,commands in cases:
            runs=[];hashes=[];bitstreams=[]
            filter_file=folder/(name+'.txt');filter_file.write_text(f'sendcmd=f={commands},crop@camera={width}:{height}:0:0,scale={width}:{height}:eval=frame',encoding='ascii')
            for i in range(args.reps+1):
                target=folder/f'{name}-{i}.mp4'
                metric,_=execute([ffmpeg,'-hide_banner','-loglevel','error','-y',*decoder,'-i',source,'-filter_threads',str(filters),'-filter_script:v',filter_file.name,'-map','0:v:0','-map','0:a?','-c:v','libx264','-preset','veryfast','-crf','18','-threads','4','-pix_fmt','yuv420p','-fps_mode','passthrough','-c:a','copy','-map_metadata','0','-movflags','+faststart',target],cwd=folder)
                _,md5=execute([ffmpeg,'-v','error','-threads','4','-i',target,'-map','0:v:0','-fps_mode','passthrough','-f','framemd5','-'])
                hashes.append(hashlib.sha256(md5).hexdigest());bitstreams.append(sha(target));(folder/f'{name}-{i}.framemd5').write_bytes(md5)
                if i:runs.append(metric)
            if baseline_md5 is None:baseline_md5=hashes[0]
            item['renders'][name]={'runs':runs,'summary':stats(runs),'all_decoded_framemd5_exact':all(x==baseline_md5 for x in hashes),'framemd5_sha256':hashes,'file_sha256':bitstreams}
        if sha(source)!=item['input_sha256']:raise RuntimeError('Synthetic source changed')
        report['resolutions'].append(item)
        (out/'video-profile.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f'{width}x{height} complete',flush=True)
    print(out/'video-profile.json')

if __name__=='__main__':main()
