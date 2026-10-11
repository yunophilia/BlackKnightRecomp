"""Parse Wii RSO/SEL export tables (static module symbol lists)."""
import struct
import sys


def parse_sel(path):
    d = open(path, 'rb').read()
    (nxt, prv, nsec, secoff, nameoff, namesz, ver, bss) = struct.unpack_from('>8I', d, 0)
    exp_off, exp_size, exp_names = struct.unpack_from('>3I', d, 0x40)
    sections = [struct.unpack_from('>2I', d, secoff + i * 8) for i in range(nsec)]
    syms = []
    for i in range(exp_size // 16):
        noff, value, sec, h = struct.unpack_from('>4I', d, exp_off + i * 16)
        p = exp_names + noff
        name = d[p:d.index(b'\0', p)].decode()
        syms.append((name, value, sec))
    return sections, syms


if __name__ == '__main__':
    secs, syms = parse_sel(sys.argv[1])
    for s, (name, value, sec) in enumerate(syms):
        print(f'{value:08x} sec{sec:2d} {name}')
