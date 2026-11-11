"""Prototype DC IR-drop / current-density solver (finite-volume on a raster, per copper layer).

Model
  - each copper layer of the net is rasterised at pitch h; every copper cell is a node
  - neighbouring cells on one layer are joined by the sheet conductance sigma*t (square cells)
  - a via / PTH barrel is a resistor rho*L / (pi*(d+p)*p) between its layers' nodes; at each end
    it ties to every cell under its annular ring (near-equipotential ring)
  - the source pad's cells are held at V (Dirichlet); each load pad sinks its current spread
    evenly over its cells
  - solve G v = i (SPD after eliminating Dirichlet nodes), then J = -sigma grad V per cell

usage: solve.py export.json NET --source REF.PIN=V --load REF.PIN=A [...] [--h 0.05] [--png out]
       solve.py --selftest
"""
import argparse, json, sys
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
from scipy.sparse.csgraph import connected_components
from matplotlib.path import Path

RHO = 1.72e-8      # ohm m, annealed copper at 20 C
T_CU = 35e-6       # m, 1 oz
PLATING = 25e-6    # m, via barrel wall


def raster(polys, x0, y0, nx, ny, h):
    """polys: [[outer, hole, hole...], ...] in mm -> bool mask [ny, nx] of cell centres inside."""
    xs = x0 + (np.arange(nx) + 0.5) * h
    ys = y0 + (np.arange(ny) + 0.5) * h
    mask = np.zeros((ny, nx), bool)
    for loops in polys:
        outer = np.array(loops[0])
        lo, hi = outer.min(0), outer.max(0)
        i0, i1 = max(0, int((lo[0] - x0) / h) - 1), min(nx, int((hi[0] - x0) / h) + 2)
        j0, j1 = max(0, int((lo[1] - y0) / h) - 1), min(ny, int((hi[1] - y0) / h) + 2)
        if i1 <= i0 or j1 <= j0:
            continue
        gx, gy = np.meshgrid(xs[i0:i1], ys[j0:j1])
        pts = np.column_stack([gx.ravel(), gy.ravel()])
        inside = Path(outer).contains_points(pts)
        for hole in loops[1:]:
            inside &= ~Path(np.array(hole)).contains_points(pts)
        mask[j0:j1, i0:i1] |= inside.reshape(gy.shape)
    return mask


def solve(layers, barrels, sources, loads, h, thickness_mm, t_cu=T_CU):
    """layers: {name: polys} in stack order; barrels: dicts(x,y,drill,pad,from,to);
    sources/loads: [(layer, polys_mm, value)].  Returns per-layer V, J maps and via currents."""
    names = list(layers)
    allpts = np.array([p for polys in layers.values() for loops in polys for p in loops[0]])
    pad = 2 * h
    x0, y0 = allpts.min(0) - pad
    nx, ny = (np.ceil((allpts.max(0) + pad - (x0, y0)) / h)).astype(int)
    masks = {n: raster(layers[n], x0, y0, nx, ny, h) for n in names}
    index = {}
    count = 0
    for n in names:
        idx = -np.ones((ny, nx), int)
        k = masks[n].sum()
        idx[masks[n]] = np.arange(count, count + k)
        count += k
        index[n] = idx
    gs = t_cu / RHO  # sheet conductance, S per square
    rows, cols, vals = [], [], []

    def link(a, b, g):
        rows.extend([a, b]); cols.extend([b, a]); vals.extend([-g, -g])
        rows.extend([a, b]); cols.extend([a, b]); vals.extend([g, g])

    for n in names:
        idx = index[n]
        for a, b in ((idx[:, :-1], idx[:, 1:]), (idx[:-1, :], idx[1:, :])):
            m = (a >= 0) & (b >= 0)
            aa, bb = a[m], b[m]
            rows.extend(np.concatenate([aa, bb, aa, bb]))
            cols.extend(np.concatenate([bb, aa, aa, bb]))
            vals.extend(np.concatenate([np.full(len(aa) * 2, -gs), np.full(len(aa) * 2, gs)]))

    # barrel nodes come after the cells
    order = {n: i for i, n in enumerate(names)}
    z = np.linspace(0, thickness_mm, len(names))  # copper layer heights (even spread; real stackup later)
    via_rec = []
    nextnode = count
    for b in barrels:
        lo, hi = order[b["from"]], order[b["to"]]
        span = [names[i] for i in range(lo, hi + 1)]
        ends = []
        for n in span:
            node = nextnode; nextnode += 1
            i, j = int((b["x"] - x0) / h), int((b["y"] - y0) / h)
            r = max(b["pad"] / 2, h * 0.75)
            ri = int(np.ceil(r / h)) + 1
            cells = []
            for jj in range(j - ri, j + ri + 1):
                for ii in range(i - ri, i + ri + 1):
                    if 0 <= ii < nx and 0 <= jj < ny and index[n][jj, ii] >= 0:
                        cx, cy = x0 + (ii + .5) * h, y0 + (jj + .5) * h
                        if (cx - b["x"]) ** 2 + (cy - b["y"]) ** 2 <= r * r:
                            cells.append(index[n][jj, ii])
            for c in cells:
                link(node, c, gs * 20)
            ends.append((n, node, len(cells)))
        segs = []
        for (na, a, _), (nb, bnode, _) in zip(ends, ends[1:]):
            L = abs(z[order[nb]] - z[order[na]]) * 1e-3
            d = b["drill"] * 1e-3
            R = RHO * L / (np.pi * (d + PLATING) * PLATING)
            link(a, bnode, 1 / R)
            segs.append((a, bnode, 1 / R))
        via_rec.append((b, ends, segs))
    N = nextnode
    G = sp.csr_matrix((vals, (rows, cols)), shape=(N, N))

    def pad_cells(layer, polys):
        m = raster(polys, x0, y0, nx, ny, h) & masks[layer]
        return index[layer][m]

    fixed = np.zeros(N, bool); vfix = np.zeros(N)
    for layer, polys, v in sources:
        c = pad_cells(layer, polys)
        if len(c) == 0:
            raise SystemExit("source pad has no copper cells")
        fixed[c] = True; vfix[c] = v
    inj = np.zeros(N)
    warnings = []
    for layer, polys, amps, name in loads:
        c = pad_cells(layer, polys)
        if len(c) == 0:
            warnings.append("load %s has no copper cells" % name); continue
        inj[c] -= amps / len(c)

    # only components holding a source can be solved; anything else floats
    ncomp, lab = connected_components(G, directed=False)
    live = np.isin(lab, np.unique(lab[fixed]))
    for layer, polys, amps, name in loads:
        c = pad_cells(layer, polys)
        if len(c) and not live[c].any():
            warnings.append("load %s isn't connected to the source" % name)
            inj[c] = 0
    free = live & ~fixed
    Gff = G[free][:, free].tocsc()
    rhs = inj[free] - G[free][:, fixed] @ vfix[fixed]
    v = np.full(N, np.nan)
    v[fixed] = vfix[fixed]
    v[free] = spla.spsolve(Gff, rhs)
    resid = np.abs(Gff @ v[free] - rhs).max()

    out = {"x0": x0, "y0": y0, "h": h, "layers": {}, "vias": [], "warnings": warnings, "nodes": int(free.sum()),
           "residual": float(resid)}
    for n in names:
        V = np.full((ny, nx), np.nan)
        V[masks[n]] = v[index[n][masks[n]]]
        # J from face currents averaged to cells (A/mm^2): face current = gs*dV, over h*t
        jx = np.zeros((ny, nx)); jy = np.zeros((ny, nx))
        fx = -np.diff(V, axis=1) / (h * 1e-3) / RHO  # A/m^2 on x faces
        fy = -np.diff(V, axis=0) / (h * 1e-3) / RHO
        fx = np.nan_to_num(fx); fy = np.nan_to_num(fy)
        jx[:, :-1] += fx / 2; jx[:, 1:] += fx / 2
        jy[:-1, :] += fy / 2; jy[1:, :] += fy / 2
        J = np.hypot(jx, jy) * 1e-6
        J[~masks[n]] = np.nan
        out["layers"][n] = {"V": V, "J": J, "jx": jx, "jy": jy}
    for b, ends, segs in via_rec:
        I = max(abs(g * (v[a] - v[c])) for a, c, g in segs) if segs else 0.0
        out["vias"].append({"ref": b.get("ref") or "via", "x": b["x"], "y": b["y"], "drill": b["drill"],
                            "I": 0.0 if np.isnan(I) else float(I)})
    return out


def selftest():
    # 10 mm x 1 mm strip, 35 um, 1 A. The source pad (0..0.2 mm) is held, the load pad (9.8..10 mm)
    # draws evenly: full current over 9.6 mm, then a linear taper over the last 0.2 mm -> 9.7 mm.
    strip = [[[[0, 0], [10, 0], [10, 1], [0, 1]]]]
    srcpad = [[[[0, 0], [0.2, 0], [0.2, 1], [0, 1]]]]
    loadpad = [[[[9.8, 0], [10, 0], [10, 1], [9.8, 1]]]]
    expect = RHO * 9.7e-3 / (1e-3 * T_CU)
    for h in (0.1, 0.05, 0.025):
        r = solve({"F.Cu": strip}, [], [("F.Cu", srcpad, 0.0)], [("F.Cu", loadpad, 1.0, "L")], h, 1.6)
        far = np.nanmin(r["layers"]["F.Cu"]["V"])
        print("strip h=%.3f: far-end drop %.4f mV (analytic %.4f, %+.2f%%), max J %.1f A/mm^2 (analytic %.1f)" % (
            h, -far * 1e3, expect * 1e3, (-far / expect - 1) * 100, np.nanmax(r["layers"]["F.Cu"]["J"]),
            1 / (1e-3 * T_CU) * 1e-6))
    # two layers, one via: 0.3 mm drill barrel, 1.6 mm board -> rho L/(pi (d+p) p)
    sq = [[[[0, 0], [2, 0], [2, 2], [0, 2]]]]
    via = {"x": 1, "y": 1, "drill": 0.3, "pad": 0.6, "from": "F.Cu", "to": "B.Cu"}
    r = solve({"F.Cu": sq, "B.Cu": sq}, [via], [("F.Cu", [[[[0, 0], [2, 0], [2, 0.1], [0, 0.1]]]], 0.0)],
              [("B.Cu", [[[[0, 1.9], [2, 1.9], [2, 2], [0, 2]]]], 1.0, "L")], 0.025, 1.6)
    Rb = RHO * 1.6e-3 / (np.pi * 0.325e-3 * 25e-6)
    print("via current %.4f A (expect 1); barrel R %.3f mOhm; total drop %.3f mV" % (
        r["vias"][0]["I"], Rb * 1e3, -np.nanmin(r["layers"]["B.Cu"]["V"]) * 1e3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export", nargs="?")
    ap.add_argument("net", nargs="?")
    ap.add_argument("--source", action="append", default=[])
    ap.add_argument("--load", action="append", default=[])
    ap.add_argument("--h", type=float, default=0.05)
    ap.add_argument("--png")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    d = json.load(open(a.export))
    layers = {n: d["layers"][n].get(a.net, []) for n in d["order"] if d["layers"][n].get(a.net)}
    barrels = [b for b in d["barrels"] if b["net"] == a.net and b["from"] in layers and b["to"] in layers]
    pads = {"%s.%s" % (p["ref"], p["pin"]): p for p in d["pads"] if p["net"] == a.net}

    def term(spec):
        key, val = spec.split("=")
        p = pads[key]
        return (p["layers"][0], p["shape"], float(val), key)

    sources = [t[:3] for t in map(term, a.source)]
    loads = [term(s) for s in a.load]
    import time
    t0 = time.time()
    r = solve(layers, barrels, sources, loads, a.h, d["stackup"]["board_thickness"])
    print("%d unknowns, solved in %.2f s, residual %.2e" % (r["nodes"], time.time() - t0, r["residual"]))
    for w in r["warnings"]:
        print("warning:", w)
    vsrc = sources[0][2]
    for key, (layer, polys, amps, name) in zip(a.load, loads):
        V = r["layers"][layer]["V"]
        x0, y0, h = r["x0"], r["y0"], r["h"]
        m = raster(polys, x0, y0, V.shape[1], V.shape[0], h) & ~np.isnan(V)
        print("  load %-6s %.3f A  V = %.4f V  |V - source| %.2f mV" % (name, amps, V[m].mean(), abs(vsrc - V[m].mean()) * 1e3))
    for n, L in r["layers"].items():
        print("  %s: max J %.1f A/mm^2, max |V - source| %.2f mV" % (n, np.nanmax(L["J"]), np.nanmax(np.abs(L["V"] - vsrc)) * 1e3))
    top = sorted(r["vias"], key=lambda v: -v["I"])[:6]
    print("  busiest barrels:", ", ".join("%s(%.1f,%.1f) %.3f A" % (v["ref"], v["x"], v["y"], v["I"]) for v in top))
    if a.png:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.colors import LogNorm
        nl = len(r["layers"])
        fig, axs = plt.subplots(2, nl, figsize=(6 * nl, 10), squeeze=False)
        ext = [r["x0"], r["x0"] + r["h"] * next(iter(r["layers"].values()))["V"].shape[1],
               r["y0"], r["y0"] + r["h"] * next(iter(r["layers"].values()))["V"].shape[0]]
        dmax = max(np.nanmax(np.abs(L["V"] - vsrc)) for L in r["layers"].values())
        jmax = max(np.nanmax(L["J"]) for L in r["layers"].values())
        for k, (n, L) in enumerate(r["layers"].items()):
            im = axs[0][k].imshow(np.abs(L["V"] - vsrc) * 1e3, origin="lower", extent=ext, cmap="viridis",
                                  vmin=0, vmax=dmax * 1e3)
            axs[0][k].set_title("%s %s: |V - source| (mV)" % (a.net, n)); fig.colorbar(im, ax=axs[0][k])
            im = axs[1][k].imshow(L["J"], origin="lower", extent=ext, cmap="inferno",
                                  norm=LogNorm(vmin=jmax / 1e3, vmax=jmax))
            step = max(1, int(0.5 / r["h"]))
            ys, xs = np.mgrid[0:L["V"].shape[0]:step, 0:L["V"].shape[1]:step]
            jx, jy = L["jx"][::step, ::step], L["jy"][::step, ::step]
            mag = np.hypot(jx, jy); ok = mag > mag.max() * 0.02
            axs[1][k].quiver(r["x0"] + (xs[ok] + .5) * r["h"], r["y0"] + (ys[ok] + .5) * r["h"],
                             jx[ok] / mag[ok], jy[ok] / mag[ok], color="cyan", scale=60, width=0.002)
            axs[1][k].set_title("%s %s: |J| (A/mm^2)" % (a.net, n)); fig.colorbar(im, ax=axs[1][k])
            for v in r["vias"]:
                axs[1][k].plot(v["x"], v["y"], "o", mfc="none", mec="w", ms=4)
        fig.tight_layout(); fig.savefig(a.png, dpi=110)
        print("wrote", a.png)


if __name__ == "__main__":
    main()
