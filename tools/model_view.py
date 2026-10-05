"""Render one model out of a VROM image, on its own, fitted to the frame.

  python tools/model_view.py <vrom.bin> <word-address> <out.ppm>

Why this exists: the scene renderer can only be checked against a scene, and
a 3D decode that is wrong does not fail, it draws. A single model can be
checked on its own terms -- a building either looks like a building or it
does not -- and that settles the polygon format and the vertex decode
without needing the camera or the scale to be right first. It is also the
quickest way to tell whether a VROM image was interleaved correctly: a wrong
one contains no models at all.

No ROM is read from or written to the repository; the path is yours.
"""
import numpy as np, struct, math, sys

raw = np.fromfile(sys.argv[1] if len(sys.argv) > 1 else "roms/lw_vrom.bin",
                  dtype=np.uint8)
W = raw.view(">u4")

def sx(v):
    v &= 0xFFFFFF
    return v - (1 << 24) if v >> 23 else v

def parse(base):
    i, prev, out = base, [], []
    for _ in range(20000):
        ph = [int(x) for x in W[i:i+7]]
        if len(ph) < 7: return out
        n = [sx(ph[k] >> 8) / 4194304.0 for k in (1, 2, 3)]
        if abs(math.sqrt(sum(x*x for x in n)) - 1.0) > 0.005: return out
        nv = 4 if (ph[0] & 0x40) else 3
        vs = [prev[k] for k in range(4) if (ph[0] >> k) & 1 and k < len(prev)]
        new = nv - len(vs)
        if new < 0: return out
        for k in range(new):
            o = i + 7 + 4*k
            vs.append((sx(int(W[o])), sx(int(W[o+1])), sx(int(W[o+2]))))
        col = ((ph[4] >> 24) & 255, (ph[4] >> 16) & 255, (ph[4] >> 8) & 255)
        if len(vs) >= 3: out.append((n, col, vs))
        prev = vs
        i += 7 + 4*new
        if ph[1] & 4: return out
    return out

def render(polys, path, size=(496, 384)):
    Wd, Hd = size
    pts = [v for _, _, vs in polys for v in vs]
    if not pts: return 0
    cx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2
    cy = (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2
    cz = (min(p[2] for p in pts) + max(p[2] for p in pts)) / 2
    ext = max(max(p[i] for p in pts) - min(p[i] for p in pts) for i in range(3))
    if ext <= 0: return 0
    s = min(Wd, Hd) * 0.42 / (ext / 2)
    fb = [0]*(Wd*Hd); zb = [1e30]*(Wd*Hd)
    L = (0.4, 0.6, -0.69)
    for n, col, vs in polys:
        P = [( Wd/2 + (v[0]-cx)*s, Hd/2 - (v[1]-cy)*s, (v[2]-cz)*s) for v in vs]
        lit = abs(sum(n[i]*L[i] for i in range(3)))
        sh = 0.2 + 0.8*min(1.0, lit)
        rgb = tuple(min(255, int(c*sh)) for c in col)
        for k in range(1, len(P)-1):
            a, b, c = P[0], P[k], P[k+1]
            d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1])
            if abs(d) < 1e-9: continue
            x0 = max(0, int(min(a[0],b[0],c[0]))); x1 = min(Wd-1, int(max(a[0],b[0],c[0])))
            y0 = max(0, int(min(a[1],b[1],c[1]))); y1 = min(Hd-1, int(max(a[1],b[1],c[1])))
            for y in range(y0, y1+1):
                for x in range(x0, x1+1):
                    l0 = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1]))/d
                    l1 = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1]))/d
                    l2 = 1-l0-l1
                    if l0 < 0 or l1 < 0 or l2 < 0: continue
                    z = l0*a[2]+l1*b[2]+l2*c[2]
                    o = y*Wd+x
                    if z < zb[o]: zb[o]=z; fb[o]=(rgb[0]<<16)|(rgb[1]<<8)|rgb[2]
    open(path,"wb").write(b"P6\n%d %d\n255\n" % (Wd, Hd) +
        bytes(b for p in fb for b in (p>>16, (p>>8)&255, p&255)))
    return sum(1 for p in fb if p)

if __name__ == "__main__":
    base = int(sys.argv[2], 0)
    polys = parse(base)
    pts = [v for _,_,vs in polys for v in vs]
    if pts:
        for i, ax in enumerate("xyz"):
            lo, hi = min(p[i] for p in pts), max(p[i] for p in pts)
            print(f"  {ax}: {lo:9d}..{hi:9d}  span {hi-lo:9d}")
    n = render(polys, sys.argv[3])
    print(f"model {base:07X}: {len(polys)} polygons, {n} pixels -> {sys.argv[3]}")
