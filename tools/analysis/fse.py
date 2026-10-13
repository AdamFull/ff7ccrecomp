# discimg.fse = array of {u32 sector (2048-byte units), u32 size, u32 reserved(0)}.
# Entry 0 is the pkg's own 'Atel' header at sector 0 (3088 bytes).
import struct,sys
d=open(sys.argv[1],'rb').read()
assert len(d)%12==0, "unexpected fse size"
ents=[struct.unpack('<III',d[o:o+12]) for o in range(0,len(d),12)]
print("entries",len(ents),"reserved nonzero:",sum(1 for e in ents if e[2]))
for i,e in enumerate(ents[:12]): print(i,[hex(x) for x in e])
print("...")
for i,e in list(enumerate(ents))[-4:]: print(i,[hex(x) for x in e])
bad=sum(1 for i in range(len(ents)-1) if ents[i+1][0]!=ents[i][0]+(ents[i][1]+0x7ff)//0x800)
print("noncontiguous",bad)
s,z,_=ents[-1]; print("archive end",hex(s*0x800+z))
