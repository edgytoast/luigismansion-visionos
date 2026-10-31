"""Inventory native interaction metadata from a locally owned ISO; writes only an audit.
Usage: python tools/audit-vr-interactions.py --disc OWN.iso --out work/interactions.json
No disc bytes, models or scripts are written. This does not modify game progression.
"""
import argparse,json,math,re,struct
from pathlib import Path
from make_iso import parse_fst
from rarc import rarc_files,decompress

def name_hash(name):
    value=0
    for c in name.encode():value=((c+(value<<8))&0xFFFFFFFF)%0x1FFFFD9
    return value

def parse_jmp(data):
    if len(data)<16:raise ValueError('Truncated JMP header')
    count,fields,start,stride=struct.unpack_from('>4I',data)
    if not stride or fields>4096 or 16+fields*12>len(data) or start<16+fields*12 or start+count*stride>len(data):raise ValueError('Invalid JMP bounds')
    layout={}
    for i in range(fields):
        key,mask,offset,shift,kind=struct.unpack_from('>IIHBB',data,16+i*12)
        if offset>=stride:raise ValueError('Invalid JMP field offset')
        layout[key]=(offset,kind,mask,shift)
    names=['dmd_name','room_no','pos_x','pos_y','pos_z','dir_x','dir_y','dir_z','scale_x','scale_y','scale_z','arg0','arg1','arg2','arg3','item_table','item_no','access','type']
    result=[]
    for i in range(count):
        row={'record':i};base=start+i*stride
        for name in names:
            field=layout.get(name_hash(name))
            if field is None:continue
            offset,kind,mask,shift=field;available=stride-offset
            if kind==1:val=data[base+offset:base+offset+min(32,available)].split(b'\0')[0].decode('shift_jis',errors='replace')
            elif kind==2:
                if available<4:raise ValueError('Truncated float')
                val=struct.unpack_from('>f',data,base+offset)[0]
                if not math.isfinite(val):raise ValueError('Non-finite transform')
            elif kind in (0,3):
                if available<4:raise ValueError('Truncated word')
                val=(struct.unpack_from('>I',data,base+offset)[0]&mask)>>shift
            elif kind==4:
                if available<2:raise ValueError('Truncated halfword')
                val=(struct.unpack_from('>H',data,base+offset)[0]&mask)>>shift
            elif kind==5:val=(data[base+offset]&mask)>>shift
            else:continue
            row[name]=val
        result.append(row)
    return {'records':result,'unknown_field_hashes':[hex(k) for k in layout if k not in {name_hash(n) for n in names}]}

def audit(disc):
    output={'disc_id':'','maps':{},'note':'Metadata inventory only. Model names do not prove an object is interactable or determine its required hand count.'}
    with open(disc,'rb') as source:
        output['disc_id']=source.read(6).decode('ascii');source.seek(0x424);offset,length=struct.unpack('>II',source.read(8));source.seek(offset)
        entries=list(parse_fst(source.read(length)))
        for name,offset,length in entries:
            if not re.fullmatch(r'Map/map\d+\.szp',str(name),re.I):continue
            source.seek(offset);members=rarc_files(decompress(source.read(length)));tables={}
            for path,data in members:
                if path.startswith('jmp/') and any(term in path.lower() for term in ('furniture','door','treasure','item','key')):
                    tables[path]=parse_jmp(data)
            output['maps'][str(name)]=tables
    return output
if __name__=='__main__':
    args=argparse.ArgumentParser(description=__doc__);args.add_argument('--disc',required=True);args.add_argument('--out',required=True);a=args.parse_args()
    report=audit(a.disc);Path(a.out).write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(report['disc_id'],len(report['maps']),'maps;',sum(len(t['records']) for m in report['maps'].values() for t in m.values()),'records inspected')

