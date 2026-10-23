"""Regenerate the recompiled C++ sources of every release whose inputs are here.

  python tools/build_generated.py [--reanalyze] [--dsy PATH] [--require-all]

The releases and where their inputs go are listed in tools/regions.py (game/<id>/ or
game/extracted/DATA, out/<id>/mem2_dump.bin). For each one: function analysis (cached in
out/<id>/functions.json), the HOME menu module (tools/rso.py, when there is a MEM2 dump),
the override scan of runtime/ (PPC_OVERRIDE), code generation into generated/<ns>/
(files are only rewritten when their content changes). With every release here it also
refreshes runtime/game_addrs.inc (tools/game_addrs.py). The exe contains the code of the
releases generated here; --require-all stops unless that is all of them (releases).

SDK function names only matter for tools/xref.py and reports; the generated code is
the same without them. They come from tools/sdk_names.txt (recorded from a match
against Dolphin's signature database), or, to rematch, from a totaldb.dsy given with
--dsy or the BKR_TOTALDB environment variable; the other releases get the USA names of
the matching functions. Dolphin is not needed.
"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from game_addrs import load_inc                  # noqa: E402
from regions import REFERENCE, REGIONS, ROOT     # noqa: E402

GEN = os.path.join(ROOT, 'generated')
TOOLS = os.path.join(ROOT, 'tools')


def run(*args):
    print('>', ' '.join(os.path.relpath(a, ROOT) if os.path.isabs(a) else a for a in args), flush=True)
    subprocess.check_call([sys.executable, *args], cwd=ROOT)


def write_if_changed(path, text):
    if os.path.exists(path) and open(path, newline='\n').read() == text:
        return
    with open(path, 'w', newline='\n') as fp:
        fp.write(text)


def scan_overrides():
    pat = re.compile(r'PPC_OVERRIDE\(\s*(\w+)\s*\)')
    found = set()
    for d, _, files in os.walk(os.path.join(ROOT, 'runtime')):
        for f in files:
            if f.endswith(('.cpp', '.h')) and f != 'game_addrs.h':
                txt = open(os.path.join(d, f), encoding='utf-8', errors='ignore').read()
                found.update(pat.findall(txt))
    return sorted(found)


def find_dsy():
    if '--dsy' in sys.argv:
        return sys.argv[sys.argv.index('--dsy') + 1]
    dsy = os.environ.get('BKR_TOTALDB')
    return dsy if dsy and os.path.exists(dsy) else None


def translate_names(region):
    """SDK names for another release: the reference release's names of the same functions."""
    import region_map
    ref = {f['start']: f.get('sig') for f in json.load(open(REFERENCE.out('functions.json')))}
    m = region_map.load(region)
    names = {m.func_pairs[a]: n for a, n in ref.items() if n and a in m.func_pairs}
    path = region.out('functions.json')
    funcs = json.load(open(path))
    for f in funcs:
        if f['start'] in names:
            f['sig'] = names[f['start']]
    json.dump(funcs, open(path, 'w'))
    print(f'{region.game_id}: {len(names)} SDK names from {REFERENCE.game_id}')


def main():
    present = [r for r in REGIONS if r.data_dir()]
    missing = [r for r in REGIONS if r not in present]
    for r in missing:
        print(f'{r.game_id} ({r.name}): no game data in game/{r.game_id} or game/extracted/DATA, skipped')
    if not present:
        sys.exit('no game data: install a disc first (see README)')
    if missing and '--require-all' in sys.argv:
        sys.exit('--require-all: ' + ', '.join(r.game_id for r in missing) + ' missing')

    reanalyze = '--reanalyze' in sys.argv
    analyzed = []
    for r in present:
        os.makedirs(r.out(), exist_ok=True)
        if reanalyze or not os.path.exists(r.out('functions.json')):
            run(os.path.join(TOOLS, 'analyze.py'), r.dol(), r.sel(), r.out('functions.json'))
            analyzed.append(r)
        dump = r.out('mem2_dump.bin')
        info = r.out('rso_info.json')
        if os.path.exists(dump) and (not os.path.exists(info) or os.path.getmtime(info) < os.path.getmtime(dump)):
            run(os.path.join(TOOLS, 'rso.py'), r.game_id)
    if REFERENCE in analyzed:
        # SDK names: rematch with a given totaldb.dsy, else the recorded names
        run(os.path.join(TOOLS, 'sigdb.py'), REFERENCE.dol(), REFERENCE.out('functions.json'),
            find_dsy() or os.path.join(TOOLS, 'sdk_names.txt'))
    if REFERENCE in present:
        for r in present:
            if r is not REFERENCE and (r in analyzed or REFERENCE in analyzed):
                translate_names(r)
    if not missing:
        run(os.path.join(TOOLS, 'game_addrs.py'))

    ovr = scan_overrides()
    known = {name for name, kind, _ in load_inc() if kind == 'func'}
    unknown = [n for n in ovr if n not in known]
    if unknown:
        sys.exit('PPC_OVERRIDE of names that are no func in tools/game_addrs.txt: ' + ', '.join(unknown))
    print(f'{len(ovr)} runtime overrides')
    procs = []
    for r in present:
        with open(r.out('overrides.txt'), 'w') as fp:
            fp.write('\n'.join(ovr))
        cmd = [sys.executable, os.path.join(TOOLS, 'recomp.py'), r.game_id]
        print('>', 'tools/recomp.py', r.game_id, flush=True)
        procs.append(subprocess.Popen(cmd, cwd=ROOT))
    if any(p.wait() for p in procs):
        sys.exit('recomp.py failed')

    # all releases' code, and the table the runtime picks the installed release's code from
    src = ['# Generated by tools/build_generated.py: the releases in this build\n']
    src += [f'include(${{CMAKE_CURRENT_LIST_DIR}}/{r.ns}/sources.cmake)\n' for r in present]
    src.append('set(PPC_RECOMP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/ppc_regions.cpp')
    src += [f' ${{PPC_RECOMP_SOURCES_{r.game_id}}}' for r in present]
    src.append(')\n')
    write_if_changed(os.path.join(GEN, 'sources.cmake'), ''.join(src))
    reg = ['// Generated by tools/build_generated.py: the releases in this build\n#include "ppc_context.h"\n\n']
    reg += [f'namespace {r.ns} {{ extern const PPCRegionCode Code; }}\n' for r in present]
    reg.append('\nextern const PPCRegionCode* const g_ppcRegionCode[] = {\n')
    reg += [f'\t&{r.ns}::Code,\n' for r in present]
    reg.append('\tnullptr\n};\n')
    write_if_changed(os.path.join(GEN, 'ppc_regions.cpp'), ''.join(reg))
    print('releases in this build: ' + ', '.join(f'{r.game_id} ({r.name})' for r in present))


if __name__ == '__main__':
    main()
