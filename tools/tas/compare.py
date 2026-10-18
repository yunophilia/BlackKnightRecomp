"""Compares 30 and 60 fps TAS runs of the same scripts (docs/tas.md).

  python tools/tas/compare.py [mission ...]     (default: every tools/tas/missions script)

Reads out/tas/<name>_30|60.csv/.log and, from call-counting runs, out/dumps/<name>_30|60.calls.
Writes out/tas/report.md.
"""
import array, csv, glob, json, math, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TAS = os.path.join(ROOT, 'out', 'tas')
DUMPS = os.path.join(ROOT, 'out', 'dumps')
FUNCS = json.load(open(os.path.join(ROOT, 'out', 'functions.json')))


def load_csv(name):
    p = os.path.join(TAS, name + '.csv')
    if not os.path.exists(p): return None
    rows = list(csv.DictReader(open(p)))
    play = False; out = []
    for r in rows:
        if r['marker'] == 'play': play = True
        elif r['marker'] == 'end': play = False
        if play: out.append(r)
    return out


def num(r, k):
    try: return float(r[k])
    except (KeyError, ValueError, TypeError): return None


def timer(r):
    m, s, c = num(r, 't_min'), num(r, 't_sec'), num(r, 't_cs')
    return None if None in (m, s, c) else m * 60 + s + c / 60


def stats(rows):
    if not rows: return None
    st = {'secs': float(rows[-1]['t']) - float(rows[0]['t'])}
    # stage timer speed while it runs
    run_t = run_g = 0.0; prev = None
    for r in rows:
        t, g = float(r['t']), timer(r)
        if prev and g is not None and prev[1] is not None and 0 < g - prev[1] < 0.5:
            run_t += t - prev[0]; run_g += g - prev[1]
        prev = (t, g)
    st['timer_rate'] = run_g / run_t if run_t > 1 else None
    st['timer_running'] = run_t
    st['frames_per_s'] = len(rows) / st['secs'] if st['secs'] > 0 else 0
    dts = {r['dt'] for r in rows if r['dt']}
    st['dt'] = ','.join(sorted(dts))
    # player motion: speed while moving and jump apexes
    pts = [(float(r['t']), num(r, 'px'), num(r, 'py'), num(r, 'pz')) for r in rows]
    pts = [p for p in pts if None not in p and all(abs(v) < 1e6 for v in p[1:])]
    speeds = []
    for a, b in zip(pts, pts[1:]):
        dt = b[0] - a[0]
        if dt > 0:
            v = math.dist(a[1:], b[1:]) / dt
            if 1 < v < 3000: speeds.append(v)
    speeds.sort()
    st['speed_p50'] = speeds[len(speeds) // 2] if speeds else None
    st['speed_p90'] = speeds[int(len(speeds) * 0.9)] if speeds else None
    # jumps: rises of more than 5 units above the last ground height that land again
    # (sample jitter on the ground must not count, there are twice as many samples at 60 fps)
    jumps = 0; ground = None; air = False
    for p in pts:
        y = p[2]
        if ground is None: ground = y
        if not air:
            if y > ground + 5: air = True
            else: ground = min(ground, y) if y < ground else (ground * 0.9 + y * 0.1)
        elif y < ground + 1:
            jumps += 1; air = False; ground = y
    apex = range(jumps)
    st['jumps'] = len(apex)
    last = rows[-1]
    st['rings'] = num(last, 'rings'); st['score'] = num(last, 'score')
    st['timer_end'] = timer(last)
    return st


def log_info(name):
    p = os.path.join(TAS, name + '.log')
    if not os.path.exists(p): return {}
    txt = open(p, errors='replace').read()
    fps = [float(x) for x in re.findall(r'status: ([0-9.]+) fps', txt)]
    return {'errors': len(re.findall(r'^\[E\]', txt, re.M)), 'fails': txt.count('TAS: FAIL'),
            'fps_min': min(fps[3:]) if len(fps) > 3 else None,
            'stages': sorted(set(re.findall(r'files/(stg\d{3})\.one', txt)))}


def load_calls(name):
    p = os.path.join(DUMPS, name + '.calls')
    if not os.path.exists(p): return None
    b = open(p, 'rb').read()
    a = array.array('I'); a.frombytes(b[8:])
    return struct.unpack('<d', b[:8])[0], a


def per_frame_funcs(name):
    """Functions called about twice as often per second at 60 fps as at 30 fps."""
    c30, c60 = load_calls(name + '_30'), load_calls(name + '_60')
    if not c30 or not c60: return None
    (s30, a30), (s60, a60) = c30, c60
    out = set()
    for f in FUNCS:
        i = (f['start'] - 0x80000000) >> 2
        if i >= len(a30): continue
        r30, r60 = a30[i] / s30, a60[i] / s60
        if r30 >= 1 and 1.7 < r60 / r30 < 2.4: out.add(f['start'])
    return out


def fmt(v, f='{:.1f}'):
    return '-' if v is None else f.format(v)


def main():
    names = sys.argv[1:] or sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(ROOT, 'tools', 'tas', 'missions', '*.tas')))
    base = None
    lines = ['# 30 vs 60 fps comparison', '',
             '| mission | stages | timer speed 30/60 | fps min 30/60 | median speed 30/60 | p90 speed 30/60 | jumps 30/60 | rings 30/60 | errors |',
             '|---|---|---|---|---|---|---|---|---|']
    pf = {}
    for n in names:
        s30, s60 = stats(load_csv(n + '_30')), stats(load_csv(n + '_60'))
        l30, l60 = log_info(n + '_30'), log_info(n + '_60')
        if not s30 or not s60:
            lines.append(f'| {n} | missing run | | | | | | | |'); continue
        pf[n] = per_frame_funcs(n)
        lines.append('| {} | {} | {} / {} | {} / {} | {} / {} | {} / {} | {} / {} | {} / {} | {} |'.format(
            n, ' '.join(l30.get('stages', [])), fmt(s30['timer_rate'], '{:.3f}'), fmt(s60['timer_rate'], '{:.3f}'),
            fmt(l30.get('fps_min')), fmt(l60.get('fps_min')), fmt(s30['speed_p50']), fmt(s60['speed_p50']),
            fmt(s30['speed_p90']), fmt(s60['speed_p90']), s30['jumps'], s60['jumps'],
            fmt(s30['rings'], '{:.0f}'), fmt(s60['rings'], '{:.0f}'),
            l30.get('errors', 0) + l60.get('errors', 0) + l30.get('fails', 0) + l60.get('fails', 0)))
    # once-per-frame functions that only appear in some missions (stage-specific code)
    sets = [s for s in pf.values() if s]
    if sets:
        common = set.intersection(*sets)
        lines += ['', f'Once-per-rendered-frame functions common to every mission: {len(common)}', '',
                  '| mission | extra per-frame functions |', '|---|---|']
        for n, s in pf.items():
            if s:
                extra = sorted(s - common)
                lines.append(f'| {n} | {len(extra)}: ' + ' '.join(f'{a:08x}' for a in extra[:40]) + (' ...' if len(extra) > 40 else '') + ' |')
    # full-settings performance pass (tas_batch.ps1 -Full -Rates 60)
    perf = []
    for n in names:
        p = os.path.join(TAS, n + '_60_full.log')
        if not os.path.exists(p): continue
        txt = open(p, errors='replace').read()
        fps = [float(x) for x in re.findall(r'status: ([0-9.]+) fps', txt)][3:]   # skip boot
        if not fps: continue
        srt = sorted(fps)
        low = sum(1 for f in fps if f < 58) / len(fps)
        perf.append(f'| {n} | {min(fps):.1f} | {srt[len(srt) // 20]:.1f} | {100 * low:.0f}% | {len(re.findall(r"^\[E\]", txt, re.M))} |')
    if perf:
        lines += ['', '## 60 fps at full settings', '', '| mission | min fps | 5th percentile | samples < 58 fps | errors |',
                  '|---|---|---|---|---|'] + perf
    open(os.path.join(TAS, 'report.md'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:len(names) + 4]))


if __name__ == '__main__':
    main()
