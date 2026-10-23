"""Address correspondence between two releases' main.dol (USA -> Europe/Japan).

The releases are built from nearly the same code: the text differs by a few hundred
bytes, so every function after a change moves, and every data section after the text
moves by one fixed amount. RegionMap aligns the function lists (instructions compared
with branch displacements and address immediates masked), then the instructions inside
functions that differ, and learns each data section's shift from the address pairs the
matched code loads (lis + addi/lwz/... at the same place in both). Every learned pair
must agree with the shift of its section, or the map refuses to load.

  python tools/region_map.py RENP8P [ADDR...]   statistics, or map the given USA addresses
"""
import bisect
import difflib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol                              # noqa: E402
from regions import REFERENCE, by_id             # noqa: E402

ADDR_IMM_OPS = {14, 15, 24, 25} | set(range(32, 56))   # addi addis ori oris, loads/stores
SAFE_BASES = (1, 2, 13)                                 # stack, small data: same offsets
MIN_RUN = 8                                             # instructions


def norm(w):
    """Instruction word with what moves between releases masked out."""
    op = w >> 26
    if op == 18:                      # b
        return w & 0xFC000003
    if op == 16:                      # bc
        return w & 0xFFFF0003
    if op in ADDR_IMM_OPS and ((w >> 16) & 31) not in SAFE_BASES:
        return w & 0xFFFF0000
    return w


def load_funcs(path):
    return [(f['start'], f['end']) for f in json.load(open(path))]


class RegionMap:
    def __init__(self, dol_a, funcs_a, dol_b, funcs_b):
        self.a, self.b = dol_a, dol_b
        self.runs = []        # (a_start, a_end, b_start): instructions that correspond 1:1
        self.func_pairs = {}  # a function start -> b function start
        self.exact = {}       # the pairs whose instructions are the same (masked as in norm)
        self._align(funcs_a, funcs_b)
        self.shifts = self._learn_shifts()

    # ---------------------------------------------------------------- text
    def _words(self, s, e, dol):
        return [norm(dol.read32(x)) for x in range(s, e, 4)]

    def _align(self, fa, fb):
        ka = [tuple(self._words(s, e, self.a)) for s, e in fa]
        kb = [tuple(self._words(s, e, self.b)) for s, e in fb]
        self._func_end = dict(fa)
        self._func_end_b = dict(fb)
        sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)
        blocks = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal':
                for i, j in zip(range(i1, i2), range(j1, j2)):
                    self.runs.append((fa[i][0], fa[i][1], fb[j][0]))
                    self.func_pairs[fa[i][0]] = fb[j][0]
                    self.exact[fa[i][0]] = fb[j][0]
            elif tag == 'replace':
                blocks.append((fa[i1:i2], fb[j1:j2]))
                self._align_code(fa[i1][0], fa[i2 - 1][1], fb[j1][0], fb[j2 - 1][1])
        self.runs.sort()
        self.run_starts = [r[0] for r in self.runs]
        # differing functions: paired when the start instruction has a counterpart
        for ba, bb in blocks:
            starts_b = {s for s, _ in bb}
            for s, _ in ba:
                t = self.map_code(s)
                if t is not None and t in starts_b:
                    self.func_pairs[s] = t

    def _align_code(self, a0, a1, b0, b1):
        """Instruction-level alignment of a stretch of differing functions."""
        wa, wb = self._words(a0, a1, self.a), self._words(b0, b1, self.b)
        sm = difflib.SequenceMatcher(None, wa, wb, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            # short runs inside rewritten code match by chance
            if tag == 'equal' and i2 - i1 >= MIN_RUN:
                self.runs.append((a0 + 4 * i1, a0 + 4 * i2, b0 + 4 * j1))

    def map_code(self, addr):
        """Address of the same instruction in b, or None (code that has no counterpart)."""
        i = bisect.bisect_right(self.run_starts, addr) - 1
        if i >= 0:
            s, e, t = self.runs[i]
            if s <= addr < e:
                return t + (addr - s)
        return None

    def map_func(self, addr):
        return self.func_pairs.get(addr)

    # ---------------------------------------------------------------- data
    def _sections(self, dol):
        secs = [(s.addr, s.end, s.index) for s in dol.data]
        secs.append((dol.bss_addr, dol.bss_addr + dol.bss_size, 'bss'))
        return secs

    def _section_of(self, addr):
        # data sections first: the small data sections lie inside the bss range
        for s, e, idx in sorted(self._sections(self.a), key=lambda x: x[2] == 'bss'):
            if s <= addr < e:
                return idx
        return None

    def _learn_shifts(self):
        """Per data section of a: the shift to b, from matched code's address loads."""
        seen = {}
        for s, t in self.exact.items():
            e = self._func_end[s]
            hi_a, hi_b = {}, {}
            for off in range(0, e - s, 4):
                wa, wb = self.a.read32(s + off), self.b.read32(t + off)
                op, rd, ra = wa >> 26, (wa >> 21) & 31, (wa >> 16) & 31
                if op == 15 and ra == 0:                       # lis rd, hi
                    hi_a[rd], hi_b[rd] = (wa & 0xFFFF) << 16, (wb & 0xFFFF) << 16
                    continue
                if op in ADDR_IMM_OPS and ra in hi_a and ra not in SAFE_BASES:
                    lo = lambda w: (w & 0xFFFF) if op in (24, 25) else ((w & 0xFFFF) ^ 0x8000) - 0x8000
                    ea = (hi_a[ra] + lo(wa)) & 0xFFFFFFFF
                    eb = (hi_b[ra] + lo(wb)) & 0xFFFFFFFF
                    sec = self._section_of(ea)
                    if sec is not None:
                        seen.setdefault(sec, {}).setdefault((eb - ea) & 0xFFFFFFFF, []).append(ea)
                if 32 <= op < 56 and op & 1:
                    hi_a.pop(ra, None)   # update form: ra now holds the address
                    hi_b.pop(ra, None)
                if op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55) and rd in hi_a and op != 15:
                    # rd overwritten (not a store, whose rd is the source)
                    hi_a.pop(rd, None)
                    hi_b.pop(rd, None)
        shifts = {}
        for sec, by in seen.items():
            moved = [k for k in by if k] or [0]
            best = max(moved, key=lambda k: len(by[k]))
            # values the code builds unchanged in a moved section are constants (error
            # codes like 0x80730001), not addresses
            others = {k: v for k, v in by.items() if k != best and k != 0}
            if others:
                bad = sum(len(v) for v in others.values())
                ex = ', '.join(f'{v[0]:08x} {k:+#x}' for k, v in list(others.items())[:5])
                raise RuntimeError(f'section {sec}: {bad} address loads disagree with shift '
                                   f'{best:#x} (e.g. at {ex})')
            shifts[sec] = best
        # sections whose addresses the code never builds with lis: same position in both
        for (sa, ea, idx), (sb, eb, _) in zip(self._sections(self.a), self._sections(self.b)):
            if idx not in shifts and ea - sa == eb - sb:
                shifts[idx] = (sb - sa) & 0xFFFFFFFF
        return shifts

    def map_data(self, addr):
        sec = self._section_of(addr)
        if sec is None or sec not in self.shifts:
            return None
        return (addr + self.shifts[sec]) & 0xFFFFFFFF

    def map(self, addr):
        if self.a.is_text(addr):
            return self.map_code(addr)
        if addr < 0x80004000:
            return addr        # low memory globals (OS, apploader): not part of main.dol
        return self.map_data(addr)


def load(region):
    """RegionMap from the reference release to region (function lists from out/<id>/)."""
    a = Dol(REFERENCE.dol())
    b = Dol(region.dol())
    return RegionMap(a, load_funcs(REFERENCE.out('functions.json')), b, load_funcs(region.out('functions.json')))


def main():
    region = by_id(sys.argv[1])
    m = load(region)
    if len(sys.argv) > 2:
        for x in sys.argv[2:]:
            a = int(x, 16)
            t = m.map(a)
            print(f'{a:08x} -> {t:08x}' if t is not None else f'{a:08x} -> (none)')
        return
    nf = len(load_funcs(REFERENCE.out('functions.json')))
    code = sum(e - s for s, e, _ in m.runs)
    total = sum(s.size for s in m.a.text)
    print(f'{len(m.func_pairs)}/{nf} functions paired, {code * 100 / total:.2f}% of the text mapped')
    for sec, sh in sorted(m.shifts.items(), key=str):
        print(f'  section {sec}: {sh - (1 << 32) if sh & 0x80000000 else sh:+#x}')


if __name__ == '__main__':
    main()
