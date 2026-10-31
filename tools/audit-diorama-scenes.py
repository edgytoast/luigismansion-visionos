#!/usr/bin/env python3
"""Audit event camera ownership from the user's own ISO; writes metadata only, never script text."""
import argparse,re,struct
from pathlib import Path
from make_iso import parse_fst
from rarc import decompress,rarc_files
p=argparse.ArgumentParser();p.add_argument('disc');p.add_argument('output');a=p.parse_args()
rows=[]
with Path(a.disc).open('rb') as f:
    f.seek(0x424);offset,size=struct.unpack('>II',f.read(8));f.seek(offset);fst=f.read(size)
    for name,offset,size in parse_fst(fst):
        match=re.fullmatch(r'Event/event(\d+)\.szp',name,re.I)
        if not match:continue
        f.seek(offset);archive=f.read(size)
        for child,data in rarc_files(decompress(archive)):
            if not child.startswith('text/') or not child.endswith('.txt'):continue
            tags=set(re.findall(rb'<([A-Z0-9_]+)>',data))
            camera=sorted(t.decode() for t in tags if t in (b'GCAMERA',b'CAMERA',b'CAMTARGET',b'CAMTARGET2',b'PDEMO'))
            rows.append((int(match[1]),', '.join(camera) or 'None',b'WARP' in tags,b'SPEAK' in tags,b'SAVE' in tags or b'CHOICE' in tags))
rows.sort()
lines=['# Diorama event-camera audit (build 126)','',
'Generated from the locally mounted PAL disc. This is camera-command metadata, not retail script content.','',
'General policy: rendered story/gameplay and dedicated item/door cameras use tracked diorama 3D when the new option is enabled. The classification does not depend on individual event IDs. Explicit menus, map/search, results, and Game Over retain their baseline rules. Pre-rendered movies remain 2D because the disc contains no 3D scene for them.','',
'Existing accepted per-event framing is retained as the fallback when the option is disabled. Native control locks, event flags, warps, ghost AI, damage, and rewards are unchanged.','',
'| Event | Camera commands | Warp | Dialogue | Save/choice commands |','|---|---|---|---|---|']
for event,camera,warp,dialogue,ui in rows:lines.append(f'| {event:02} | {camera} | {"Yes" if warp else ""} | {"Yes" if dialogue else ""} | {"Yes" if ui else ""} |')
lines+=['',f'Audited {len(rows)} event scripts. Native item/door rigs are covered separately through the existing player-demo draw marker and primary perspective-subview selection. Opening, staff-roll and ending scenes expand only when real perspective geometry was captured.','',
'Validation covers the complete presentation state matrix and the existing camera/avatar/checkpoint/HLE regression checks. This audit does not mean every camera has been visually verified in a headset.']
Path(a.output).write_text('\n'.join(lines)+'\n');print(f'Audited {len(rows)} event scripts; metadata written to {a.output}')
