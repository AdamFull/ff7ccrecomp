import struct,sys,math,collections,random
fse=open(sys.argv[1],'rb').read(); pkg=open(sys.argv[2],'rb')
ents=[struct.unpack('<III',fse[o:o+12]) for o in range(8,len(fse)-11,12)]
def H(b):
    c=collections.Counter(b); n=len(b); return -sum(v/n*math.log2(v/n) for v in c.values()) if n else 0
pkg.seek(0); hdr=pkg.read(0x1000); print("pkg hdr entropy %.2f"%H(hdr)); print(hdr[:64].hex(' '))
idx=list(range(12))+sorted(random.Random(1).sample(range(len(ents)),25))
mags=collections.Counter()
for i in range(len(ents)):
    _,s,z=ents[i]
    if z==0: continue
    pkg.seek(s*0x800); b=pkg.read(min(z,16)); mags[b[:4]]+=1
for i in idx:
    _,s,z=ents[i]
    pkg.seek(s*0x800); b=pkg.read(min(z,4096))
    print(f"#{i:5d} sec={s:#7x} size={z:#9x} H={H(b):.2f} {b[:32].hex(' ')}")
print("top 4-byte magics:")
for k,v in mags.most_common(25): print(" ",k.hex(' '),repr(k),v)
