"""PowerPC 750CL (Gekko/Broadway) instruction decoder.

decode(word) returns an Insn with a mnemonic and the raw field accessors.
Mnemonics are the canonical (non-simplified) forms; the '.' (Rc) and 'o' (OE)
suffixes are reported as flags rather than baked into the name.
"""


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


class Insn:
    __slots__ = ('w', 'op', 'rc', 'oe')

    def __init__(self, w, op, rc=False, oe=False):
        self.w = w
        self.op = op
        self.rc = rc
        self.oe = oe

    # register fields
    @property
    def rd(self): return (self.w >> 21) & 31
    rs = rd
    frd = rd
    frs = rd
    crfd = property(lambda s: (s.w >> 23) & 7)
    crbd = rd

    @property
    def ra(self): return (self.w >> 16) & 31
    fra = ra
    crfs = property(lambda s: (s.w >> 18) & 7)
    crba = ra

    @property
    def rb(self): return (self.w >> 11) & 31
    frb = rb
    crbb = rb
    sh = rb

    @property
    def rc_(self): return (self.w >> 6) & 31
    frc = rc_
    mb = rc_

    @property
    def me(self): return (self.w >> 1) & 31

    @property
    def simm(self): return sext(self.w & 0xFFFF, 16)

    @property
    def uimm(self): return self.w & 0xFFFF

    # branch fields
    @property
    def bo(self): return (self.w >> 21) & 31

    @property
    def bi(self): return (self.w >> 16) & 31

    @property
    def bd(self): return sext(self.w & 0xFFFC, 16)

    @property
    def li(self): return sext(self.w & 0x03FFFFFC, 26)

    @property
    def aa(self): return (self.w >> 1) & 1

    @property
    def lk(self): return self.w & 1

    @property
    def spr(self):
        s = (self.w >> 11) & 0x3FF
        return ((s & 0x1F) << 5) | (s >> 5)

    tbr = spr

    @property
    def crm(self): return (self.w >> 12) & 0xFF

    @property
    def fm(self): return (self.w >> 17) & 0xFF

    @property
    def sr(self): return (self.w >> 16) & 15

    @property
    def nb(self): return (self.w >> 11) & 31

    @property
    def to(self): return (self.w >> 21) & 31

    # paired single quantized load/store
    @property
    def ps_d(self): return sext(self.w & 0xFFF, 12)

    @property
    def ps_w(self): return (self.w >> 15) & 1

    @property
    def ps_i(self): return (self.w >> 12) & 7

    @property
    def psx_w(self): return (self.w >> 10) & 1

    @property
    def psx_i(self): return (self.w >> 7) & 7

    def __repr__(self):
        return f'<{self.op}{"o" if self.oe else ""}{"." if self.rc else ""} {self.w:08x}>'


PRIMARY = {
    3: 'twi', 7: 'mulli', 8: 'subfic', 10: 'cmpli', 11: 'cmpi', 12: 'addic',
    13: 'addic.', 14: 'addi', 15: 'addis', 16: 'bc', 17: 'sc', 18: 'b',
    20: 'rlwimi', 21: 'rlwinm', 23: 'rlwnm', 24: 'ori', 25: 'oris',
    26: 'xori', 27: 'xoris', 28: 'andi.', 29: 'andis.',
    32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu',
    38: 'stb', 39: 'stbu', 40: 'lhz', 41: 'lhzu', 42: 'lha', 43: 'lhau',
    44: 'sth', 45: 'sthu', 46: 'lmw', 47: 'stmw', 48: 'lfs', 49: 'lfsu',
    50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu',
    56: 'psq_l', 57: 'psq_lu', 60: 'psq_st', 61: 'psq_stu',
}

OP19 = {
    0: 'mcrf', 16: 'bclr', 33: 'crnor', 50: 'rfi', 129: 'crandc', 150: 'isync',
    193: 'crxor', 225: 'crnand', 257: 'crand', 289: 'creqv', 417: 'crorc',
    449: 'cror', 528: 'bcctr',
}

# XO-form (9-bit xo + OE)
OP31_XO = {
    8: 'subfc', 10: 'addc', 11: 'mulhwu', 40: 'subf', 75: 'mulhw', 104: 'neg',
    136: 'subfe', 138: 'adde', 200: 'subfze', 202: 'addze', 232: 'subfme',
    234: 'addme', 235: 'mullw', 266: 'add', 459: 'divwu', 491: 'divw',
}

OP31 = {
    0: 'cmp', 4: 'tw', 19: 'mfcr', 20: 'lwarx', 23: 'lwzx', 24: 'slw',
    26: 'cntlzw', 28: 'and', 32: 'cmpl', 54: 'dcbst', 55: 'lwzux', 60: 'andc',
    83: 'mfmsr', 86: 'dcbf', 87: 'lbzx', 119: 'lbzux', 124: 'nor',
    144: 'mtcrf', 146: 'mtmsr', 150: 'stwcx.', 151: 'stwx', 183: 'stwux',
    210: 'mtsr', 215: 'stbx', 242: 'mtsrin', 246: 'dcbtst', 247: 'stbux',
    278: 'dcbt', 279: 'lhzx', 284: 'eqv', 306: 'tlbie', 310: 'eciwx',
    311: 'lhzux', 316: 'xor', 339: 'mfspr', 343: 'lhax', 370: 'tlbia',
    371: 'mftb', 375: 'lhaux', 407: 'sthx', 412: 'orc', 438: 'ecowx',
    439: 'sthux', 444: 'or', 467: 'mtspr', 470: 'dcbi', 476: 'nand',
    512: 'mcrxr', 533: 'lswx', 534: 'lwbrx', 535: 'lfsx', 536: 'srw',
    566: 'tlbsync', 567: 'lfsux', 595: 'mfsr', 597: 'lswi', 598: 'sync',
    599: 'lfdx', 631: 'lfdux', 659: 'mfsrin', 661: 'stswx', 662: 'stwbrx',
    663: 'stfsx', 695: 'stfsux', 725: 'stswi', 727: 'stfdx', 759: 'stfdux',
    790: 'lhbrx', 792: 'sraw', 824: 'srawi', 854: 'eieio', 918: 'sthbrx',
    922: 'extsh', 954: 'extsb', 982: 'icbi', 983: 'stfiwx', 1014: 'dcbz',
}

OP59 = {
    18: 'fdivs', 20: 'fsubs', 21: 'fadds', 22: 'fsqrts', 24: 'fres',
    25: 'fmuls', 28: 'fmsubs', 29: 'fmadds', 30: 'fnmsubs', 31: 'fnmadds',
}

OP63_A = {
    18: 'fdiv', 20: 'fsub', 21: 'fadd', 22: 'fsqrt', 23: 'fsel', 25: 'fmul',
    26: 'frsqrte', 28: 'fmsub', 29: 'fmadd', 30: 'fnmsub', 31: 'fnmadd',
}

OP63_X = {
    0: 'fcmpu', 12: 'frsp', 14: 'fctiw', 15: 'fctiwz', 32: 'fcmpo',
    38: 'mtfsb1', 40: 'fneg', 64: 'mcrfs', 70: 'mtfsb0', 72: 'fmr',
    134: 'mtfsfi', 136: 'fnabs', 264: 'fabs', 583: 'mffs', 711: 'mtfsf',
}

OP4_X = {
    0: 'ps_cmpu0', 32: 'ps_cmpo0', 40: 'ps_neg', 64: 'ps_cmpu1', 72: 'ps_mr',
    96: 'ps_cmpo1', 136: 'ps_nabs', 264: 'ps_abs', 528: 'ps_merge00',
    560: 'ps_merge01', 592: 'ps_merge10', 624: 'ps_merge11', 1014: 'dcbz_l',
}

OP4_6 = {6: 'psq_lx', 38: 'psq_lux', 7: 'psq_stx', 39: 'psq_stux'}

OP4_A = {
    10: 'ps_sum0', 11: 'ps_sum1', 12: 'ps_muls0', 13: 'ps_muls1',
    14: 'ps_madds0', 15: 'ps_madds1', 18: 'ps_div', 20: 'ps_sub', 21: 'ps_add',
    23: 'ps_sel', 24: 'ps_res', 25: 'ps_mul', 26: 'ps_rsqrte', 28: 'ps_msub',
    29: 'ps_madd', 30: 'ps_nmsub', 31: 'ps_nmadd',
}


def decode(w):
    p = w >> 26
    rc = bool(w & 1)
    if p in PRIMARY:
        name = PRIMARY[p]
        # addic. / andi. / andis. always record; rotate/mask ops carry an Rc bit
        if p in (20, 21, 23):
            return Insn(w, name, rc=rc)
        return Insn(w, name, rc=name.endswith('.'))
    if p == 19:
        xo = (w >> 1) & 0x3FF
        n = OP19.get(xo)
        return Insn(w, n or 'illegal')
    if p == 31:
        xo10 = (w >> 1) & 0x3FF
        xo9 = (w >> 1) & 0x1FF
        if xo9 in OP31_XO and xo10 not in OP31:
            return Insn(w, OP31_XO[xo9], rc=rc, oe=bool(w & 0x400))
        n = OP31.get(xo10)
        if n is None:
            return Insn(w, 'illegal')
        if n == 'stwcx.':
            return Insn(w, n, rc=True)
        return Insn(w, n, rc=rc)
    if p == 59:
        n = OP59.get((w >> 1) & 31)
        return Insn(w, n or 'illegal', rc=rc)
    if p == 63:
        xo10 = (w >> 1) & 0x3FF
        if xo10 in OP63_X:
            return Insn(w, OP63_X[xo10], rc=rc)
        n = OP63_A.get((w >> 1) & 31)
        return Insn(w, n or 'illegal', rc=rc)
    if p == 4:
        xo10 = (w >> 1) & 0x3FF
        if xo10 in OP4_X:
            return Insn(w, OP4_X[xo10], rc=rc)
        xo6 = (w >> 1) & 0x3F
        if xo6 in OP4_6:
            return Insn(w, OP4_6[xo6])
        n = OP4_A.get((w >> 1) & 31)
        return Insn(w, n or 'illegal', rc=rc)
    return Insn(w, 'illegal')


SPR_NAMES = {
    1: 'xer', 8: 'lr', 9: 'ctr', 18: 'dsisr', 19: 'dar', 22: 'dec', 25: 'sdr1',
    26: 'srr0', 27: 'srr1', 268: 'tbl', 269: 'tbu', 272: 'sprg0', 273: 'sprg1',
    274: 'sprg2', 275: 'sprg3', 282: 'ear', 284: 'tbl_w', 285: 'tbu_w',
    287: 'pvr', 912: 'gqr0', 913: 'gqr1', 914: 'gqr2', 915: 'gqr3',
    916: 'gqr4', 917: 'gqr5', 918: 'gqr6', 919: 'gqr7', 920: 'hid2',
    921: 'wpar', 922: 'dma_u', 923: 'dma_l', 1008: 'hid0', 1009: 'hid1',
    1017: 'l2cr', 1019: 'ictc', 1020: 'thrm1', 1021: 'thrm2', 1022: 'thrm3',
}


def disasm(addr, w):
    """Very small disassembler used for comments/debugging."""
    i = decode(w)
    o = i.op + ('o' if i.oe else '') + ('.' if i.rc and not i.op.endswith('.') else '')
    if i.op in ('b',):
        t = (i.li if i.aa else addr + i.li) & 0xFFFFFFFF
        return f'{"bl" if i.lk else "b"}{"a" if i.aa else ""} 0x{t:08x}'
    if i.op == 'bc':
        t = (i.bd if i.aa else addr + i.bd) & 0xFFFFFFFF
        return f'bc{"l" if i.lk else ""} {i.bo},{i.bi},0x{t:08x}'
    if i.op in ('bclr', 'bcctr'):
        return f'{i.op}{"l" if i.lk else ""} {i.bo},{i.bi}'
    if i.op in ('addi', 'addis', 'addic', 'addic.', 'subfic', 'mulli'):
        return f'{o} r{i.rd},r{i.ra},{i.simm}'
    if i.op in ('ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.'):
        return f'{o} r{i.ra},r{i.rs},0x{i.uimm:x}'
    if i.op in ('cmpi',):
        return f'cmpwi cr{i.crfd},r{i.ra},{i.simm}'
    if i.op in ('cmpli',):
        return f'cmplwi cr{i.crfd},r{i.ra},0x{i.uimm:x}'
    if i.op in ('cmp', 'cmpl'):
        return f'{i.op}w cr{i.crfd},r{i.ra},r{i.rb}'
    if i.op in ('rlwinm', 'rlwimi'):
        return f'{o} r{i.ra},r{i.rs},{i.sh},{i.mb},{i.me}'
    if i.op == 'rlwnm':
        return f'{o} r{i.ra},r{i.rs},r{i.rb},{i.mb},{i.me}'
    p = w >> 26
    if 32 <= p <= 47:
        return f'{o} r{i.rd},{i.simm}(r{i.ra})'
    if 48 <= p <= 55:
        return f'{o} f{i.rd},{i.simm}(r{i.ra})'
    if p in (56, 57, 60, 61):
        return f'{o} f{i.rd},{i.ps_d}(r{i.ra}),{i.ps_w},qr{i.ps_i}'
    if i.op in ('mfspr', 'mtspr'):
        n = SPR_NAMES.get(i.spr, str(i.spr))
        return f'{i.op} {n},r{i.rd}'
    if p == 31:
        return f'{o} r{i.rd},r{i.ra},r{i.rb}'
    if p in (59, 63, 4):
        return f'{o} f{i.rd},f{i.ra},f{i.rb},f{i.rc_}'
    return o
