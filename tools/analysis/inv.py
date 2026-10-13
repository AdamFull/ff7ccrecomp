import struct,sys,collections
fse=open(sys.argv[1],'rb').read(); pkg=open(sys.argv[2],'rb')
ents=[struct.unpack('<III',fse[o:o+12]) for o in range(0,len(fse)-11,12)]
cnt=collections.Counter(); sz=collections.Counter(); first={}
for i,(s,z,_) in enumerate(ents):
    if not z: k='(empty)'
    else:
        pkg.seek(s*0x800); b=pkg.read(16)
        if b[:4]==b'RIFF': k='RIFF/AT3 fmt=%04x'%struct.unpack('<H',b[20:22])[0] if False else 'RIFF (ATRAC3)'
        elif b[:4] in (b'Atel',b'PSMF',b'SSCF'): k=b[:4].decode()
        elif b[4:6]==b'\xff\xff'[:0] : k='?'
        elif b[6:8]==b'\xff\xff': k='anim-like (..ffff..30)'
        elif b[:4]==b'\0\0\0\0': k='zero-led'
        else: k='other'
    cnt[k]+=1; sz[k]+=z; first.setdefault(k,i)
for k,v in cnt.most_common(): print(f"{k:28s} n={v:5d} total={sz[k]/2**20:8.1f} MiB first=#{first[k]}")
# index ranges
prev=None; runs=[]
for i,(s,z,_) in enumerate(ents):
    pkg.seek(s*0x800); b=pkg.read(4) if z else b''
    k=b[:4] if b[:4] in (b'RIFF',b'Atel',b'PSMF',b'SSCF') else b'misc'
    if k!=prev: runs.append([k,i,i]); prev=k
    else: runs[-1][2]=i
big=[r for r in runs if r[2]-r[1]>=20]
for r in big: print(r[0].decode(),r[1],'-',r[2])
