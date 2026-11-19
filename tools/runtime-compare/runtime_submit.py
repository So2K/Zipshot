"""Submit one structured job to an already running, task-owned Sandbox session."""
import argparse
import json
from pathlib import Path
import re

p=argparse.ArgumentParser()
p.add_argument('--stage',type=Path,default=Path('D:/ZipshotRuntimeCompare-20261118/session01'))
p.add_argument('--id',required=True)
p.add_argument('--kind',required=True,choices=['capture','recording','editor','inspect','shutdown'])
p.add_argument('--timeout',type=int,default=900)
p.add_argument('arguments',nargs=argparse.REMAINDER)
a=p.parse_args()
if not re.fullmatch('[a-zA-Z0-9_-]+',a.id): raise ValueError('Invalid job id')
if not (a.stage/'.zipshot-runtime-session.json').is_file(): raise RuntimeError('Unknown stage')
arguments=a.arguments[1:] if a.arguments[:1]==['--'] else a.arguments
target=a.stage/'input/jobs'/(a.id+'.json')
if target.exists(): raise RuntimeError('Job id exists; inspect its outcome instead of resubmitting')
temporary=target.with_suffix('.tmp')
temporary.write_text(json.dumps({'id':a.id,'kind':a.kind,'arguments':arguments,'timeout_seconds':a.timeout}),encoding='utf-8')
temporary.rename(target)
print('Submitted',a.id)
