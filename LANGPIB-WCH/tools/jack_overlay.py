#!/usr/bin/env python3
# Overlay SMD and THT RJ45 footprints aligned on the mating face (max courtyard y = front) and report pad clashes.
# usage: jack_overlay.py dir.pretty [--clr 0.2] [--ring 0.25] [--front NAME=dy ...]   (THT copper = drill + 2*ring)
import glob, math, os, re, sys

def load(f, front_adj=0.0):
    t = open(f).read()
    pads = []
    for m in re.finditer(r'\(pad "?([^" ]*)"? (smd|thru_hole|np_thru_hole) (\w+) \(at ([-\d.]+) ([-\d.]+)[^)]*\) \(size ([\d.]+) ([\d.]+)\)([^\n]*)', t):
        num, kind, shape, x, y, w, h, rest = m.groups()
        d = re.search(r'\(drill ([\d.]+)', rest)
        pads.append(dict(num=num, kind=kind, x=float(x), y=float(y), w=float(w), h=float(h), drill=float(d.group(1)) if d else 0))
    ys = [float(b) for lay in ('F.CrtYd', 'F.SilkS') for s in re.findall(r'\(fp_line[^\n]*?\(layer ' + re.escape(lay) + r'\)', t)
          for a, b in re.findall(r'\((?:start|end) ([-\d.]+) ([-\d.]+)\)', s)]
    front = max(ys) + front_adj
    for p in pads: p['y'] -= front    # front face at y = 0, body at y < 0
    return pads

def rect_circle_gap(r, cx, cy, rad):    # copper gap between an axis-aligned SMD rect and a round THT pad
    dx = max(abs(cx - r['x']) - r['w'] / 2, 0); dy = max(abs(cy - r['y']) - r['h'] / 2, 0)
    return math.hypot(dx, dy) - rad

if __name__ == '__main__':
    a = sys.argv[1:]; clr = float(a[a.index('--clr') + 1]) if '--clr' in a else 0.2
    ring = float(a[a.index('--ring') + 1]) if '--ring' in a else 0.25
    adj = {k: float(v) for s in a if '=' in s for k, v in [s.split('=')]}
    fs = sorted(glob.glob(os.path.join(a[0], '*.kicad_mod')))
    name = lambda f: os.path.basename(f)[:-10]
    smd = [f for f in fs if 'SMD' in name(f)]; tht = [f for f in fs if 'TH_' in name(f)]
    for s in smd:
        sp = [p for p in load(s, adj.get(name(s), 0)) if p['kind'] == 'smd']
        for th in tht:
            tp = [p for p in load(th, adj.get(name(th), 0)) if p['kind'] == 'thru_hole' and p['num']]
            hits = []
            for q in tp:
                rad = q['drill'] / 2 + ring
                for p in sp:
                    g = rect_circle_gap(p, q['x'], q['y'], rad)
                    if g < clr: hits.append(f"T{q['num']}@({q['x']:.2f},{q['y']:.2f})~S{p['num']} {g:+.2f}")
            print(f"{name(s)[:28]:28s} x {name(th)[:30]:30s} {len(hits):2d}  {'; '.join(hits[:6])}")
