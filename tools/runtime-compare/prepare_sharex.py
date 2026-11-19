"""Fetch an official portable ShareX release into a task-owned cache.

No installation or user settings changes. Pin version and the published SHA256.
Usage: python tools/runtime-compare/prepare_sharex.py --cache <external-cache>
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import time
import urllib.request
import zipfile


def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''): h.update(block)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--cache',required=True,type=Path)
    parser.add_argument('--version',default='22.4.0')
    parser.add_argument('--sha256',default='c321ed353a6c6ae32093927f97c22e0f0627628ce6fa47ed366bcefd05936c7b')
    args=parser.parse_args()
    if not args.version or any(c not in '0123456789.' for c in args.version): raise ValueError('Unexpected version')
    cache=args.cache.resolve()
    cache.mkdir(parents=True,exist_ok=True)
    name=f'ShareX-{args.version}-portable-x64.zip'
    archive=cache/name
    url=f'https://github.com/ShareX/ShareX/releases/download/v{args.version}/{name}'
    if not archive.exists():
        partial=cache/(name+f'.{time.time_ns()}.part')
        with urllib.request.urlopen(url,timeout=30) as response, partial.open('xb') as output:
            total=0
            while True:
                block=response.read(1024*1024)
                if not block: break
                output.write(block); total+=len(block)
                if total%(16*1024*1024)==0: print(f'Downloaded {total//(1024*1024)} MiB',flush=True)
        if digest(partial)!=args.sha256.lower(): raise RuntimeError('Published archive SHA256 did not match')
        partial.rename(archive)
    if digest(archive)!=args.sha256.lower(): raise RuntimeError('Existing cache archive does not match')
    destination=cache/f'ShareX-{args.version}-portable-x64'
    marker=destination/'.zipshot-official-portable.json'
    if destination.exists() and not marker.exists(): raise RuntimeError('Existing output is not an owned extraction')
    if not destination.exists():
        destination.mkdir()
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                path=PurePosixPath(info.filename.replace('\\','/'))
                target=(destination/str(path)).resolve()
                if path.is_absolute() or '..' in path.parts or any(':' in p for p in path.parts) or not target.is_relative_to(destination):
                    raise RuntimeError('Unsafe archive path')
                if stat.S_ISLNK(info.external_attr>>16): raise RuntimeError('Symlinks are not accepted')
            z.extractall(destination)
        executables=list(destination.rglob('ShareX.exe'))
        if len(executables)!=1: raise RuntimeError('Unexpected app layout')
        marker.write_text(json.dumps({'version':args.version,'release_url':f'https://github.com/ShareX/ShareX/releases/tag/v{args.version}',
            'archive_url':url,'archive_sha256':digest(archive),'exe_relative':str(executables[0].relative_to(destination)),
            'exe_sha256':digest(executables[0]),'installed':False},indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'directory':str(destination),'manifest':str(marker),'archive_sha256':digest(archive)},indent=2),flush=True)


if __name__=='__main__': main()
