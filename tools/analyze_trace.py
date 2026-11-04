#!/usr/bin/env python3
"""Analyze the last per-frame trace block in sinking_star_pan.log.
Prints frame-time stats and the frames where the camera's motion is uneven."""
import sys, statistics as st
log = sys.argv[1] if len(sys.argv) > 1 else 'dist/sinking_star_pan.log'
lines = open(log).read().splitlines()
starts = [i for i, l in enumerate(lines) if l.startswith('--- trace')]
if not starts: sys.exit('no trace block')
rows = []
for l in lines[starts[-1] + 1:]:
    p = l.split()
    if len(p) != 7: break
    try: rows.append([float(x) for x in p])
    except ValueError: break
print(f'{len(rows)} frames')
dts = [r[1] for r in rows[1:]]
print(f'frame dt: mean {st.mean(dts)*1000:.1f} ms  stdev {st.pstdev(dts)*1000:.1f}  min {min(dts)*1000:.1f}  max {max(dts)*1000:.1f}')
raws = [r[4] for r in rows[1:] if r[4] > 0.002]
print(f'raw frame dt: mean {st.mean(raws)*1000:.1f} ms  stdev {st.pstdev(raws)*1000:.1f}  min {min(raws)*1000:.1f}  max {max(raws)*1000:.1f}')
# eased offset velocity per frame (world units / s): even velocity = smooth motion on screen
moving = []
for i in range(1, len(rows)):
    t, dt, tx, ex, raw, avg, midx = rows[i]
    dex = ex - rows[i-1][3]; dtx = tx - rows[i-1][2]
    moving.append((i, raw*1000, dt*1000, dtx, dex, dex / raw if raw > 0 else 0))
mv = [m for m in moving if abs(m[4]) > 0.005]
print(f'{len(mv)} frames with view motion')
if mv:
    vs = [m[5] for m in mv]
    print(f'eased velocity u/s: mean {st.mean(vs):.2f} stdev {st.pstdev(vs):.2f}')
    print(' idx raw(ms) used(ms) d_target d_eased  vel')
    for m in mv[:40]: print(f'{m[0]:4d} {m[1]:7.1f} {m[2]:7.1f} {m[3]:8.3f} {m[4]:8.3f} {m[5]:7.2f}')
