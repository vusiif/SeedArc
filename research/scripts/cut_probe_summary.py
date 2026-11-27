# SPDX-License-Identifier: GPL-3.0-or-later
"""DEV-037: summarise cut-probe JSON files (research/ideas/frame-cut-r0.md).

  python -I research/scripts/cut_probe_summary.py FILE.json ...

Per file: frame bytes of every rule relative to greedy, and the shift-1..7 range (the noise
reference). A candidate "beats the noise" when it is below every shift result and below greedy.
"""
import json
import sys


def main(paths):
    print('| tree | variant | greedy | shift range | balanced | path-w25 | path-w50 | sim-w25 | sim-w50 |')
    print('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    for path in paths:
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        rules = {r['rule']: r for r in d['rules']}
        g = rules['greedy']['compressed_bytes']

        def pct(name):
            return 100.0 * (rules[name]['compressed_bytes'] - g) / g

        shifts = [pct(f'shift-{k}') for k in range(1, 8)]
        low = min(shifts)
        cells = []
        for name in ['balanced', 'path-w25', 'path-w50', 'sim-w25', 'sim-w50']:
            mark = '**' if pct(name) < min(0.0, low) else ''
            cells.append(f"{mark}{pct(name):+.2f}%{mark} ({rules[name]['frames']})")
        tree = path.replace('\\', '/').rsplit('/', 1)[-1].rsplit('-', 3)[0]
        print(f"| {tree} | {d['variant']} | {g} ({rules['greedy']['frames']}) | "
              f"{low:+.2f}% .. {max(shifts):+.2f}% | " + ' | '.join(cells) + ' |')


if __name__ == '__main__':
    main(sys.argv[1:])
