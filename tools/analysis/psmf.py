# Lists PSMF movies inside discimg.pkg and dumps the PES/elementary-stream structure of one of them.
#   python psmf.py discimg.fse discimg.pkg            -> list movies (entry, size, streams, timestamps)
#   python psmf.py discimg.fse discimg.pkg ENTRY [N]   -> first N PES packets of entry ENTRY
import struct, sys

fse = open(sys.argv[1], 'rb').read()
entries = [struct.unpack('<III', fse[o:o + 12]) for o in range(0, len(fse), 12)]
pkg = open(sys.argv[2], 'rb')

def read_entry(i, size=None):
    sector, length, _ = entries[i]
    pkg.seek(sector * 2048)
    return pkg.read(length if size is None else min(size, length))

def pts(b):
    return ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1

if len(sys.argv) < 4:
    for i in range(len(entries)):
        h = read_entry(i, 0x80)
        if h[:4] == b'PSMF':
            off = struct.unpack('>I', h[8:12])[0]
            first = int.from_bytes(h[0x54:0x5A], 'big'); last = int.from_bytes(h[0x5A:0x60], 'big')
            print(f"entry {i:5d} size {entries[i][1]:10d} version {h[4:8]} data@{off} ts {first}..{last} "
                  f"({(last - first) / 90000:.1f} s)")
    sys.exit()

entry, limit = int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 40
data = read_entry(entry, 4 << 20)
off = struct.unpack('>I', data[8:12])[0]
shown = 0
for pack in range(off, len(data) - 2048, 2048):
    p = pack
    while p + 6 <= pack + 2048 and data[p:p + 3] == b'\0\0\1':
        sid = data[p + 3]
        if sid == 0xBA:
            p += 14 + (data[p + 13] & 7)
            continue
        length = struct.unpack('>H', data[p + 4:p + 6])[0]
        if sid in (0xE0, 0xBD):
            flags = data[p + 7] >> 6
            hl = data[p + 8]
            body = data[p + 9 + hl:p + 6 + length]
            t = pts(data[p + 9:p + 14]) if flags & 2 else None
            aud = body.count(b'\0\0\1\x09') if sid == 0xE0 else body.count(b'\x0f\xd0')
            print(f"pack {(pack - off) // 2048:5d} sid {sid:02X} len {length:5d} pts {t} hdr {hl} "
                  f"payload {len(body)} head {body[:12].hex()} markers {aud}")
            shown += 1
        p += 6 + length
    if shown >= limit:
        break
