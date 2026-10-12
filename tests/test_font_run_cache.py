#!/usr/bin/env python3
"""Standalone exact-pixel/reference test; no repository object files changed."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--output', type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
header=(root/'include/font_compat.h').read_text()
names=sorted(set(re.findall(r'\b(font_compat_\w+)\s*\(',header)))
with tempfile.TemporaryDirectory(prefix='dsco-font-cache-') as tmp:
    tmp=Path(tmp)
    flags=['cc','-std=c11','-O3','-Wall','-Wextra','-I',str(root/'include')]
    subprocess.run(flags+[f'-D{name}=baseline_{name}' for name in names]+['-c',str(args.baseline.resolve()),'-o',str(tmp/'baseline.o')],check=True)
    subprocess.run(flags+[str(root/'tests/test_font_run_cache.c'),str(tmp/'baseline.o'),'-framework','CoreFoundation','-framework','CoreGraphics','-framework','CoreText','-lpthread','-o',str(tmp/'test')],check=True)
    results=[]
    for label, overrides in [
        ('default', {}),
        ('explicit-global-font', {'DSCO_PIXEL_FONT':'Courier'}),
        ('explicit-prose-and-math', {'DSCO_PIXEL_PROSE_FONT':'Helvetica', 'DSCO_PIXEL_MATH_FONT':'Times-Roman'}),
    ]:
        env=os.environ.copy()
        for name in ('DSCO_PIXEL_FONT','DSCO_PIXEL_PROSE_FONT','DSCO_PIXEL_MATH_FONT'):
            env.pop(name,None)
        env.update(overrides)
        cp=subprocess.run([str(tmp/'test')],env=env,check=True,capture_output=True,text=True)
        result=json.loads(cp.stdout)
        result['case']=label
        results.append(result)
        print(json.dumps(result))
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(results,indent=2)+'\n')
