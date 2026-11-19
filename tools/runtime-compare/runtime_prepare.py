"""Prepare task-only, read-only Sandbox inputs. Does not launch or control host UI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[2]
BASE=Path('D:/ZipshotRuntimeCompare-20261118')

def link_file(source,target):
    target.parent.mkdir(parents=True,exist_ok=True)
    if target.exists():
        if target.stat().st_size!=source.stat().st_size: raise RuntimeError('Unexpected staged file '+str(target))
        return
    try: os.link(source,target)
    except OSError: shutil.copy2(source,target)

def link_tree(source,target,skip=()):
    for path in source.rglob('*'):
        if path.is_file() and path.name.lower() not in skip: link_file(path,target/path.relative_to(source))

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser();p.add_argument('--stage',type=Path,default=BASE/'session01');args=p.parse_args()
    stage=args.stage.resolve(); inp=stage/'input';out=stage/'output'
    marker=stage/'.zipshot-runtime-session.json'
    if stage.exists() and not marker.exists(): raise RuntimeError('Existing stage is not task-owned')
    inp.mkdir(parents=True,exist_ok=True);out.mkdir(exist_ok=True);(inp/'jobs').mkdir(exist_ok=True)
    approval={'task':'zipshot-runtime-compare-20261118','host_stage':str(stage),'purpose':'Synthetic desktop only, no host input or personal pixels'}
    marker.write_text(json.dumps(approval,indent=2),encoding='utf-8')
    (inp/'.isolation-approved.json').write_text(json.dumps(approval),encoding='utf-8')
    for name in ['ffmpeg.exe','ffprobe.exe']:
        link_file(BASE/'sandbox-input'/name,inp/name)
    app=inp/'zipshot';app.mkdir(exist_ok=True)
    link_file(ROOT/'build/zipshot.exe',app/'zipshot.exe')
    if sha(app/'zipshot.exe')!='0730b10f1f00295e32f603feb3c482fe2aa66da08b644bb5595592b0ef1ac783': raise RuntimeError('Release identity changed')
    (app/'zipshot.ini').write_text('''[main]
Language=en
ShareXImported=1
SaveDir=C:\\ZipshotLab\\work\\shots\\zipshot
AfterCopy=1
AfterSave=1
AfterToast=1
AfterEditor=0
ShowCursor=0
Sound=0
SoundCapture=0
SoundDone=0
SoundAction=0
ToastSeconds=0.6
ToastFadeSeconds=0
JpegOverKB=0
FFmpeg=C:\\ZipshotLab\\input\\ffmpeg.exe
[hotkeys]
Full=PrintScreen
Region=Ctrl+PrintScreen
Window=Alt+PrintScreen
Video=Shift+PrintScreen
Gif=Ctrl+Shift+PrintScreen
''',encoding='utf-16')
    official=BASE/'vendor/ShareX-22.4.0-portable-x64'
    link_tree(official,inp/'sharex22',skip=('portable','personalpath.cfg','.zipshot-official-portable.json'))
    (inp/'sharex22/PersonalPath.cfg').write_text('C:\\ZipshotLab\\work\\sharex22\n',encoding='utf-8')
    # Legacy runtime only. Never copy its personal ShareX/ folder or sticker/user data.
    legacy=Path('D:/Programs/ShareX');legacy_out=inp/'sharex18';legacy_out.mkdir(exist_ok=True)
    for path in legacy.iterdir():
        if path.is_file() and (path.suffix.lower() in ('.dll','.exe','.manifest') or path.name.endswith(('.runtimeconfig.json','.deps.json'))):
            link_file(path,legacy_out/path.name)
        elif path.is_dir() and path.name not in ('ShareX','Stickers','exiftool_files'):
            for dll in path.rglob('*.dll'): link_file(dll,legacy_out/dll.relative_to(legacy))
    (legacy_out/'PersonalPath.cfg').write_text('C:\\ZipshotLab\\work\\sharex18\n',encoding='utf-8')
    for path in (ROOT/'tools/runtime-compare').glob('*.ps1'): shutil.copy2(path,inp/path.name)
    for path in (ROOT/'build/runtime-compare/editor-fixtures').glob('*'):
        if path.is_file(): link_file(path,inp/'fixtures'/path.name)
    link_file(ROOT/'build/runtime-compare/capture_hotkeys.exe',inp/'capture_hotkeys.exe')
    link_file(ROOT/'build/runtime-compare/editor-ui-native/editor_ui_native.exe',inp/'editor_ui_native.exe')
    sharex_ui=Path('D:/Tools/ZipshotEditorUi22420261118')
    if sharex_ui.exists(): link_tree(sharex_ui,inp/'editor22')
    payload=BASE/'recording-payload'
    if payload.exists():
        link_tree(payload,inp/'recording')
        # Replace one directory entry atomically, preserving any linked source.
        shutil.copy2(payload/'recording_guest.ps1',inp/'recording/recording_guest_legacy.ps1')
        temporary=inp/'recording/.dispatch.tmp'
        shutil.copy2(ROOT/'tools/runtime-compare/runtime_recording_dispatch.ps1',temporary)
        os.replace(temporary,inp/'recording/recording_guest.ps1')
    versions={name:{'file':str((inp/name/file).relative_to(inp)),'sha256':sha(inp/name/file)} for name,file in [('zipshot','zipshot.exe'),('sharex22','ShareX.dll'),('sharex18','ShareX.dll')]}
    (inp/'input-manifest.json').write_text(json.dumps({'versions':versions,'ffmpeg_sha256':sha(inp/'ffmpeg.exe'),'clipboard_redirection':False},indent=2),encoding='utf-8')
    from PIL import Image
    wallpaper=Image.new('RGB',(1920,1080),'#183b63');wallpaper.save(inp/'wallpaper.png')
    print(json.dumps({'stage':str(stage),'input':str(inp),'output':str(out),'versions':versions},indent=2))

if __name__=='__main__': main()
