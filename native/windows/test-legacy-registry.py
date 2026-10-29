#!/usr/bin/env python3
"""Compare NT native controls with Wine's opt-in Windows 9x REG_SZ behavior."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import uuid

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime',type=Path,required=True)
parser.add_argument('--probes',type=Path,required=True)
parser.add_argument('--output-root',type=Path,required=True)
args=parser.parse_args()
runtime,probes=args.runtime.resolve(),args.probes.resolve()
root=args.output_root.resolve()/('legacy-registry-'+uuid.uuid4().hex[:8]);root.mkdir(parents=True)
checks,executions=[],[]

def check(value,label):
    checks.append(dict(passed=bool(value),label=label))
    if not value:print('FAIL '+label,flush=True)

def run(label,command):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
    text=result.stdout+result.stderr
    (root/(label+'.log')).write_text(text,encoding='utf-8')
    executions.append(dict(label=label,command=list(map(str,command)),exit_code=result.returncode,output=text))
    check(result.returncode==0,label+' successful execution')
    return text

def parse(text):
    rows={}
    for line in text.splitlines():
        match=re.fullmatch(r'case=(\w+) status=(\d+) query=(\d+) type=(\d+) size=(\d+) hex=([0-9a-f]*)',line)
        if match:rows[match[1]]=dict(status=int(match[2]),query=int(match[3]),type=int(match[4]),size=int(match[5]),hex=match[6])
    return rows

for bits in (32,64):
    native=None;loader=runtime/f'wine{bits}.exe';probe=probes/f'legacy-registry-probe{bits}.exe'
    rejected=subprocess.run([str(probe)],capture_output=True,text=True,timeout=10)
    check(rejected.returncode==3 and 'refusing host HKCU' in rejected.stderr,f'{bits} accidental native execution refused before registry access')
    for mode in ('native','win10','win98','per_app_win98','per_app_native'):
        prefix=root/f'{bits}-{mode}'
        run(f'{bits}-{mode}-init',[loader,'--prefix',prefix,'init'])
        if mode=='native':
            # This is a disposable private application hive, not host HKCU.
            command=[probe,'--native',prefix/'.winedows/registry.dat']
        else:
            default='win98' if mode in ('win98','per_app_native') else 'win10'
            settings='Windows Registry Editor Version 5.00\n\n[HKEY_CURRENT_USER\\Software\\Wine]\n"Version"="'+default+'"\n'
            if mode.startswith('per_app_'):
                app='win98' if mode=='per_app_win98' else 'native'
                settings+='\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\probe.exe]\n"Version"="'+app+'"\n'
            reg=root/f'{bits}-{mode}.reg';reg.write_text(settings)
            run(f'{bits}-{mode}-import',[loader,'--prefix',prefix,'reg','import',reg])
            shutil.copyfile(probe,prefix/'drive_c/probe.exe')
            shutil.copyfile(probes/f'legacy-registry-module{bits}.dll',prefix/f'drive_c/legacy-registry-module{bits}.dll')
            command=[loader,'--prefix',prefix,'run',r'C:\probe.exe','--prefix']
        rows=parse(run(f'{bits}-{mode}-probe',command))
        check(len(rows)==13,f'{bits} {mode} thirteen recorded registry cases')
        if native is None:native=rows;continue
        win9x=mode in ('win98','per_app_win98')
        for case,row in rows.items():
            if not win9x or case in ('expand_short','binary_short','wide_short','host_boundary'):
                check(row==native.get(case),f'{bits} {mode} {case} native NT bytes preserved')
            elif case=='null':
                check(row['status']==87 and row['query']==2,f'{bits} {mode} null REG_SZ rejected like Wine Win9x')
            else:
                expected=('\0' if case=='empty' else 'Space Cadet\0').encode('utf-16le').hex()
                check(row['status']==0 and row['query']==0 and row['type']==1 and row['hex']==expected,
                      f'{bits} {mode} {case} Wine Win9x string length')
result=dict(runtime=str(runtime),checks=checks,executions=executions,
            passed=sum(c['passed'] for c in checks),failed=sum(not c['passed'] for c in checks))
(root/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(f"PASS {result['passed']} FAIL {result['failed']} {root/'result.json'}",flush=True)
raise SystemExit(bool(result['failed']))
