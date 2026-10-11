"""Match functions against Dolphin's DSY signature database (same hash as Dolphin)."""
import json
import struct
import sys

from dol import Dol


def checksum(words):
    s = 0
    for opcode in words:
        op = opcode & 0xFC000000
        op2 = 0
        op3 = 0
        aux = op >> 26
        if aux == 4:
            op2 = opcode & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = opcode & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = opcode & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = opcode & 0x7FF
        elif aux == 59:
            op2 = opcode & 0x3F
            if op2 < 16:
                op3 = opcode & 0x7C0
        elif 32 <= aux < 56:
            op2 = opcode & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x1FFFF)
        s ^= (op | op2 | op3)
    return s


def load_dsy(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    db = {}
    for i in range(n):
        cs, size = struct.unpack_from('<II', d, 4 + i * 136)
        name = d[4 + i * 136 + 8: 4 + i * 136 + 136].split(b'\0')[0].decode('latin1')
        db.setdefault(cs, []).append((name, size))
    return db


if __name__ == '__main__':
    dol = Dol(sys.argv[1])
    funcs = json.load(open(sys.argv[2]))
    db = load_dsy(sys.argv[3])
    hits = 0
    for f in funcs:
        n = (f['end'] - f['start']) // 4
        words = [dol.read32(f['start'] + 4 * k) for k in range(n)]
        cs = checksum(words)
        m = db.get(cs)
        if m:
            cands = [nm for nm, sz in m if sz == n * 4]
            if len(cands) >= 1 and n > 3:
                f['sig'] = cands[0]
                hits += 1
    print(f'{hits} signature matches', file=sys.stderr)
    json.dump(funcs, open(sys.argv[2], 'w'))
