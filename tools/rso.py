"""Prepare the HOME menu module (hbm_part.rso) for static recompilation.

The game links the module at boot with OSLink, so its relocated code sits at a fixed
MEM2 address. hbm_part.rso is the same file in every release, but where it is loaded
and the main.dol addresses it is linked against are not, so each release needs its own
MEM2 dump taken after linking: BKR_DUMPMEM2=1 writes mem2_dump.bin next to the exe on
the first call into MEM2 code; copy it to out/<game ID>/mem2_dump.bin. This finds the
module in it, runs the normal function analysis over its text and writes, to out/<id>/:

  rso_image.bin       relocated text section
  rso_info.json       {"text": addr, "size": n, "hash": fnv1a64, "data": [[addr, size], ...]}
  rso_functions.json  function list (same format as functions.json)

  python tools/rso.py GAMEID      (tools/build_generated.py runs it when there is a dump)
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Section          # noqa: E402
from analyze import Analyzer, cross_targets, data_code_pointers   # noqa: E402
from regions import by_id                                         # noqa: E402

DUMP_BASE = 0x90000000


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Image:
    """Minimal Dol-like view (what Analyzer needs) over the linked module."""

    def __init__(self, text, data):
        self.sections = [text] + data
        self.text = [text]
        self.data = data
        self.entry = text.addr

    def section_at(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.end:
                return s
        return None

    def is_text(self, addr):
        s = self.section_at(addr)
        return s is not None and s.kind == 'text'

    def read32(self, addr):
        s = self.section_at(addr)
        if s is None:
            return None
        return struct.unpack_from('>I', s.data, addr - s.addr)[0]

    def words(self, sec):
        n = sec.size // 4
        return struct.unpack(f'>{n}I', sec.data[:n * 4])


def main():
    region = by_id(sys.argv[1])
    rso = open(region.rso(), 'rb').read()
    mem = open(region.out('mem2_dump.bin'), 'rb').read()
    nsec, secoff = struct.unpack_from('>II', rso, 8)
    secs = [struct.unpack_from('>II', rso, secoff + 8 * i) for i in range(nsec)]
    text_off, text_size = secs[1]
    # locate the linked module: relocations only touch a few words, so match a run
    probe = rso[text_off + 0x2800:text_off + 0x2820]
    i = mem.find(probe)
    if i < 0:
        sys.exit('module text not found in the dump')
    text_addr = DUMP_BASE + i - 0x2800
    load_base = text_addr - text_off
    print(f'module loaded at {load_base:08x}, text {text_addr:08x}+{text_size:x}')
    img = mem[text_addr - DUMP_BASE:text_addr - DUMP_BASE + text_size]
    text = Section('text', 1, 0, text_addr, img)
    data = []
    for idx, (off, size) in enumerate(secs[2:], start=2):
        if off and size and load_base + off + size - DUMP_BASE <= len(mem):
            a = load_base + off
            data.append(Section('data', idx, 0, a, mem[a - DUMP_BASE:a - DUMP_BASE + size]))

    view = Image(text, data)
    an = Analyzer(view, [text_addr])
    funcs = an.run()
    jt = {t for f in funcs for ts in f.jumptables.values() for t in ts}
    an.starts |= data_code_pointers(an, jt) - an.starts
    while True:
        funcs = an.run()
        extra = cross_targets(an, funcs) - an.starts
        if not extra:
            break
        an.starts |= extra
    funcs = [f for f in funcs if text_addr <= f.start < text_addr + text_size]
    print(f'{len(funcs)} functions')
    open(region.out('rso_image.bin'), 'wb').write(img)
    json.dump([f.to_json() for f in funcs], open(region.out('rso_functions.json'), 'w'))
    json.dump({'text': text_addr, 'size': text_size, 'hash': fnv1a64(img),
               'data': [[s.addr, s.size] for s in data]},
              open(region.out('rso_info.json'), 'w'))


if __name__ == '__main__':
    main()
