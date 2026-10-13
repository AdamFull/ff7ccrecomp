import struct,sys,collections
d=open(sys.argv[1],'rb').read()
n=len(d)
print("fse size",n, "mod12 from 8:",(n-8)%12)
ents=[]
o=8
while o+12<=n:
    a,b,c=struct.unpack('<III',d[o:o+12]); ents.append((o,a,b,c)); o+=12
for e in ents[:12]: print([hex(x) for x in e])
print("...")
for e in ents[-8:]: print([hex(x) for x in e])
fl=collections.Counter(e[1] for e in ents); print("first field top:",[(hex(k),v) for k,v in fl.most_common(10)])
# check contiguity
bad=0
for i in range(len(ents)-1):
    _,a,s,z=ents[i]; _,a2,s2,z2=ents[i+1]
    if s2!=s+(z+0x7ff)//0x800: bad+=1
print("entries",len(ents),"noncontiguous",bad)
last=max(ents,key=lambda e:e[2]); print("max sector",hex(last[2]),"end byte",hex(last[2]*0x800+last[3]))
