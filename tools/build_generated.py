"""Regenerate the recompiled C++ sources.

  python tools/build_generated.py [--reanalyze] [--dsy PATH]

Steps: function analysis (cached in out/functions.json), signature matching,
override scan of runtime/ (functions the runtime replaces), code generation into
generated/ (files are only rewritten when their content changes).

Signature matching is optional: it only names SDK functions in out/functions.json
for tools/xref.py; the generated code is the same without it. It uses Dolphin's
signature database (Data/Sys/totaldb.dsy in Dolphin's repo, Sys/totaldb.dsy in a
Dolphin build), found via --dsy, the BKR_TOTALDB environment variable, game/totaldb.dsy
or a Dolphin install under Program Files.
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


def find_dsy():
    if '--dsy' in sys.argv:
        return sys.argv[sys.argv.index('--dsy') + 1]
    cands = [os.environ.get('BKR_TOTALDB'), os.path.join(ROOT, 'game', 'totaldb.dsy')]
    for pf in (os.environ.get('ProgramFiles'), os.environ.get('ProgramFiles(x86)')):
        if pf:
            cands.append(os.path.join(pf, 'Dolphin', 'Sys', 'totaldb.dsy'))
    return next((c for c in cands if c and os.path.exists(c)), None)


def main():
    os.makedirs(os.path.join(ROOT, 'out'), exist_ok=True)
    if '--reanalyze' in sys.argv or not os.path.exists(FUNCS):
        run(os.path.join(TOOLS, 'analyze.py'), DOL, SEL, FUNCS)
        dsy = find_dsy()
        if dsy:
            run(os.path.join(TOOLS, 'sigdb.py'), DOL, FUNCS, dsy)
        else:
            print('totaldb.dsy not found; skipping signature matching (optional)')
    ovr = scan_overrides()
    with open(OVR, 'w') as fp:
        fp.write('\n'.join(f'{a:08X}' for a in ovr))
    print(f'{len(ovr)} runtime overrides')
    run(os.path.join(TOOLS, 'recomp.py'), DOL, FUNCS, GEN, OVR)


if __name__ == '__main__':
    main()
