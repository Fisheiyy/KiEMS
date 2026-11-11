"""Export one or more nets' copper for the DC prototype (run with KiCad's python).
usage: export_net.py board.kicad_pcb out.json NET [NET ...]
     : export_net.py board.kicad_pcb --list   (power-ish nets and their pin types)
"""
import json, sys, pcbnew

board = pcbnew.LoadBoard(sys.argv[1])
IU = 1e6  # nm per mm

if sys.argv[2] == "--list":
    from collections import defaultdict
    pins = defaultdict(list)
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname():
                pins[p.GetNetname()].append("%s.%s:%s" % (fp.GetReference(), p.GetNumber(), p.GetPinType() or "-"))
    for net, ps in sorted(pins.items(), key=lambda kv: -len(kv[1])):
        print("%-24s %3d  %s" % (net, len(ps), " ".join(ps[:14])))
    sys.exit(0)

out_path, nets = sys.argv[2], set(sys.argv[3:])
copper_layers = [l for l in board.GetEnabledLayers().CuStack()]
names = {l: board.GetLayerName(l) for l in copper_layers}

def polys(sps):
    res = []
    for i in range(sps.OutlineCount()):
        o = sps.Outline(i)
        loops = [[[o.CPoint(j).x / IU, -o.CPoint(j).y / IU] for j in range(o.PointCount())]]
        for h in range(sps.HoleCount(i)):
            hl = sps.Hole(i, h)
            loops.append([[hl.CPoint(j).x / IU, -hl.CPoint(j).y / IU] for j in range(hl.PointCount())])
        res.append(loops)
    return res

maxerr = int(0.005 * IU)
layers = {}
for l in copper_layers:
    for net in nets:
        sps = pcbnew.SHAPE_POLY_SET()
        for z in board.Zones():
            if z.GetNetname() == net and z.IsOnLayer(l) and z.HasFilledPolysForLayer(l):
                sps.Append(z.GetFilledPolysList(l))
        for t in board.GetTracks():
            if t.GetNetname() == net and t.IsOnLayer(l):
                t.TransformShapeToPolygon(sps, l, 0, maxerr, pcbnew.ERROR_INSIDE)
        for fp in board.GetFootprints():
            for p in fp.Pads():
                if p.GetNetname() == net and p.IsOnLayer(l):
                    p.TransformShapeToPolygon(sps, l, 0, maxerr, pcbnew.ERROR_INSIDE)
        sps.Simplify()
        layers.setdefault(names[l], {})[net] = polys(sps)

barrels = []
for t in board.GetTracks():
    if t.GetClass() == "PCB_VIA" and t.GetNetname() in nets:
        p = t.GetPosition()
        top, bot = t.TopLayer(), t.BottomLayer()
        barrels.append({"net": t.GetNetname(), "x": p.x / IU, "y": -p.y / IU, "drill": t.GetDrillValue() / IU,
                        "pad": t.GetWidth(top) / IU, "from": names[top], "to": names[bot], "ref": ""})
pads = []
for fp in board.GetFootprints():
    for p in fp.Pads():
        if p.GetNetname() not in nets:
            continue
        pos = p.GetPosition()
        onid = [l for l in copper_layers if p.IsOnLayer(l)]; on = [names[l] for l in onid]
        sps = pcbnew.SHAPE_POLY_SET()
        p.TransformShapeToPolygon(sps, onid[0], 0, maxerr, pcbnew.ERROR_INSIDE)
        pads.append({"ref": fp.GetReference(), "pin": p.GetNumber(), "net": p.GetNetname(), "type": p.GetPinType(),
                     "x": pos.x / IU, "y": -pos.y / IU, "layers": on, "shape": polys(sps)})
        if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH and p.GetDrillSize().x > 0:
            barrels.append({"net": p.GetNetname(), "x": pos.x / IU, "y": -pos.y / IU, "drill": min(p.GetDrillSize().x, p.GetDrillSize().y) / IU,
                            "pad": min(p.GetSize(onid[0]).x, p.GetSize(onid[0]).y) / IU, "from": on[0], "to": on[-1],
                            "ref": "%s.%s" % (fp.GetReference(), p.GetNumber())})

stack = {"board_thickness": board.GetDesignSettings().GetBoardThickness() / IU}
json.dump({"layers": layers, "order": [names[l] for l in copper_layers], "barrels": barrels, "pads": pads, "stackup": stack},
          open(out_path, "w"))
print("layers", {k: {n: len(v) for n, v in d.items()} for k, d in layers.items()}, "barrels", len(barrels), "pads", len(pads))
