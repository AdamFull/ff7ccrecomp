import struct,sys
d=open(sys.argv[1],'rb').read()
base=0x74  # vaddr->off for first segment
def o(a): return a+base
stub,stube=0x19e03c,0x19e1f4
p=stub
while p<stube:
    na,ver,attr,esz,vc,fc,nids,funcs=struct.unpack('<IHHBBHII',d[o(p):o(p)+20])
    n=d[o(na):d.index(b'\0',o(na))].decode()
    ns=struct.unpack(f'<{fc}I',d[o(nids):o(nids)+4*fc])
    print(n, ' '.join(f'{x:08X}' for x in ns))
    p+=esz*4
