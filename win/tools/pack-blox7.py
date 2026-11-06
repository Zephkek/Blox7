"""packs win/out/dist into the cabinet Blox7.exe carries, plus its id header
usage: pack-blox7.py <dist dir> <out dir>"""
import hashlib
import os
import subprocess
import sys

dist, out = (os.path.abspath(a) for a in sys.argv[1:3])
os.makedirs(out, exist_ok=True)

files = []
for root, dirs, names in os.walk(dist):
    dirs.sort()
    for name in sorted(names):
        full = os.path.join(root, name)
        files.append((full, os.path.relpath(full, dist).replace('/', '\\'), os.path.getsize(full)))
total = sum(size for _, _, size in files)

listing = os.path.join(out, 'blox7-files.txt')
with open(listing, 'w', encoding='utf-8', newline='\r\n') as f:
    for _, rel, size in files:
        f.write(f'{size} {rel}\n')

ddf = os.path.join(out, 'payload.ddf')
with open(ddf, 'w', encoding='utf-8', newline='\r\n') as f:
    f.write('.OPTION EXPLICIT\n')
    for key, value in (('CabinetNameTemplate', 'payload.cab'), ('DiskDirectoryTemplate', out), ('Cabinet', 'on'),
                       ('Compress', 'on'), ('CompressionType', 'LZX'), ('CompressionMemory', '21'),
                       ('MaxDiskSize', '0'), ('MaxCabinetSize', '0'), ('CabinetFileCountThreshold', '0'),
                       ('FolderFileCountThreshold', '0'), ('FolderSizeThreshold', '0'), ('UniqueFiles', 'on'),
                       ('RptFileName', os.path.join(out, 'payload.rpt')),
                       ('InfFileName', os.path.join(out, 'payload.inf'))):
        f.write(f'.Set {key}="{value}"\n')
    for full, rel, _ in files:
        f.write(f'"{full}" "{rel}"\n')
    f.write(f'"{listing}" ".blox7-files"\n')

content = hashlib.sha256()
for full, rel, size in files:
    content.update(rel.encode() + b'|' + str(size).encode() + b'|')
    with open(full, 'rb') as data:
        content.update(hashlib.sha256(data.read()).digest())
digest = content.hexdigest()[:16]

cab = os.path.join(out, 'payload.cab')
stamp = os.path.join(out, 'payload.stamp')
packed = os.path.exists(stamp) and open(stamp, encoding='ascii').read() == digest
if not (packed and os.path.exists(cab)):
    if os.path.exists(cab):
        os.remove(cab)
    subprocess.run(['makecab.exe', '/F', ddf], cwd=out, check=True, stdout=subprocess.DEVNULL)
    with open(stamp, 'w', encoding='ascii') as f:
        f.write(digest)
with open(os.path.join(out, 'payload_id.h'), 'w', encoding='ascii', newline='\n') as f:
    f.write(f'#pragma once\n#define BLOX7_PAYLOAD_ID L"{digest}"\n#define BLOX7_PAYLOAD_BYTES {total}ULL\n')
print(f'{len(files)} files, {total / 1048576:.1f} MB -> payload.cab {os.path.getsize(cab) / 1048576:.1f} MB, id {digest}')
