"""Cross-reference helper: callers of a function, and functions in an address range."""
import bisect, json, sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol
from ppc import decode, disasm

from regions import REFERENCE, by_id

# BKR_REGION=RENP8P|RENJ8P for another release (default: USA)
_R = by_id(os.environ['BKR_REGION']) if os.environ.get('BKR_REGION') else REFERENCE
D = Dol(_R.dol())
F = json.load(open(_R.out('functions.json')))
STARTS = [f['start'] for f in F]

def func_of(a):
    i = bisect.bisect_right(STARTS, a) - 1
    return F[i] if i >= 0 else None

def name(f):
    return f.get('name') or (f.get('sig', '').split('\t')[0].strip()) or ''

def callers(t):
    out = []
    for f in F:
        for a in range(f['start'], f['end'], 4):
            w = D.read32(a)
            if (w >> 26) == 18 and (w & 1):
                i = decode(w)
                tt = (i.li if i.aa else a + i.li) & 0xFFFFFFFF
                if tt == t:
                    out.append((a, f))
    return out

def dis(start, end=None, n=None):
    f = func_of(start)
    end = end or (f['end'] if f else start + 64)
    a = start
    while a < end and (n is None or n > 0):
        print(f'{a:08x}: {disasm(a, D.read32(a))}')
        a += 4
        if n: n -= 1

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'callers':
        t = int(sys.argv[2], 16)
        for a, f in callers(t):
            print(f'{a:08x} in {f["start"]:08x} {name(f)}')
    elif cmd == 'dis':
        dis(int(sys.argv[2], 16), int(sys.argv[3], 16) if len(sys.argv) > 3 else None)
    elif cmd == 'range':
        lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
        for f in F:
            if lo <= f['start'] < hi:
                print(f'{f["start"]:08x} {f["end"]-f["start"]:5x} {name(f)}')
