"""Function boundary analysis for CodeWarrior-compiled PPC DOLs.

Strategy:
  * seed function starts from the entry point, known symbols and every `bl` target;
  * linearly sweep each text section: a function ends at an unconditional
    terminator (blr / b / bctr) once no pending forward branch targets remain
    inside it; code following a terminator (after zero padding) starts a new
    function;
  * switch statements (`lwzx; mtctr; bctr` with a table address built by
    lis/addi) are detected so their case labels stay inside the function.
"""
import bisect
import json
import struct
import sys

from dol import Dol
from ppc import decode


class Function:
    def __init__(self, start):
        self.start = start
        self.end = start
        self.jumptables = {}   # bctr address -> list of targets
        self.name = None

    def to_json(self):
        d = {'start': self.start, 'end': self.end}
        if self.jumptables:
            d['jt'] = {str(k): v for k, v in self.jumptables.items()}
        if self.name:
            d['name'] = self.name
        return d


def branch_target(addr, ins):
    if ins.op == 'b':
        return (ins.li if ins.aa else addr + ins.li) & 0xFFFFFFFF
    if ins.op == 'bc':
        return (ins.bd if ins.aa else addr + ins.bd) & 0xFFFFFFFF
    return None


def is_uncond_bc(ins):
    # BO = 1z1zz means "branch always"
    return (ins.bo & 0x14) == 0x14


class Analyzer:
    def __init__(self, dol, known_starts=()):
        self.dol = dol
        self.words = {}
        for s in dol.text:
            for i, w in enumerate(dol.words(s)):
                self.words[s.addr + i * 4] = w
        self.starts = set()
        self.starts.add(dol.entry)
        for a in known_starts:
            if a in self.words:
                self.starts.add(a)
        # every bl target
        for a, w in self.words.items():
            if (w >> 26) == 18 and (w & 1):
                ins = decode(w)
                t = branch_target(a, ins)
                if t in self.words and t != a + 4:
                    self.starts.add(t)

    def word(self, a):
        return self.words.get(a)

    def find_jumptable(self, bctr_addr, fstart):
        """Recognise CodeWarrior switch dispatch preceding a bctr."""
        # Walk back looking for mtctr rX and the lwzx rX, rT, rI that produced it.
        regs = {}
        lwzx = None
        mtctr_reg = None
        bound = None
        a = bctr_addr - 4
        lo = max(fstart, bctr_addr - 4 * 24)
        hi_vals = {}
        table = None
        while a >= lo:
            ins = decode(self.words[a])
            if mtctr_reg is None:
                if ins.op == 'mtspr' and ins.spr == 9:
                    mtctr_reg = ins.rs
                elif ins.op not in ('or',):
                    pass
            elif lwzx is None:
                if ins.op == 'lwzx' and ins.rd == mtctr_reg:
                    lwzx = (ins.ra, ins.rb)
            else:
                if table is None:
                    # look for addi rT, rT, lo and lis rT, hi
                    if ins.op == 'addi' and ins.rd == lwzx[0] and ins.ra == lwzx[0]:
                        hi_vals['lo'] = ins.simm
                    elif ins.op == 'addi' and ins.rd == lwzx[0] and ins.ra != 0:
                        hi_vals['lo'] = ins.simm
                        hi_vals['src'] = ins.ra
                    elif ins.op == 'addis' and ins.ra == 0 and (ins.rd == lwzx[0] or ins.rd == hi_vals.get('src')) and 'lo' in hi_vals:
                        table = ((ins.simm << 16) + hi_vals['lo']) & 0xFFFFFFFF
                if bound is None and ins.op == 'cmpli':
                    bound = ins.uimm + 1
                if table is not None and bound is not None:
                    break
            a -= 4
        if table is None and lwzx is not None:
            # resolve the table base register by backward constant propagation
            table = self.resolve_const(lwzx[0], bctr_addr - 4, fstart, 0, skip_lwzx=True)
        if table is None:
            return None
        sec = self.dol.section_at(table)
        if sec is None:
            return None
        targets = []
        limit = bound if bound is not None else 4096
        for k in range(limit):
            v = self.dol.read32(table + 4 * k)
            if v is None or v not in self.words:
                break
            if bound is None and (v < fstart or v > fstart + 0x10000):
                break
            targets.append(v)
        if bound is not None and len(targets) != bound:
            # partial table: still usable but flag it
            pass
        return targets if targets else None

    def resolve_const(self, reg, a, fstart, depth, skip_lwzx=False):
        """Value of `reg` before address `a` (inclusive), following lis/addi/ori chains."""
        if depth > 6:
            return None
        while a >= fstart:
            ins = decode(self.words[a])
            p = ins.w >> 26
            writes = None
            if p in (14, 15) and ins.rd == reg:
                if ins.ra == 0:
                    return (ins.simm << 16 if p == 15 else ins.simm) & 0xFFFFFFFF
                base = self.resolve_const(ins.ra, a - 4, fstart, depth + 1)
                if base is None:
                    return None
                return (base + (ins.simm << 16 if p == 15 else ins.simm)) & 0xFFFFFFFF
            if p == 24 and ins.ra == reg:   # ori rA, rS, imm
                base = self.resolve_const(ins.rs, a - 4, fstart, depth + 1)
                return None if base is None else (base | ins.uimm)
            if p == 31 and ins.op == 'or' and ins.ra == reg and ins.rs == ins.rb:   # mr
                return self.resolve_const(ins.rs, a - 4, fstart, depth + 1)
            # any other instruction writing reg ends the search
            if ins.op == 'lwzx' and skip_lwzx:
                pass
            elif p in (32, 33, 34, 35, 40, 41, 42, 43, 7, 8, 12, 13, 20, 21, 23) and ins.rd == reg and p not in (20, 21, 23):
                return None
            elif p in (20, 21, 23, 24, 25, 26, 27, 28, 29) and ins.ra == reg:
                return None
            elif p == 31 and ins.op not in ('stw', 'stwx', 'cmp', 'cmpl') and (ins.rd == reg if ins.op in ('lwzx', 'add', 'subf', 'mullw', 'lbzx', 'lhzx') else ins.ra == reg) and not (ins.op == 'lwzx' and skip_lwzx):
                return None
            a -= 4
        return None

    def run(self):
        funcs = []
        for sec in self.dol.text:
            a = sec.addr
            end = sec.end
            while a < end:
                # skip zero padding
                while a < end and self.words[a] == 0:
                    a += 4
                if a >= end:
                    break
                f = Function(a)
                self.starts.add(a)
                a = self.sweep(f, end)
                funcs.append(f)
        return funcs

    def next_start_after(self, a, sorted_starts):
        i = bisect.bisect_right(sorted_starts, a)
        return sorted_starts[i] if i < len(sorted_starts) else None

    def sweep(self, f, sec_end):
        if getattr(self, '_sorted_n', -1) != len(self.starts):
            self._sorted = sorted(self.starts)
            self._sorted_n = len(self.starts)
        sorted_starts = self._sorted
        nxt = self.next_start_after(f.start, sorted_starts)
        limit = min(sec_end, nxt) if nxt else sec_end
        pending = 0
        a = f.start
        while a < limit:
            w = self.words[a]
            ins = decode(w)
            term = False
            if ins.op == 'b' and not ins.lk:
                t = branch_target(a, ins)
                if f.start <= t < limit and t > a:
                    pending = max(pending, t)
                term = True
            elif ins.op == 'bc' and not ins.lk:
                t = branch_target(a, ins)
                if f.start <= t < limit and t > a:
                    pending = max(pending, t)
                if is_uncond_bc(ins):
                    term = True
            elif ins.op == 'bclr' and not ins.lk and is_uncond_bc(ins):
                term = True
            elif ins.op == 'bcctr' and not ins.lk and is_uncond_bc(ins):
                jt = self.find_jumptable(a, f.start)
                if jt:
                    f.jumptables[a] = jt
                    for t in jt:
                        if t > a and t < limit:
                            pending = max(pending, t)
                term = True
            elif ins.op == 'rfi':
                term = True
            elif w == 0:
                # zero word inside code: treat as end (padding)
                break
            a += 4
            if term and a > pending:
                break
        f.end = a
        return a


def load_known(sel_path):
    from sel import parse_sel
    _, syms = parse_sel(sel_path)
    bases = {1: 0x80004000, 2: 0x80026c00}
    out = {}
    for name, value, sec in syms:
        if sec in bases:
            out[bases[sec] + value] = name
    return out


def cross_targets(an, funcs):
    """Branch targets that land inside a different function (not at its start)."""
    starts = [f.start for f in funcs]
    new = set()
    for f in funcs:
        for a in range(f.start, f.end, 4):
            ins = decode(an.words[a])
            ts = []
            if ins.op in ('b', 'bc'):
                t = branch_target(a, ins)
                if ins.op == 'b' and ins.lk and t == a + 4:
                    continue
                ts.append(t)
            ts.extend(f.jumptables.get(a, ()))
            for t in ts:
                if t in an.words and not (f.start <= t < f.end):
                    i = bisect.bisect_right(starts, t) - 1
                    if i >= 0 and funcs[i].start != t:
                        new.add(t)
    return new


TERMINATORS = (0x4E800020, 0x4E800420, 0x4C000064)


def data_code_pointers(an, jt_targets):
    """Words in data sections pointing at plausible function entries (vtables, callbacks)."""
    out = set()
    for sec in an.dol.data:
        n = sec.size // 4
        for i in range(n):
            v = struct.unpack_from('>I', sec.data, i * 4)[0]
            if v & 3 or v not in an.words or v in jt_targets:
                continue
            prev = an.words.get(v - 4)
            if prev is None:
                continue
            if prev in TERMINATORS or prev == 0 or (prev >> 26) == 18 and not (prev & 1):
                out.add(v)
    return out


if __name__ == '__main__':
    dol = Dol(sys.argv[1])
    known = load_known(sys.argv[2]) if len(sys.argv) > 2 else {}
    an = Analyzer(dol, known.keys())
    funcs = an.run()
    jt_targets = {t for f in funcs for ts in f.jumptables.values() for t in ts}
    dp = data_code_pointers(an, jt_targets) - an.starts
    print(f'{len(dp)} new function starts from data pointers', file=sys.stderr)
    an.starts |= dp
    while True:
        funcs = an.run()
        extra = cross_targets(an, funcs) - an.starts
        print(f'pass: {len(funcs)} functions, {len(extra)} new split points', file=sys.stderr)
        if not extra:
            break
        an.starts |= extra
    for f in funcs:
        if f.start in known:
            f.name = known[f.start]
    print(f'{len(funcs)} functions, {sum(len(f.jumptables) for f in funcs)} jump tables', file=sys.stderr)
    out = sys.argv[3] if len(sys.argv) > 3 else 'functions.json'
    json.dump([f.to_json() for f in funcs], open(out, 'w'))
