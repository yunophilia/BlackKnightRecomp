"""The game's releases the recompiler supports, and where their build inputs are.

Build inputs of a release (game ID G):
  game/G/sys/main.dol, game/G/files/BLACKKNIGHT.sel, game/G/files/hbm_part.rso
      an extracted disc (bkr_install or DolphinTool "extract -g" output); game/extracted/DATA
      is used instead when it holds that release
  out/G/mem2_dump.bin
      MEM2 after the HOME menu module is linked (BKR_DUMPMEM2=1, see tools/rso.py); optional
Per-release intermediate files go to out/G/, the generated code to generated/<ns>/.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Region:
    def __init__(self, game_id, name, ns):
        self.game_id = game_id   # disc game ID, e.g. RENE8P
        self.name = name         # USA, Europe, Japan
        self.ns = ns             # C++ namespace and generated/ subdirectory
        self.title_id = 0x00010000_00000000 | int.from_bytes(game_id[:4].encode(), 'big')

    def data_dir(self):
        """Extracted disc of this release, or None if it is not there."""
        own = os.path.join(ROOT, 'game', self.game_id)
        if os.path.exists(os.path.join(own, 'sys', 'main.dol')):
            return own
        ext = os.path.join(ROOT, 'game', 'extracted', 'DATA')
        boot = os.path.join(ext, 'sys', 'boot.bin')
        if os.path.exists(boot) and open(boot, 'rb').read(6).decode('ascii', 'replace') == self.game_id:
            return ext
        return None

    def dol(self):
        return os.path.join(self.data_dir(), 'sys', 'main.dol')

    def sel(self):
        return os.path.join(self.data_dir(), 'files', 'BLACKKNIGHT.sel')

    def rso(self):
        return os.path.join(self.data_dir(), 'files', 'hbm_part.rso')

    def out(self, *name):
        return os.path.join(ROOT, 'out', self.game_id, *name)

    def __repr__(self):
        return self.game_id


# USA first: it is the reference release (runtime addresses are written down for it)
REGIONS = [
    Region('RENE8P', 'USA', 'rene8p'),
    Region('RENP8P', 'Europe', 'renp8p'),
    Region('RENJ8P', 'Japan', 'renj8p'),
]
REFERENCE = REGIONS[0]


def by_id(game_id):
    for r in REGIONS:
        if r.game_id == game_id:
            return r
    raise KeyError(game_id)
