#!/usr/bin/env python3
"""Offline Book2 image builder (stdlib only).

Firmware physical topology is fixed, not an MBR: A has three 148 MiB mirrors;
B is 998 MiB; logical drive2 is 48 MiB; logical drive3 spans the disk tail.
A's supplied tree includes files whose embedded runtime paths target B and D.
Build A core resources, B applications plus dictionary data, C user storage,
and D learning content. D/__archive/A also preserves the original A tree.
All source files and their SHA256 are retained; dictionary search/fallback and
application execution still require guest validation. See the audit README.

LFN uses standard UTF-16LE FAT entries; names must round-trip GBK for firmware.
SFN uses unique ASCII aliases. No external tools, mounts, pip or intermediate
multi-GB volume images are needed. Existing output is refused by default.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import sys

SECTOR=512
SPC=16
CLUSTER=SECTOR*SPC
RESERVED=32
ROOT=Path(__file__).resolve().parents[1]
DEFAULT_RES=ROOT.parent/'book2-res'
RAW={'data0.dat.bin':[0], 'data2.dat.bin':[0x800,0x7800,0x10000],
     'data3.dat.bin':[0x2800,0x9800,0x12000], 'data4.dat.bin':[0x5800,0xc800,0x15000]}
A_BASES=[0x1a000,0x65000,0xb0000]
A_SECTORS=0x4a000
B_BASE=0xfb000
B_SECTORS=0x1f3000
C_BASE=0x2ef000
C_SECTORS=0x18000
ARCHIVE_BASE=0x308000
CORE_PREFIXES=('FONT/', '系统/配置/', '系统/数据/shell/', '系统/数据/TeacherPic/')

def core_resource(rel):
    return rel.startswith(CORE_PREFIXES) or rel.rpartition('/')[0]=='系统/数据'

def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def sha_file(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''): h.update(b)
    return h.hexdigest()

def checksum(sfn):
    s=0
    for c in sfn: s=(((s&1)<<7)+(s>>1)+c)&255
    return s

def lfn(name,sfn):
    raw=name.encode('utf-16le')
    units=list(struct.unpack('<'+'H'*(len(raw)//2),raw))
    if not 1<=len(units)<=255: raise ValueError('FAT LFN length: '+name)
    units.append(0)
    units += [0xffff]*((-len(units))%13)
    out=[]
    for i in range(len(units)//13,0,-1):
        e=bytearray(32);e[0]=i|(0x40 if i==len(units)//13 else 0)
        e[11]=15;e[13]=checksum(sfn)
        for pos,v in zip((1,3,5,7,9,14,16,18,20,22,24,28,30),units[(i-1)*13:i*13]):struct.pack_into('<H',e,pos,v)
        out.append(bytes(e))
    return b''.join(out)

def short_alias(name,used):
    base,sep,ext=name.rpartition('.')
    if not sep:base,ext=name,''
    clean=lambda s:re.sub('[^A-Z0-9_$~-]','_',s.upper())
    base=clean(base) or '_';ext=clean(ext)[:3]
    for n in range(1,1000000):
        stem=(base[:max(1,8-len(str(n))-1)]+'~'+str(n))[:8]
        s=(stem.ljust(8)+ext.ljust(3)).encode('ascii')
        if s not in used:used.add(s);return s
    raise ValueError('SFN namespace exhausted')

def entry(sfn,attr,start,size=0):
    b=bytearray(32);b[:11]=sfn;b[11]=attr
    # Fixed 2000-01-01 timestamps make builds reproducible.
    for off in [16,18,24]:struct.pack_into('<H',b,off,0x2821)
    struct.pack_into('<H',b,20,start>>16);struct.pack_into('<H',b,26,start&65535)
    struct.pack_into('<I',b,28,size)
    return bytes(b)

class Node:
    def __init__(self,name,parent=None,source=None):
        self.name=name;self.parent=parent;self.source=source;self.children=[]
        self.size=source.stat().st_size if source else 0
        self.start=0;self.count=0;self.sfn=b'';self.digest=None
    @property
    def directory(self):return self.source is None
    @property
    def path(self):return (self.parent.path+'/' if self.parent and self.parent.name else '')+self.name

class Volume:
    def __init__(self,label,base,sectors):
        self.label=label;self.base=base;self.sectors=sectors;self.root=Node('');self.nodes=[self.root]
        self.fatsz=1
        while True:
            clusters=(sectors-RESERVED-2*self.fatsz)//SPC
            need=math.ceil((clusters+2)*4/SECTOR)
            if need<=self.fatsz:break
            self.fatsz=need
        self.clusters=clusters
        self.data_sector=RESERVED+2*self.fatsz
    def add(self,path,source):
        parts=path.split('/');parent=self.root
        for part in parts:
            part.encode('gbk',errors='strict')
            if any(c in part for c in '\\/:*?"<>|') or part[-1:] in (' ','.'):
                raise ValueError('Unsupported FAT name '+path)
        for part in parts[:-1]:
            match=next((c for c in parent.children if c.name==part),None)
            if match is None:
                if any(c.name.casefold()==part.casefold() for c in parent.children):raise ValueError('Case collision '+path)
                match=Node(part,parent);parent.children.append(match);self.nodes.append(match)
            if not match.directory:raise ValueError('File/directory collision '+path)
            parent=match
        if any(c.name.casefold()==parts[-1].casefold() for c in parent.children):raise ValueError('Case collision '+path)
        node=Node(parts[-1],parent,source);parent.children.append(node);self.nodes.append(node)
    def plan(self):
        for node in self.nodes:
            if node.directory:
                node.children.sort(key=lambda c:c.name.encode('gbk'))
                used=set()
                for c in node.children:c.sfn=short_alias(c.name,used)
                size=(2 if node is not self.root else 0)*32+32
                for c in node.children:size+=len(lfn(c.name,c.sfn))+32
                node.size=size
            if node.size>0xffffffff:raise ValueError('FAT file exceeds 4GiB: '+node.path)
        # Allocation is deterministic. No 16-bit cluster workaround is assumed.
        ordered=[self.root]+sorted(self.nodes[1:],key=lambda n:(not n.directory,n.path.encode('gbk')))
        cursor=2
        for node in ordered:
            node.count=max(1,math.ceil(node.size/CLUSTER)) if node.directory else math.ceil(node.size/CLUSTER)
            node.start=cursor if node.count else 0;cursor+=node.count
        self.used=cursor-2
        if self.used>self.clusters:raise ValueError(f'{self.label}: needs {self.used} clusters; capacity {self.clusters}')
        self.fat=bytearray(self.fatsz*SECTOR)
        struct.pack_into('<II',self.fat,0,0x0ffffff8,0x0fffffff)
        for node in ordered:
            for c in range(node.start,node.start+node.count):
                struct.pack_into('<I',self.fat,c*4,0x0fffffff if c==node.start+node.count-1 else c+1)
    def offset(self,c):return (self.base+self.data_sector+(c-2)*SPC)*SECTOR
    def boot(self):
        b=bytearray(SECTOR);b[:11]=b'\xeb\x58\x90BOOK2AUD'
        struct.pack_into('<HBHBHHBHHHII',b,11,512,SPC,RESERVED,2,0,0,0xf8,0,63,255,self.base,self.sectors)
        struct.pack_into('<IHHIHH',b,36,self.fatsz,0,0,2,1,6)
        b[64]=0x80;b[66]=0x29;struct.pack_into('<I',b,67,0x324b4f42)
        b[71:82]=self.label.encode('ascii')[:11].ljust(11,b' ');b[82:90]=b'FAT32   ';b[510:]=b'\x55\xaa'
        return b
    def write(self,f):
        boot=self.boot();info=bytearray(512)
        for off,v in [(0,0x41615252),(484,0x61417272),(488,self.clusters-self.used),(492,self.used+2 if self.used<self.clusters else 0xffffffff),(508,0xaa550000)]:struct.pack_into('<I',info,off,v)
        f.seek(self.base*SECTOR);f.write(bytes(RESERVED*SECTOR))
        for s in (0,6):f.seek((self.base+s)*SECTOR);f.write(boot);f.write(info)
        for i in (0,1):f.seek((self.base+RESERVED+i*self.fatsz)*SECTOR);f.write(self.fat)
        for n in self.nodes:
            if n.directory:
                data=b''
                if n is not self.root:
                    data+=entry(b'.          ',16,n.start)
                    data+=entry(b'..         ',16,0 if n.parent is self.root else n.parent.start)
                for c in n.children:data+=lfn(c.name,c.sfn)+entry(c.sfn,16 if c.directory else 32,c.start,0 if c.directory else c.size)
                f.seek(self.offset(n.start));f.write(data.ljust(n.count*CLUSTER,b'\0'))
            elif n.count:
                f.seek(self.offset(n.start));h=hashlib.sha256();written=0
                with n.source.open('rb') as src:
                    for b in iter(lambda:src.read(1024*1024),b''):h.update(b);f.write(b);written+=len(b)
                if written!=n.size:raise ValueError('Source changed during build: '+str(n.source))
                n.digest=h.hexdigest();f.write(bytes(n.count*CLUSTER-written))
            else:n.digest=hashlib.sha256(b'').hexdigest()
        print(f'{self.label}: {sum(not n.directory for n in self.nodes)} files, {self.used}/{self.clusters} clusters',flush=True)
    def manifest(self):
        return {'label':self.label,'base_lba':self.base,'sectors':self.sectors,'clusters':self.clusters,'used_clusters':self.used,'standard_fat32_cluster_count':self.clusters>=65525,
                'files':{n.path:{'size':n.size,'sha256':n.digest,'start_cluster':n.start} for n in self.nodes if not n.directory}}

def read_volume(f,base,sectors):
    """Independent FAT/LFN reader: validates both FATs, backups, chains and hashes."""
    f.seek(base*512);b=f.read(512)
    if b[82:90]!=b'FAT32   ' or b[510:]!=b'\x55\xaa' or u16(b,11)!=512 or b[13]!=16:raise ValueError('Invalid BPB')
    if u32(b,32)!=sectors:raise ValueError('Volume length mismatch')
    if u32(b,28)!=base or b[16]!=2 or u16(b,14)!=RESERVED:raise ValueError('Volume geometry mismatch')
    rs=u16(b,14);fs=u32(b,36);data=rs+b[16]*fs
    for s in (6,):
        f.seek((base+s)*512)
        if f.read(512)!=b:raise ValueError('Boot backup mismatch')
    f.seek((base+rs)*512);fat=f.read(fs*512)
    f.seek((base+rs+fs)*512)
    if f.read(fs*512)!=fat:raise ValueError('FAT copies differ')
    maxc=(sectors-data)//16+1;owners=set();result={}
    def chain(c):
        seen=set()
        while c<0xffffff8:
            if c<2 or c>maxc or c in seen or c in owners:raise ValueError('Invalid/crosslinked FAT chain')
            seen.add(c);owners.add(c);yield c;c=u32(fat,c*4)&0xfffffff
    def blocks(c):
        for cluster in chain(c):
            f.seek((base+data+(cluster-2)*16)*512);yield f.read(CLUSTER)
    def walk(start,prefix):
        raw=b''.join(blocks(start));lf=[]
        for off in range(0,len(raw),32):
            e=raw[off:off+32]
            if e[0]==0:break
            if e[0]==0xe5:lf=[];continue
            if e[11]==15:lf.append(e);continue
            if e[0]==46:lf=[];continue
            if not lf:raise ValueError('Expected LFN')
            count=lf[0][0]&31
            if len(lf)!=count or not lf[0][0]&64:raise ValueError('LFN sequence length')
            namebytes=b''
            for idx,part in enumerate(reversed(lf),1):
                if part[0]&31!=idx or part[13]!=checksum(e[:11]) or part[12]!=0 or part[26:28]!=b'\0\0':raise ValueError('LFN checksum/order')
                namebytes+=part[1:11]+part[14:26]+part[28:32]
            units=[namebytes[i:i+2] for i in range(0,len(namebytes),2)];units=units[:units.index(b'\0\0')] if b'\0\0' in units else units
            name=b''.join(units).decode('utf-16le');name.encode('gbk');lf=[]
            path=prefix+name;c=(u16(e,20)<<16)|u16(e,26);size=u32(e,28)
            if e[11]&16:walk(c,path+'/')
            else:
                h=hashlib.sha256();remaining=size;clusters=0
                if c:
                    for block in blocks(c):
                        take=min(remaining,len(block));h.update(block[:take]);remaining-=take;clusters+=1
                if remaining or clusters!=math.ceil(size/CLUSTER):raise ValueError('File chain length: '+path)
                if path in result:raise ValueError('Duplicate directory entry '+path)
                result[path]={'size':size,'sha256':h.hexdigest(),'start_cluster':c}
    walk(u32(b,44),'')
    # Every allocated data cluster must belong to an observed file or directory.
    allocated={i for i in range(2,maxc+1) if u32(fat,i*4)&0xfffffff}
    if allocated!=owners:raise ValueError('Lost allocated clusters')
    f.seek((base+1)*512);info=f.read(512)
    if (u32(info,0),u32(info,484),u32(info,508))!=(0x41615252,0x61417272,0xaa550000):raise ValueError('FSInfo signature mismatch')
    if u32(info,488)!=(maxc-1)-len(owners):raise ValueError('FSInfo free count mismatch')
    f.seek((base+7)*512)
    if f.read(512)!=info:raise ValueError('FSInfo backup mismatch')
    return result

def preservation_path(source_relative):
    disk,rel=source_relative.split('/',1)
    return '__archive/A/'+rel if disk=='A' else rel

def make_plan(res,size,apps=(),includes=()):
    selected=set()
    aroot=res/'res/A'
    for app in apps:
        if not app or '/' in app or '\\' in app:raise ValueError('--app requires a bare application name')
        binary=aroot/'应用/程序'/(app+'.bda')
        if not binary.is_file():raise ValueError('Unknown application '+app)
        # All supplied B application resources are included by default.
        # --app validates a requested profile name without guessing dependencies.
    for pattern in includes:
        if Path(pattern).is_absolute() or '..' in Path(pattern).parts:raise ValueError('--a-include must stay within A')
        matches=list(aroot.glob(pattern))
        if not matches:raise ValueError('No A resource matches '+pattern)
        for p in matches:
            for item in ([p] if p.is_file() else p.rglob('*')):
                if item.is_file():selected.add(item.relative_to(aroot).as_posix())
    av=Volume('BOOK2A',A_BASES[0],A_SECTORS);bv=Volume('BOOK2B',B_BASE,B_SECTORS)
    cv=Volume('BOOK2C',C_BASE,C_SECTORS)
    archive=Volume('BOOK2D',ARCHIVE_BASE,size//512-ARCHIVE_BASE)
    omitted=[];sources={}
    for disk in ('A','B'):
        source=res/'res'/disk
        if not source.is_dir():raise ValueError('Missing resource directory '+str(source))
        for path in sorted(source.rglob('*')):
            if path.is_symlink():raise ValueError('Symlink input unsupported: '+str(path))
            if not path.is_file():continue
            rel=path.relative_to(source).as_posix();sources[disk+'/'+rel]=path
            archive.add(preservation_path(disk+'/'+rel),path)
            if disk=='A':
                if rel.startswith('应用/'):
                    bv.add(rel,path)
                elif rel.startswith('系统/数据/词典/'):
                    bv.add('词典/'+rel.split('/',3)[3],path)
                elif rel.startswith('系统/数据/词典音库/'):
                    bv.add('词典音库/'+rel.split('/',3)[3],path)
                if (core_resource(rel) or rel in selected) and not path.name.startswith('._') and path.name!='.DS_Store':av.add(rel,path)
                else:omitted.append(rel)
    for v in (av,bv,cv,archive):v.plan()
    return [av,bv,cv,archive],omitted,sources

def verify_image(out,manifest,res):
    if out.stat().st_size!=manifest['size']:raise ValueError('Image length differs from manifest')
    with out.open('rb') as f:
        for name,bases in RAW.items():
            d=(res/'core'/name).read_bytes()
            for base in bases:
                f.seek(base*512)
                if f.read(len(d))!=d:raise ValueError('Firmware mismatch '+name+' '+hex(base))
        for v in manifest['volumes']:
            actual=read_volume(f,v['base_lba'],v['sectors'])
            if actual!=v['files']:raise ValueError('Resource mismatch '+v['label'])
        a=manifest['volumes'][0]
        for base in A_BASES[1:]:
            if read_volume(f,base,A_SECTORS)!=a['files']:raise ValueError('A mirror mismatch')
    return {'firmware_copies':sum(map(len,RAW.values())),'a_mirrors':3,'volumes':6,'archive_files':len(manifest['volumes'][-1]['files']),
            'all_file_hashes_verified':True,'firmware_boot_verified':False}

def main():
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--res',type=Path,default=DEFAULT_RES)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--size-mib',type=int,default=3072)
    ap.add_argument('--manifest',type=Path)
    ap.add_argument('--plan',action='store_true',help='Print capacity plan without writing output')
    ap.add_argument('--app',action='append',default=[],help='Validate a requested application name; all supplied B application resources are included by default')
    ap.add_argument('--a-include',action='append',default=[],help='Include an additional A-relative file/directory/glob; repeatable, all selections must fit 148MiB')
    ap.add_argument('--verify-only',action='store_true',help='Re-read image and compare manifest plus current sources')
    ap.add_argument('--resume-raw-bootstrap',action='store_true',help='Only populate an existing raw-only image created by this audit; refuses any nonzero volume region')
    args=ap.parse_args();out=args.out.resolve();res=args.res.resolve();size=args.size_mib*1024**2
    mp=args.manifest or Path(str(out)+'.manifest.json')
    if args.verify_only:
        manifest=json.loads(mp.read_text(encoding='utf8'))
        current={disk+'/'+p.relative_to(res/'res'/disk).as_posix() for disk in ('A','B') for p in (res/'res'/disk).rglob('*') if p.is_file()}
        if {preservation_path(p) for p in current}!=set(manifest['volumes'][-1]['files']):raise ValueError('Current input path set differs from preservation manifest')
        for rel in current:
            record=manifest['volumes'][-1]['files'][preservation_path(rel)]
            if sha_file(res/'res'/rel)!=record['sha256']:raise ValueError('Input changed '+rel)
        print(json.dumps(verify_image(out,manifest,res),indent=2));return
    if size%524288 or size//512<0x309000:raise ValueError('Image must be >=0x309000 sectors and 512KiB aligned')
    volumes,omitted,sources=make_plan(res,size,args.app,args.a_include)
    for v in volumes:print(f'{v.label}: LBA {v.base:#x}, sectors {v.sectors:#x}, files {sum(not n.directory for n in v.nodes)}, allocated {v.used*CLUSTER:,} bytes')
    print(f'A runtime subset omits {len(omitted)} paths; entire input preserved in tail archive (firmware access unverified).')
    if args.plan:return
    if mp.exists():raise ValueError('Refusing existing manifest '+str(mp))
    if out.exists() and not args.resume_raw_bootstrap:raise ValueError('Refusing existing output '+str(out))
    out.parent.mkdir(parents=True,exist_ok=True)
    if not out.exists() and shutil.disk_usage(out.parent).free<size:raise ValueError('Insufficient disk space')
    # Preflight raw firmware extents before creating or modifying output.
    raw=[]
    for name,bases in RAW.items():
        d=(res/'core'/name).read_bytes()
        limit={'data0.dat.bin':0x800,'data2.dat.bin':0x2000,'data3.dat.bin':0x3000,'data4.dat.bin':0x800}[name]*512
        if len(d)>limit:raise ValueError('Firmware exceeds reserved extent '+name)
        raw.extend((base,d,name) for base in bases)
    if args.resume_raw_bootstrap:
        if not out.exists() or out.stat().st_size!=size:raise ValueError('Bootstrap size mismatch')
        with out.open('rb') as f:
            for base,d,name in raw:
                f.seek(base*512)
                if f.read(len(d))!=d:raise ValueError('Bootstrap firmware mismatch '+name)
            # Scan every byte of future volume regions, not merely headers.
            for start,end in [(A_BASES[0],A_BASES[0]+A_SECTORS),(A_BASES[1],A_BASES[1]+A_SECTORS),(A_BASES[2],A_BASES[2]+A_SECTORS),(B_BASE,size//512)]:
                f.seek(start*512);left=(end-start)*512
                while left:
                    b=f.read(min(left,4*1024**2))
                    if not b or any(b):raise ValueError('Resume requires a raw-only zeroed volume region')
                    left-=len(b)
    manifest={'format':'book2-image-audit-v1','size':size,'recipe':{'apps':args.app,'a_include':args.a_include},'lfn':'UTF-16LE standard FAT LFN; input GBK roundtrip validated','a_omitted_runtime_paths':omitted,
              'limitations':['A contains core resources; B applications/dictionaries and D learning data follow embedded binary paths; D/__archive/A preserves the entire original A tree', 'A and C use firmware-required FAT32 with cluster counts below standard FAT32 threshold', 'Dictionary fallback paths, D accessibility and application execution require guest verification; no boot claim'], 'firmware':[],'volumes':[]}
    mode='r+b' if args.resume_raw_bootstrap else 'x+b'
    with out.open(mode) as f:
        if not args.resume_raw_bootstrap:f.truncate(size)
        for base,d,name in raw:
            f.seek(base*512);f.write(d);manifest['firmware'].append({'name':name,'base_lba':base,'size':len(d),'sha256':hashlib.sha256(d).hexdigest()})
        for v in volumes:v.write(f);manifest['volumes'].append(v.manifest())
        # Mirror only actual 148 MiB A volume; no giant intermediate image.
        av=volumes[0]
        for base in A_BASES[1:]:
            pos=0
            while pos<A_SECTORS*512:
                count=min(4*1024**2,A_SECTORS*512-pos)
                f.seek(A_BASES[0]*512+pos);b=f.read(count)
                f.seek(base*512+pos);f.write(b);pos+=count
            # Hidden-sector BPB field reflects each physical mirror location.
            for sector in (0,6):f.seek((base+sector)*512+28);f.write(struct.pack('<I',base))
        f.flush();os.fsync(f.fileno())
    # All original input files, including metadata files, must remain present.
    archive_files=manifest['volumes'][-1]['files']
    if set(archive_files)!={preservation_path(p) for p in sources}:raise ValueError('Preservation volume missing input paths')
    for rel,p in sources.items():
        if sha_file(p)!=archive_files[preservation_path(rel)]['sha256']:raise ValueError('Source changed '+rel)
    manifest['source_preservation']={rel:{'volume':'BOOK2D','path':preservation_path(rel)} for rel in sources}
    manifest['verification']=verify_image(out,manifest,res)
    with mp.open('x',encoding='utf8') as f:json.dump(manifest,f,ensure_ascii=False,indent=2)
    print(json.dumps(manifest['verification'],indent=2));print('Image:',out,'Manifest:',mp)

if __name__=='__main__':
    try:main()
    except (OSError,ValueError,UnicodeError) as exc:print('ERROR:',exc,file=sys.stderr);raise SystemExit(1)
