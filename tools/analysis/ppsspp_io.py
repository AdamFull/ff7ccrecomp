# Maps the game's discimg.pkg reads in a PPSSPP debug log (run with --log=<file> -d) to archive entries.
#
# The game reads the archive through the raw UMD block devices (umd0:/umd1:): sceIoLseek takes an absolute
# sector (LBN) and sceIoRead a sector count. The pkg's start LBN comes from the log's sceIoGetstat line.
#
#   python -I ppsspp_io.py <ppsspp log> <discimg.fse> [manifest.tsv]
import bisect
import re
import struct
import sys

log_path, fse_path = sys.argv[1], sys.argv[2]
fse = open(fse_path, 'rb').read()
entries = [struct.unpack('<III', fse[i:i + 12]) for i in range(0, len(fse), 12)]
starts = [e[0] for e in entries]

types = {}
if len(sys.argv) > 3:
    for line in open(sys.argv[3], encoding='utf8'):
        if not line.startswith('#'):
            fields = line.rstrip('\n').split('\t')
            types[int(fields[0])] = fields[1]

pkg_lbn = None
pos = {}  # fd -> current sector relative to the pkg
last = None
for line in open(log_path, encoding='utf8', errors='replace'):
    if pkg_lbn is None:
        m = re.search(r'sceIoGetstat\(disc0:/PSP_GAME/USRDIR/discimg\.pkg.*sector = ([0-9a-f]+)', line)
        if m:
            pkg_lbn = int(m.group(1), 16)
        continue
    m = re.search(r'sceIoLseek\((\d+), ([0-9a-f]+), 0\)', line)
    if m:
        pos[m.group(1)] = int(m.group(2), 16) - pkg_lbn
        continue
    m = re.search(r'sceIoRead\((\d+), [0-9a-f]+, (\d+)\)', line)
    if not m or m.group(1) not in pos:
        continue
    fd, count = m.group(1), int(m.group(2))
    sector = pos[fd]
    i = bisect.bisect_right(starts, sector) - 1
    while i + 1 < len(entries) and entries[i + 1][0] == sector and entries[i][1] == 0:
        i += 1  # skip empty entries that share the sector
    pos[fd] = sector + count
    at_start = entries[i][0] == sector
    if (i, at_start) == last:
        continue  # collapse sequential streaming reads of the same entry
    last = (i, at_start)
    print(f"{line[:9]} fd{fd} entry {i:5d} {'start' if at_start else 'cont '} "
          f"{types.get(i, '?'):5s} size={entries[i][1]}")

if pkg_lbn is None:
    sys.exit("discimg.pkg sceIoGetstat line not found (was the log captured with -d?)")
