#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Original ISO/Joliet/CUE metadata fixtures; no proprietary game contents."""
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    result = importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result


iso = module('metadata_iso', 'make-media-fixtures.py')
cue = module('metadata_cue', 'make-cue-fixtures.py')
ROOT = (97, 12, 4, 7, 10, 40, 0)
DATES = {
    'README.TXT': ((97, 11, 20, 16, 19, 32, -20), False),
    'DATA': ((96, 3, 19, 15, 19, 4, 22), True),
    'PAYLOAD.BIN': ((100, 2, 29, 0, 0, 1, 52), False),
    'EMPTY': ((0, 0, 0, 0, 0, 0, 0), True),
    'NESTED.TXT': ((0, 1, 1, 0, 0, 1, 52), True),
    'RÉSUMÉ Ω.TXT': ((255, 12, 31, 23, 59, 59, -48), False),
}


def stamp(value):
    return bytes(n & 255 for n in value)


def ticks(value):
    if not any(value):
        time = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
    else:
        year, month, day, hour, minute, second, offset = value
        time = dt.datetime(1900 + year, month, day, hour, minute, second,
                           tzinfo=dt.timezone(dt.timedelta(minutes=offset * 15))).astimezone(dt.timezone.utc)
    return int((time - dt.datetime(1601, 1, 1, tzinfo=dt.timezone.utc)).total_seconds()) * 10_000_000


def image(joliet):
    data = bytearray(iso.image(joliet));expected=[];readme=None
    for wide in ([False, True] if joliet else [False]):
        root, desc = (22, 17) if wide else (20, 16)
        data[desc * 2048 + 174:desc * 2048 + 181] = stamp(ROOT)
        selected = wide == joliet
        if selected:expected.append({'name': '.', 'ticks': ticks(ROOT), 'attributes': 16, 'unspecified': False})
        for block, parent in ((root, ''), (root + 1, 'DATA/')):
            at = block * 2048
            while data[at]:
                count = data[at];name_size=data[at+32];identifier=bytes(data[at+33:at+33+name_size])
                if identifier in (b'\0', b'\1'):
                    value = DATES['DATA'][0] if parent and identifier == b'\0' else ROOT
                else:
                    name = identifier.decode('utf-16-be' if wide else 'ascii').split(';')[0].rstrip('.')
                    value, hidden = DATES[name.upper()]
                    if wide and name.upper() == 'README.TXT':value = (*value[:6], 22)
                    data[at+25] = (data[at+25] & ~1) | int(hidden)
                    if selected:
                        directory = bool(data[at+25] & 2)
                        expected.append({'name':parent+name, 'ticks':ticks(value), 'attributes':(16 if directory else 1) | (2 if hidden else 0), 'unspecified':not any(value)})
                        if name.upper() == 'README.TXT':readme=at
                data[at+18:at+25]=stamp(value);at+=count
    return data, expected, readme


def main():
    root=Path(sys.argv[1]);root.mkdir(parents=True, exist_ok=True);manifest={}
    for kind, wide in (('primary',False),('joliet',True)):
        data, expected, position=image(wide);path=root/(kind+'.iso');path.write_bytes(data)
        (root/(kind+'.tsv')).write_text(''.join(f"{e['name']}\t{e['ticks']}\t{e['attributes']}\n" for e in expected),encoding='utf-8')
        manifest[kind]={'path':str(path.resolve()),'entries':expected,'sha256':hashlib.sha256(data).hexdigest()}
        if not wide:
            # Reuse the independently tested CUE layout, replacing only the
            # MODE1 user-data sectors with this fixture's ISO filesystem bytes.
            binary=bytearray(cue.make_bin())
            for block in range(48):binary[block*2352+16:block*2352+2064]=data[block*2048:(block+1)*2048]
            (root/'disc.bin').write_bytes(binary);(root/'disc.cue').write_text(cue.CUE,encoding='utf-8')
            bad_dates={
                'month-zero':(97,0,20,16,19,32,0),'month-13':(97,13,20,16,19,32,0),
                'day-zero':(97,11,0,16,19,32,0),'april-31':(97,4,31,16,19,32,0),
                '1900-not-leap':(0,2,29,0,0,0,0),'2001-not-leap':(101,2,29,0,0,0,0),
                'hour-24':(97,11,20,24,0,0,0),'minute-60':(97,11,20,0,60,0,0),
                'second-60':(97,11,20,0,0,60,0),'offset-west':(97,11,20,0,0,0,-49),
                'offset-east':(97,11,20,0,0,0,53),'partial-zero':(0,0,0,0,0,0,1),
            }
            for name,value in bad_dates.items():
                malformed=bytearray(data);malformed[position+18:position+25]=stamp(value)
                (root/('bad-'+name+'.iso')).write_bytes(malformed)
    manifest['unspecified_policy']='1970-01-01 UTC, deterministic fallback, not a claimed recording date'
    (root/'metadata.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(f'Metadata fixtures ready: {root}')


if __name__=='__main__':main()
