"""Generates one TAS script per mission for the 30 vs 60 fps comparison (docs/tas.md).

  python tools/tas/gen_missions.py

Navigation data (tools/tas/nav_*.json) was recorded from the 99% PAL test save
(out/tasnand/full): world-map locations are picked with the pointer, missions with
D-pad paths from each location's initially highlighted icon.
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
LOCS = json.load(open(os.path.join(HERE, 'nav_locations.json')))
NAV = json.load(open(os.path.join(HERE, 'nav_missions.json')))

# name -> (location, icon, kind)
MISSIONS = {
    'crystal_goal': ('crystal', 0, 'run'), 'crystal_legacy': ('crystal', 1, 'run'), 'crystal_gate': ('crystal', 4, 'run'),
    'megalith_clock': ('megalith', 0, 'run'), 'megalith_columns': ('megalith', 2, 'fight'),
    'camelot_goal': ('camelot', 4, 'run'), 'camelot_surge_ground': ('camelot', 5, 'run'), 'camelot_surge_air': ('camelot', 7, 'run'),
    'deepwoods_goal': ('deepwoods', 2, 'run'), 'boss_lancelot': ('deepwoods', 4, 'boss'), 'boss_lancelot2': ('deepwoods', 7, 'boss'),
    'cauldron_clock': ('cauldron', 0, 'run'), 'cauldron_escape': ('cauldron', 1, 'run'),
    'boss_arthur': ('faraway', 0, 'boss'), 'boss_darkqueen': ('darkhollow', 0, 'boss'),
    'titanic_perfecthit': ('titanic', 0, 'fight'), 'boss_gawain': ('titanic', 3, 'boss'), 'titanic_goal': ('titanic', 7, 'run'),
    'dragons_clock': ('dragons', 0, 'run'), 'boss_dragonslayer': ('dragons', 6, 'boss'),
    'molten_goal': ('molten', 0, 'run'), 'boss_percival': ('molten', 1, 'boss'), 'molten_grind': ('molten', 8, 'run'),
    'shrouded_clock': ('shrouded', 0, 'run'), 'shrouded_noground': ('shrouded', 3, 'run'),
    'knights_clock': ('knights', 0, 'run'), 'knights_wisps': ('knights', 4, 'fight'),
}

BOOT = '''include ../watches.tas
watch px f32 [8072bfbc]+380
watch py f32 [8072bfbc]+384
watch pz f32 [8072bfbc]+388
log boot
wait 40
hold 0.3 A+B
wait 5.7
hold 0.3 A+B
wait 5.7
hold 0.3 A
wait 7.7
hold 0.3 START
wait 3.7
hold 0.3 START
wait 3.7
hold 0.3 A
wait 3
'''

# Generic play: run forward, jump and slash; A also dismisses dialogue. Bosses add
# guarding and right-stick slashes, fights stay mostly in place.
PLAY = {
    'run': 'set LY=1.0\nrepeat 40\ntap X 0.1\nwait 0.4\ntap A 0.15\nwait 0.6\ntap X 0.1\nwait 0.5\nend\nset\n',
    'fight': 'repeat 40\nset LY=0.6\ntap X 0.1\nwait 0.3\ntap X 0.1\nwait 0.3\nset LX=0.6\ntap A 0.15\nwait 0.5\ntap X 0.1\nwait 0.4\nend\nset\n',
    'boss': 'repeat 40\nset LY=0.5\ntap X 0.1\nwait 0.3\nhold 0.3 RB\nset LX=-0.7\ntap A 0.15\nwait 0.3\nhold 0.2 RX=1.0\ntap X+A 0.1\nwait 0.4\nend\nset\n',
}

def path(edges, a, b):
    import collections
    prev = {a: None}; q = collections.deque([a])
    while q:
        u = q.popleft()
        for d, v in edges.get(str(u), {}).items():
            if v not in prev: prev[v] = (u, d); q.append(v)
    out = []; u = b
    while prev[u]: u, d = prev[u]; out.append(d)
    return out[::-1]

os.makedirs(os.path.join(HERE, 'missions'), exist_ok=True)
for name, (loc, icon, kind) in MISSIONS.items():
    x, y = LOCS[loc].split()
    p = f'PX={x}+PY={y}'
    s = BOOT + f'hold 1 {p}\nfilemark\ntap A+{p} 0.1\nwait 2.5\n'
    for d in path(NAV[loc]['edges'], NAV[loc]['start'], icon): s += f'tap {d} 0.12\nwait 0.55\n'
    # mission, then "Are you ready?" YES (boss missions start without the question; the
    # second A then only skips the intro)
    s += f'wait 0.3\nshot {name}_select\ntap A 0.2\nwait 1.5\ntap A 0.2\n'
    s += 'until file files_stg timeout 40\nwait 8\nlog play\ncalls -\nshot {0}_start\n'.format(name)
    s += PLAY[kind]
    s += f'calls {name}\nshot {name}_end\nwait 0.5\nlog end\nquit\n'
    open(os.path.join(HERE, 'missions', name + '.tas'), 'w').write(s)

# Battle mode: two scripted players (player 2 joins by pressing A, then points at Sonic),
# rule "Battle", one script per arena (picked with player 1's pointer).
ARENAS = {'misty': (0.26, 0.30), 'castle': (0.45, 0.30), 'forest': (0.64, 0.30), 'night': (0.83, 0.30),
          'crypt': (0.26, 0.56), 'cauldron': (0.45, 0.56), 'plain': (0.64, 0.56)}
BATTLE_PLAY = 'set LY=1.0 P2:LY=1.0\nrepeat 40\ntap X P2:X 0.1\nwait 0.3\nset LX=0.7 P2:LX=-0.7\ntap A P2:A 0.15\nwait 0.5\nset LY=1.0 P2:LY=1.0\ntap X P2:X 0.1\nwait 0.6\nend\nset\n'
for arena, (ax, ay) in ARENAS.items():
    name = f'battle_{arena}'
    # BOOT ends by confirming Adventure on the main menu; stop before that and pick Battle
    s_ = 'players 2\n' + BOOT
    s_ = s_[:s_.rindex('hold 0.3 A\nwait 3\n')]
    s_ += 'tap DOWN 0.12\nwait 1\ntap A 0.15\nwait 3\ntap A 0.15\nwait 3\n'          # Battle, rule "Battle"
    # P1 joins, P2 joins (its first press reconnects it) and picks Sonic; P1 confirms last,
    # since a player joining resets the others' ready state
    s_ += 'tap A 0.15\nwait 2\ntap P2:A 0.15\nwait 2\ntap P2:A 0.15\nwait 2\n'
    s_ += 'hold 1 P2:PX=0.40+PY=0.26\ntap P2:A+PX=0.40+PY=0.26 0.15\nwait 1.5\ntap P2:A 0.15\nwait 1.5\n'
    s_ += 'tap A 0.15\nwait 2\n'
    s_ += 'tap START 0.15\nwait 2\ntap A 0.15\nwait 3\n'                              # on-screen START
    # the arena under the pointer is picked with A and confirmed with a second A
    s_ += f'filemark\nhold 1 PX={ax}+PY={ay}\ntap A+PX={ax}+PY={ay} 0.15\nwait 1.5\nshot {name}_select\ntap A 0.15\n'
    s_ += 'until file sng_stg timeout 40\nwait 6\nlog play\ncalls -\n' + f'shot {name}_start\n'
    s_ += BATTLE_PLAY + f'calls {name}\nshot {name}_end\nwait 0.5\nlog end\nquit\n'
    open(os.path.join(HERE, 'missions', name + '.tas'), 'w').write(s_)
print(len(MISSIONS) + len(ARENAS), 'scripts in tools/tas/missions')
