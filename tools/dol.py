"""DOL executable loader."""
import struct


class Section:
    def __init__(self, kind, index, offset, addr, data):
        self.kind = kind          # 'text' or 'data'
        self.index = index
        self.offset = offset
        self.addr = addr
        self.data = data
        self.size = len(data)

    @property
    def end(self):
        return self.addr + self.size

    def __repr__(self):
        return f'<{self.kind}{self.index} {self.addr:08x}-{self.end:08x}>'


class Dol:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.raw = d
        offs = struct.unpack_from('>18I', d, 0)
        addrs = struct.unpack_from('>18I', d, 72)
        sizes = struct.unpack_from('>18I', d, 144)
        self.bss_addr, self.bss_size, self.entry = struct.unpack_from('>3I', d, 216)
        self.sections = []
        for i in range(18):
            if sizes[i]:
                kind = 'text' if i < 7 else 'data'
                self.sections.append(Section(kind, i, offs[i], addrs[i], d[offs[i]:offs[i] + sizes[i]]))
        self.text = [s for s in self.sections if s.kind == 'text']
        self.data = [s for s in self.sections if s.kind == 'data']

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
        o = addr - s.addr
        return struct.unpack_from('>I', s.data, o)[0]

    def words(self, sec):
        n = sec.size // 4
        return struct.unpack(f'>{n}I', sec.data[:n * 4])
