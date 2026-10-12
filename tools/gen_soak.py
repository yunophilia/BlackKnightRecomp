"""Generate a random-input soak script for long stability runs.

  python tools/gen_soak.py out/soak.txt [--seconds 900] [--seed N]

The script first replays tools/scripts/mission1.txt up to the stage start, then mixes
movement, slashes, jumps, guards, menu confirmations and short D-pad bursts (which
also navigate the world map). Run it with:

  powershell -ExecutionPolicy Bypass -File tools/run_test.ps1 -Seconds 900 \\
      -GameArgs "--nobox --input-script <abs path> --shots 1800 --shots-start 1800"

then look for [E] lines in out/BlackKnightRecomp.log and review the screenshots.
"""
import argparse
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--seconds', type=float, default=900)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    head = [l for l in open(os.path.join(HERE, 'scripts', 'mission1.txt')).read().split('\n')
            if l.strip() and float(l.split()[0]) < 171]
    rnd = random.Random(a.seed)
    t, lines = 171.0, []
    while t < a.seconds - 10:
        r = rnd.random()
        if r < 0.32:
            lines.append(f'{t:.1f} {rnd.uniform(1, 4):.1f} LX={rnd.uniform(-0.8, 0.8):.2f} LY={rnd.uniform(0.3, 1):.2f}')
        elif r < 0.55:
            lines.append(f'{t:.1f} 0.15 X')
        elif r < 0.66:
            lines.append(f'{t:.1f} 0.2 A')
        elif r < 0.72:   # "YES" on confirmation dialogs
            lines += [f'{t:.1f} 0.2 UP', f'{t + 0.5:.1f} 0.2 A']
            t += 0.6
        elif r < 0.82:   # D-pad bursts: menus and the world map
            d, n = rnd.choice(['RIGHT', 'LEFT', 'UP', 'DOWN']), rnd.randint(1, 3)
            lines += [f'{t + 0.4 * k:.1f} 0.15 {d}' for k in range(n)]
            lines.append(f'{t + 0.4 * n + 0.3:.1f} 0.2 A')
            t += 0.4 * n + 0.4
        elif r < 0.86:
            lines.append(f'{t:.1f} 0.2 B')
        elif r < 0.90:
            lines.append(f'{t:.1f} 0.5 RB')
        elif r < 0.92:   # title screen needs A+B
            lines.append(f'{t:.1f} 0.3 A+B')
        else:
            lines.append(f'{t:.1f} 0.2 A')
        t += rnd.uniform(0.3, 1.2)
    open(a.out, 'w').write('\n'.join(head + lines) + '\n')
    print(f'{len(lines)} events, {a.seconds:.0f} s')


if __name__ == '__main__':
    main()
