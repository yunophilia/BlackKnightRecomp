"""Regenerate the recompiled C++ sources.

  python tools/build_generated.py [--reanalyze]

Steps: function analysis (cached in out/functions.json), signature matching,
override scan of runtime/ (functions the runtime replaces), code generation into
generated/ (files are only rewritten when their content changes).
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOL = os.path.join(ROOT, 'game', 'extracted', 'DATA', 'sys', 'main.dol')
SEL = os.path.join(ROOT, 'game', 'extracted', 'DATA', 'files', 'BLACKKNIGHT.sel')
DSY = r'C:\dolphin_emu\Dolphin-x64\Sys\totaldb.dsy'
FUNCS = os.path.join(ROOT, 'out', 'functions.json')
OVR = os.path.join(ROOT, 'out', 'overrides.txt')
GEN = os.path.join(ROOT, 'generated')
TOOLS = os.path.join(ROOT, 'tools')


def run(*args):
    print('>', ' '.join(args), flush=True)
    subprocess.check_call([sys.executable, *args], cwd=ROOT)


def scan_overrides():
    pat = re.compile(r'PPC_FUNC\(\s*fn_([0-9A-Fa-f]{8})\s*\)')
    found = set()
    for d, _, files in os.walk(os.path.join(ROOT, 'runtime')):
        for f in files:
            if f.endswith(('.cpp', '.h')):
                txt = open(os.path.join(d, f), encoding='utf-8', errors='ignore').read()
                found.update(int(m, 16) for m in pat.findall(txt))
    return sorted(found)


def main():
    os.makedirs(os.path.join(ROOT, 'out'), exist_ok=True)
    if '--reanalyze' in sys.argv or not os.path.exists(FUNCS):
        run(os.path.join(TOOLS, 'analyze.py'), DOL, SEL, FUNCS)
        if os.path.exists(DSY):
            run(os.path.join(TOOLS, 'sigdb.py'), DOL, FUNCS, DSY)
    ovr = scan_overrides()
    with open(OVR, 'w') as fp:
        fp.write('\n'.join(f'{a:08X}' for a in ovr))
    print(f'{len(ovr)} runtime overrides')
    run(os.path.join(TOOLS, 'recomp.py'), DOL, FUNCS, GEN, OVR)


if __name__ == '__main__':
    main()
