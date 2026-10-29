#!/usr/bin/env python3
"""Native controls and prefix regression tests; only owned console children run."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--probes', type=Path, required=True)
parser.add_argument('--output-root', type=Path, required=True)
args = parser.parse_args()
runtime, probes = args.runtime.resolve(), args.probes.resolve()
root = args.output_root.resolve() / ('legacy-test-' + uuid.uuid4().hex[:8])
root.mkdir(parents=True)
records, checks = [], []


def check(condition, label):
    checks.append(dict(pass_=bool(condition), label=label))
    if not condition:
        print('FAIL ' + label, flush=True)


def run(label, command, cwd=None, environment=None):
    completed = subprocess.run(list(map(str, command)), cwd=cwd, env=environment,
        capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=45)
    output = completed.stdout + completed.stderr
    (root / (label + '.log')).write_text(output, encoding='utf-8')
    records.append(dict(label=label, command=list(map(str, command)), exit_code=completed.returncode, output=output))
    check(completed.returncode == 0, label + ' probe exits successfully')
    return output


def launches(label, command, logical, physical, report_root, injected, precedence=False, cwd=None, env=None, absent=False, child_bits=None):
    reports = report_root / label
    reports.mkdir()
    mode = '--parent-cwd' if cwd else '--parent'
    arguments = [mode, logical, physical, reports] + ([cwd] if cwd else [])
    output = run(label, command + arguments, environment=env)
    rows = [line for line in output.splitlines() if line.startswith('launch=')]
    check(len(rows) == 4, label + ' four launch variants')
    for line in rows:
        name = line.split()[0]
        if absent:
            check('child_report=MISSING' in line, label + ' ' + name + ' no unintended child')
            continue
        check('child_report=injected=' + str(int(injected)) in line, label + ' ' + name + ' injection state')
        check('arg3=two words arg4=tail' in line, label + ' ' + name + ' arguments preserved')
        if child_bits:
            check('bits=' + str(child_bits) in line, label + ' ' + name + ' child architecture')
        earlier = precedence and '-unquoted ' in line
        check(('module=' in line and line.lower().endswith('\\program.exe')) == earlier,
              label + ' ' + name + ' earliest executable precedence')
        check(('child_index=2' if earlier else 'child_index=1') in line,
              label + ' ' + name + ' argument boundary')
    return output


for bits in (32, 64):
    native = root / ('n' + str(bits))
    native_app, native_data = native / 'app', native / 'data'
    native_app.mkdir(parents=True); native_data.mkdir()
    shutil.copyfile(probes / f'legacy-launch-probe{bits}.exe', native_app / 'probe.exe')
    (native_app / 'appdir-only.dat').write_bytes(b'APPDATA!')
    (native_data / 'fixture.dat').write_bytes(b'FIXTURE!')
    (native_data / 'bad.exe').write_bytes(b'not executable')
    check(len(str(native_data / 'fixture.dat')) < 128, f'{bits} native control pathname below OFSTRUCT limit')
    native_command = [native_app / 'probe.exe']
    native_open = run(f'{bits}-native-open', native_command + ['--openfile', native_data, native_data / 'fixture.dat', native_data / 'fixture.dat'])
    for label in ('logical', 'physical', 'relative', 'search_appdir'):
        check(f'open_read={label} success=1' in native_open, f'{bits} native {label} OpenFile reads')
        check(f'open_reopen={label} success=1' in native_open, f'{bits} native {label} OF_REOPEN')
    native_errors = run(f'{bits}-native-errors', native_command + ['--errors', native_data / 'bad.exe'])

    prefix = root / (f'prefix{bits}-with-long-backing-storage-to-exceed-old-OFSTRUCT-limit')
    loader = runtime / f'wine{bits}.exe'
    run(f'{bits}-init', [loader, '--prefix', prefix, 'init'])
    app = prefix / 'drive_c/app'; app.mkdir()
    shutil.copyfile(probes / f'legacy-launch-probe{bits}.exe', app / 'probe.exe')
    (app / 'appdir-only.dat').write_bytes(b'APPDATA!')
    data = prefix / 'drive_c/Program Files/Legacy API'; data.mkdir(parents=True)
    (data / 'fixture.dat').write_bytes(b'FIXTURE!'); (data / 'bad.exe').write_bytes(b'not executable')
    command = [loader, '--prefix', prefix, 'run', r'C:\app\probe.exe']
    output = run(f'{bits}-prefix-open', command + ['--openfile', data, r'C:\Program Files\Legacy API\fixture.dat', data / 'fixture.dat'])
    for label in ('logical', 'physical', 'relative', 'search_appdir'):
        check(f'open_read={label} success=1' in output, f'{bits} prefix {label} OpenFile reads')
        check(f'open_reopen={label} success=1' in output, f'{bits} prefix {label} OF_REOPEN')
        check(f'open_parse={label} result=0' in output, f'{bits} prefix {label} OF_PARSE')
    check('count=8' in output, f'{bits} prefix file data read')
    check('open_read=missing success=0' in output, f'{bits} prefix absent file stays absent')
    output = run(f'{bits}-prefix-errors', command + ['--errors', r'C:\Program Files\Legacy API\bad.exe'])
    for case in ('empty','space','leading_space','leading_tab','quoted_empty','quoted_space','quoted_tab','missing','missing_directory'):
        for api in ('CreateProcessA','WinExec'):
            pattern = rf'^error_case={case} api={api} result=(\d+) error=(\d+)$'
            expected, actual = re.search(pattern,native_errors,re.M), re.search(pattern,output,re.M)
            check(expected and actual and expected.groups() == actual.groups(),f'{bits} {api} {case} matches native errors')
    # Plain text is unsupported DOS input on native x64 (216); the Wine-style
    # image validation reports BAD_EXE_FORMAT (193). Neither may launch a child.
    check(re.search(r'error_case=bad_image api=CreateProcessA result=0 error=(193|216)',output),f'{bits} invalid image rejected')
    check(re.search(r'error_case=bad_image api=WinExec result=(0|11) ',output),f'{bits} invalid image legacy failure code')

    readonly = root / ('readonly' + str(bits)); readonly.mkdir()
    readonly_file = readonly / 'fixture.dat'; readonly_file.write_bytes(b'UNCHANGED')
    run(f'{bits}-readonly-map',[loader,'--prefix',prefix,'drive','map','D',readonly,'fixed','FIXTURE'])
    output = run(f'{bits}-readonly-open',command+['--policy',readonly_file])
    check('open_read=readonly success=1' in output,f'{bits} physical fixed readonly mapping reads')
    policy = [line for line in output.splitlines() if line.startswith('policy ')]
    check(len(policy)==2 and all('success=0' in line for line in policy),f'{bits} projected OpenFile create/delete enforce readonly')
    check(readonly_file.read_bytes()==b'UNCHANGED',f'{bits} readonly mapping bytes unchanged')

    child = prefix / 'drive_c/Program Files/Maxis/FullTilt/Cadet/Cadet.exe'; child.parent.mkdir(parents=True)
    shutil.copyfile(probes / f'legacy-launch-probe{bits}.exe', child)
    reports = prefix / 'drive_c/reports'; reports.mkdir()
    logical = r'C:\Program Files\Maxis\FullTilt\Cadet\Cadet.exe'
    launches(f'{bits}-prefix-launch', command, logical, child, reports, True, child_bits=bits)
    launches(f'{bits}-native-launch', native_command, str(child), child, reports, False, child_bits=bits)
    shorter = prefix / 'drive_c/Program.exe'; shutil.copyfile(probes / f'legacy-launch-probe{bits}.exe', shorter)
    launches(f'{bits}-prefix-precedence',command,logical,child,reports,True,precedence=True,child_bits=bits)
    launches(f'{bits}-native-precedence',native_command,str(child),child,reports,False,precedence=True,child_bits=bits)
    # The fixed file is a test-owned earlier candidate, never a host Program.exe.
    shorter.unlink()
    launches(f'{bits}-prefix-extension',command,logical[:-4],child,reports,True,child_bits=bits)
    launches(f'{bits}-native-extension',native_command,str(child)[:-4],child,reports,False,child_bits=bits)
    shutil.copyfile(probes / f'legacy-launch-probe{96-bits}.exe', child)
    launches(f'{bits}-prefix-crossarch',command,logical,child,reports,True,child_bits=96-bits)
    bare = data / f'wd-cwd-child-{bits}.exe'; shutil.copyfile(probes / f'legacy-launch-probe{bits}.exe',bare)
    launches(f'{bits}-prefix-cwd',command,bare.name,bare,reports,True,cwd=data,child_bits=bits)
    environment = dict(os.environ, NoDefaultCurrentDirectoryInExePath='1')
    launches(f'{bits}-prefix-no-cwd',command,bare.name,bare,reports,True,cwd=data,env=environment,absent=True)
    launches(f'{bits}-native-no-cwd',native_command,bare.name,bare,reports,False,cwd=data,env=environment,absent=True)
    launches(f'{bits}-prefix-explicit-cwd',command,'.\\'+bare.name,bare,reports,True,cwd=data,env=environment,child_bits=bits)

result = dict(runtime=str(runtime), checks=checks, executions=records,
    passed=sum(c['pass_'] for c in checks), failed=sum(not c['pass_'] for c in checks))
(root/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(f"PASS {result['passed']} FAIL {result['failed']} {root/'result.json'}",flush=True)
raise SystemExit(bool(result['failed']))
