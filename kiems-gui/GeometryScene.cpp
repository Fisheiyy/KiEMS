#include "GeometryScene.hpp"

#include "Document.hpp"
#include "SimulationEditor.hpp"

#include "kiems/config.hpp"
#include "kiems/constants.hpp"

#include <QFile>
#include <QSet>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace kiems_gui {

namespace {

using Json = nlohmann::json;

/// Simulation units per millimetre.
constexpr double kUnitsPerMm = 1e-3 / kiems::constants::baseUnit * kiems::constants::unitMultiplier;

GeometryPoint point(const Json& j) {
    return {j.at("x").get<double>(), j.at("y").get<double>()};
}

std::vector<GeometryTriangle> triangles(const Json& list) {
    std::vector<GeometryTriangle> result;
    result.reserve(list.size());
    for (const Json& t : list) result.push_back({point(t.at("a")), point(t.at("b")), point(t.at("c"))});
    return result;
}

GeometryLoop loop(const Json& list) {
    GeometryLoop result;
    result.reserve(list.size());
    for (const Json& p : list) result.push_back(point(p));
    return result;
}

std::vector<double> numbers(const Json& list) {
    std::vector<double> result;
    for (const Json& value : list) result.push_back(value.get<double>());
    return result;
}

/// Whether `p` is inside the region `loops` bound, holes and separate pieces included.
bool inside(const std::vector<GeometryLoop>& loops, GeometryPoint p) {
    bool in = false;
    for (const GeometryLoop& l : loops) {
        for (std::size_t i = 0, j = l.size() - 1; i < l.size(); j = i++) {
            if ((l[i].y > p.y) != (l[j].y > p.y) &&
                p.x < (l[j].x - l[i].x) * (p.y - l[i].y) / (l[j].y - l[i].y) + l[i].x) {
                in = !in;
            }
        }
    }
    return in;
}

double distanceToEdges(const std::vector<GeometryLoop>& loops, GeometryPoint p) {
    double best = std::numeric_limits<double>::infinity();
    for (const GeometryLoop& l : loops) {
        for (std::size_t i = 0, j = l.size() - 1; i < l.size(); j = i++) {
            const double dx = l[i].x - l[j].x, dy = l[i].y - l[j].y;
            const double length2 = dx * dx + dy * dy;
            double t = length2 > 0 ? ((p.x - l[j].x) * dx + (p.y - l[j].y) * dy) / length2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            best = std::min(best, std::hypot(p.x - (l[j].x + t * dx), p.y - (l[j].y + t * dy)));
        }
    }
    return best;
}

QColor layerColor(const GeometryContext& context, const QString& layer, int index, int count) {
    const auto it = context.layerColors.find(layer.toStdString());
    if (it != context.layerColors.end() && it->second.size() >= 7) {
        const QColor color(QString::fromStdString(it->second.substr(0, 7)));
        if (color.isValid()) return color;
    }
    // KiCad's default theme: red on top, blue on the bottom, and the inner layers between.
    if (index == 0) return QColor(200, 52, 52);
    if (index == count - 1) return QColor(77, 127, 196);
    static const QColor inner[] = {QColor(127, 200, 127), QColor(206, 125, 44), QColor(79, 203, 203),
                                   QColor(219, 98, 139)};
    return inner[(index - 1) % 4];
}

/// Builds scene geometry in mm, centred on the simulated area.
class Builder {
public:
    Builder(double cx, double cy) : _cx(cx), _cy(cy) {}

    SceneVertex vertex(double x, double y, double z, const QColor& color, double shade = 1.0) const {
        return {static_cast<float>((x - _cx) / kUnitsPerMm), static_cast<float>((y - _cy) / kUnitsPerMm),
                static_cast<float>(z / kUnitsPerMm), static_cast<float>(color.redF() * shade),
                static_cast<float>(color.greenF() * shade), static_cast<float>(color.blueF() * shade),
                static_cast<float>(color.alphaF())};
    }

    void triangle(std::vector<SceneVertex>& out, GeometryPoint a, GeometryPoint b, GeometryPoint c, double z,
                  const QColor& color) const {
        out.push_back(vertex(a.x, a.y, z, color));
        out.push_back(vertex(b.x, b.y, z, color));
        out.push_back(vertex(c.x, c.y, z, color));
    }

    void line(std::vector<SceneVertex>& out, double x1, double y1, double z1, double x2, double y2, double z2,
              const QColor& color) const {
        out.push_back(vertex(x1, y1, z1, color));
        out.push_back(vertex(x2, y2, z2, color));
    }

    void loop(std::vector<SceneVertex>& out, const GeometryLoop& points, double z, const QColor& color) const {
        for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            line(out, points[j].x, points[j].y, z, points[i].x, points[i].y, z, color);
        }
    }

private:
    double _cx, _cy;
};

/// A capsule's outline: a circle of `radius` around (x, y) when the ends coincide, otherwise the
/// stadium from (x, y) to (x2, y2). Always 2 * kHalf points, so two capsules on the same centreline
/// pair up point by point.
constexpr int kHalf = 12;

GeometryLoop capsule(const GeometryVia& via, double radius) {
    const double dx = via.x2 - via.x, dy = via.y2 - via.y;
    const double angle = std::atan2(dy, dx);
    GeometryLoop points;
    points.reserve(2 * kHalf);
    for (int end = 0; end < 2; ++end) {
        const GeometryPoint centre = end == 0 ? GeometryPoint{via.x2, via.y2} : GeometryPoint{via.x, via.y};
        for (int i = 0; i < kHalf; ++i) {
            const double a = angle - std::numbers::pi / 2 + end * std::numbers::pi +
                             std::numbers::pi * i / (kHalf - 1);
            points.push_back({centre.x + radius * std::cos(a), centre.y + radius * std::sin(a)});
        }
    }
    return points;
}

/// A plated hole: its barrel through the board and its pad ring on each copper layer.
void addVia(const Builder& b, SceneGroup& group, const GeometryVia& via, const std::vector<double>& metalZ,
            const QColor& color) {
    if (metalZ.empty() || via.drill <= 0) return;
    const double top = metalZ.front(), bottom = metalZ.back();
    const GeometryLoop hole = capsule(via, via.drill / 2);
    const GeometryLoop ring = capsule(via, std::max(via.ring, via.drill) / 2);
    const double cx = (via.x + via.x2) / 2, cy = (via.y + via.y2) / 2;
    for (std::size_t i = 0, j = hole.size() - 1; i < hole.size(); j = i++) {
        // Lit from the upper left, so the barrel reads as round.
        const double nx = (hole[i].x + hole[j].x) / 2 - cx, ny = (hole[i].y + hole[j].y) / 2 - cy;
        const double length = std::hypot(nx, ny);
        const double light = length > 0 ? (-0.6 * nx + 0.8 * ny) / length : 0;
        const double shade = 0.55 + 0.45 * std::max(0.0, light);
        const SceneVertex a = b.vertex(hole[j].x, hole[j].y, top, color, shade);
        const SceneVertex c = b.vertex(hole[i].x, hole[i].y, top, color, shade);
        const SceneVertex d = b.vertex(hole[i].x, hole[i].y, bottom, color, shade);
        const SceneVertex e = b.vertex(hole[j].x, hole[j].y, bottom, color, shade);
        group.triangles.insert(group.triangles.end(), {a, c, d, a, d, e});
    }
    for (double z : metalZ) {
        for (std::size_t i = 0, j = hole.size() - 1; i < hole.size(); j = i++) {
            b.triangle(group.triangles, ring[j], ring[i], hole[i], z, color);
            b.triangle(group.triangles, ring[j], hole[i], hole[j], z, color);
        }
    }
}

} // namespace

SimulationGeometry SimulationGeometry::load(const QString& path) {
    SimulationGeometry geometry;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        geometry.error = QObject::tr("The geometry hasn't been built yet.");
        return geometry;
    }
    try {
        const Json json = Json::parse(file.readAll().toStdString());
        const Json& board = json.at("geometry").at("slicedBoard");
        for (const Json& layer : board.at("previewLayerTriangles")) geometry.layerTriangles.push_back(triangles(layer));
        geometry.topMask = triangles(board.value("topMaskTriangles", Json::array()));
        geometry.bottomMask = triangles(board.value("bottomMaskTriangles", Json::array()));
        for (const Json& l : board.value("cutoutLoops", Json::array())) geometry.cutoutLoops.push_back(loop(l));
        if (geometry.cutoutLoops.empty()) geometry.cutoutLoops.push_back(loop(board.value("outline", Json::array())));
        for (const Json& l : board.value("npthHoleLoops", Json::array())) geometry.npthHoles.push_back(loop(l));
        for (const Json& v : board.value("stitchingVias", Json::array())) {
            const double x = v.at("x").get<double>(), y = v.at("y").get<double>();
            geometry.stitchingVias.push_back(
                {x, y, x, y, v.at("diameter").get<double>(), v.at("annularRingDiameter").get<double>()});
        }
        for (const Json& p : board.value("failedStitchingViaAttempts", Json::array())) {
            geometry.failedStitchingVias.push_back(point(p));
        }
        const Json& bounds = board.at("bounds");
        geometry.xMin = bounds.at("xMin").get<double>();
        geometry.yMin = bounds.at("yMin").get<double>();
        geometry.xMax = bounds.at("xMax").get<double>();
        geometry.yMax = bounds.at("yMax").get<double>();
        if (json.contains("grid")) {
            const Json& grid = json.at("grid").at("gridLines");
            geometry.gridX = numbers(grid.value("x", Json::array()));
            geometry.gridY = numbers(grid.value("y", Json::array()));
            geometry.gridZ = numbers(grid.value("z", Json::array()));
            geometry.pmlXMin = grid.value("pmlInnerXMin", 0.0);
            geometry.pmlXMax = grid.value("pmlInnerXMax", 0.0);
            geometry.pmlYMin = grid.value("pmlInnerYMin", 0.0);
            geometry.pmlYMax = grid.value("pmlInnerYMax", 0.0);
            geometry.pmlZMin = grid.value("pmlInnerZMin", 0.0);
            geometry.pmlZMax = grid.value("pmlInnerZMax", 0.0);
        }
    } catch (const std::exception& exception) {
        geometry = SimulationGeometry();
        geometry.error = QObject::tr("The geometry file couldn't be read: %1").arg(exception.what());
    }
    return geometry;
}

GeometryContext GeometryContext::of(const BoardData& board, const Document& document, int simulation) {
    GeometryContext context;
    context.layerColors = board.layerColors;
    context.referencePlane = document.referencePlane(simulation).value_or(1);

    // Each layer's height as the pipeline places it: the stackup scaled to simulation units, metals
    // as sheets at the top of the substrate below them (kiems::Simulation::getMetalLayerOffset()).
    kiems::EMSConfig config;
    std::vector<kiems::LayerConfig> layers;
    for (const libkicad::StackupLayer& layer : board.stackup) {
        using Kind = libkicad::StackupLayerKind;
        const kiems::LayerKind kind = layer.kind == Kind::Copper             ? kiems::LayerKind::Metal
                                      : layer.kind == Kind::SolderMaskTop    ? kiems::LayerKind::SolderMaskTop
                                      : layer.kind == Kind::SolderMaskBottom ? kiems::LayerKind::SolderMaskBottom
                                                                             : kiems::LayerKind::Substrate;
        layers.emplace_back(kind, layer.name, layer.thicknessMm, layer.epsilonR, layer.lossTangent);
    }
    config.loadStackup(std::move(layers));
    const kiems::EMSConfig scaled = config.scaledToSimulationUnits();
    double offset = 0, topMask = 0, bottomMask = 0;
    for (const kiems::LayerConfig& layer : scaled.layers()) {
        switch (layer.kind()) {
        case kiems::LayerKind::Substrate: offset -= layer.thickness(); break;
        case kiems::LayerKind::Metal:
            context.metalNames << QString::fromStdString(layer.name());
            context.metalZ.push_back(offset);
            break;
        case kiems::LayerKind::SolderMaskTop: topMask = layer.thickness(); break;
        case kiems::LayerKind::SolderMaskBottom: bottomMask = layer.thickness(); break;
        }
    }
    if (!context.metalZ.empty()) {
        context.topMaskZ = context.metalZ.front() + topMask;
        context.bottomMaskZ = context.metalZ.back() - bottomMask;
    }

    // The pipeline's frame starts at the board outline's lower-left corner.
    double originX = std::numeric_limits<double>::infinity(), originY = originX;
    for (const libkicad::PolygonLoop& l : board.geometry.outline) {
        for (const auto& [x, y] : l.pointsMm) {
            originX = std::min(originX, x * kUnitsPerMm);
            originY = std::min(originY, y * kUnitsPerMm);
        }
    }
    if (!std::isfinite(originX)) originX = originY = 0;

    for (const libkicad::ThroughHole& hole : board.throughHoles) {
        const double x = hole.xMm * kUnitsPerMm - originX, y = hole.yMm * kUnitsPerMm - originY;
        const double width = hole.drillWidthMm * kUnitsPerMm, height = hole.drillHeightMm * kUnitsPerMm;
        GeometryVia via{x, y, x, y, std::min(width, height), std::max(hole.padWidthMm, hole.padHeightMm) * kUnitsPerMm};
        if (width != height) {
            // A slot: its centreline runs along the drill's longer side.
            const double half = std::abs(width - height) / 2;
            const double angle = hole.orientationDeg * std::numbers::pi / 180 +
                                 (height > width ? std::numbers::pi / 2 : 0);
            via.x = x - half * std::cos(angle);
            via.y = y - half * std::sin(angle);
            via.x2 = x + half * std::cos(angle);
            via.y2 = y + half * std::sin(angle);
        }
        context.boardVias.push_back(via);
    }

    // Every pin the simulation gives a port: driven, terminated, or probed.
    const QStringList involved = document.involvedNets(simulation);
    const QSet<QString> nets(involved.begin(), involved.end());
    SimulationEditor editor(const_cast<Document&>(document), &board, simulation);
    for (const libkicad::PadPosition& pad : board.pads) {
        if (!nets.contains(QString::fromStdString(pad.netName))) continue;
        const QString footprint = QString::fromStdString(pad.footprintRef), number = QString::fromStdString(pad.padNumber);
        const PinState state = editor.pinState(footprint, number);
        if (!state.excited && !state.probed && !state.absorbing) continue;
        GeometryPort port;
        port.name = footprint + '.' + number;
        port.kind = state.excited ? PortKind::Excited : state.absorbing ? PortKind::Load : PortKind::Probe;
        port.x = pad.xMm * kUnitsPerMm - originX;
        port.y = pad.yMm * kUnitsPerMm - originY;
        port.width = pad.widthMm * kUnitsPerMm;
        port.height = pad.heightMm * kUnitsPerMm;
        port.orientationDegrees = pad.orientationDeg;
        port.metal = std::max(0, static_cast<int>(context.metalNames.indexOf(QString::fromStdString(pad.copperLayerName))));
        context.ports.push_back(port);
    }
    return context;
}

GeometryScene buildGeometryScene(const SimulationGeometry& geometry, const GeometryContext& context) {
    GeometryScene scene;
    const Builder b((geometry.xMin + geometry.xMax) / 2, (geometry.yMin + geometry.yMax) / 2);
    const std::vector<double>& metalZ = context.metalZ;
    const double top = metalZ.empty() ? 0 : metalZ.front();
    const double bottom = metalZ.empty() ? 0 : metalZ.back();
    // Lifts what's drawn on a surface just off it, towards the viewer's side of the board.
    const double lift = 0.004 * kUnitsPerMm;

    // Copper, layer by layer.
    const int metals = static_cast<int>(geometry.layerTriangles.size());
    for (int i = 0; i < metals; ++i) {
        const QString name = i < context.metalNames.size() ? context.metalNames[i] : QObject::tr("Layer %1").arg(i + 1);
        const double z = i < static_cast<int>(metalZ.size()) ? metalZ[static_cast<std::size_t>(i)] : 0;
        SceneGroup group{name, layerColor(context, name, i, metals)};
        group.triangles.reserve(geometry.layerTriangles[static_cast<std::size_t>(i)].size() * 3);
        for (const GeometryTriangle& t : geometry.layerTriangles[static_cast<std::size_t>(i)]) {
            b.triangle(group.triangles, t[0], t[1], t[2], z, group.swatch);
        }
        scene.groups.push_back(std::move(group));
    }

    // The board's own vias and plated pad holes that the cut keeps, and the stitching vias that
    // close its edges.
    {
        SceneGroup vias{QObject::tr("Vias"), QColor(214, 170, 76)};
        for (const GeometryVia& via : context.boardVias) {
            const GeometryPoint centre{(via.x + via.x2) / 2, (via.y + via.y2) / 2};
            if (inside(geometry.cutoutLoops, centre) ||
                distanceToEdges(geometry.cutoutLoops, centre) < std::max(via.ring, via.drill) / 2) {
                addVia(b, vias, via, metalZ, vias.swatch);
            }
        }
        if (!vias.triangles.empty()) scene.groups.push_back(std::move(vias));
        SceneGroup stitching{QObject::tr("Stitching vias"), QColor(92, 206, 170)};
        for (const GeometryVia& via : geometry.stitchingVias) addVia(b, stitching, via, metalZ, stitching.swatch);
        if (!stitching.triangles.empty()) scene.groups.push_back(std::move(stitching));
    }
    if (!geometry.failedStitchingVias.empty()) {
        SceneGroup failed{QObject::tr("Rejected stitching vias"), QColor(235, 64, 52)};
        const double arm = 0.25 * kUnitsPerMm;
        for (const GeometryPoint& p : geometry.failedStitchingVias) {
            b.line(failed.lines, p.x - arm, p.y - arm, top + lift, p.x + arm, p.y + arm, top + lift, failed.swatch);
            b.line(failed.lines, p.x - arm, p.y + arm, top + lift, p.x + arm, p.y - arm, top + lift, failed.swatch);
        }
        scene.groups.push_back(std::move(failed));
    }

    // Ports: the pad, and a line down (or up) to the reference plane it's measured against.
    if (!context.ports.empty()) {
        // Driven yellow, terminated orange, probed cyan -- none of them a copper layer's color.
        SceneGroup ports{QObject::tr("Ports"), QColor(255, 236, 80)};
        const double plane = context.referencePlane < static_cast<int>(metalZ.size())
                                 ? metalZ[static_cast<std::size_t>(context.referencePlane)]
                                 : bottom;
        for (const GeometryPort& port : context.ports) {
            const QColor color = port.kind == PortKind::Excited ? QColor(255, 236, 80)
                                 : port.kind == PortKind::Load  ? QColor(255, 140, 30)
                                                                : QColor(70, 200, 255);
            const double z = port.metal < static_cast<int>(metalZ.size()) ? metalZ[static_cast<std::size_t>(port.metal)] : top;
            const double side = port.metal == static_cast<int>(metalZ.size()) - 1 && port.metal > 0 ? -lift : lift;
            const double angle = port.orientationDegrees * std::numbers::pi / 180;
            const double c = std::cos(angle), s = std::sin(angle);
            GeometryLoop corners;
            static constexpr int kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& corner : kCorners) {
                const double lx = corner[0] * port.width / 2, ly = corner[1] * port.height / 2;
                corners.push_back({port.x + lx * c - ly * s, port.y + lx * s + ly * c});
            }
            b.triangle(ports.triangles, corners[0], corners[1], corners[2], z + side, color);
            b.triangle(ports.triangles, corners[0], corners[2], corners[3], z + side, color);
            b.line(ports.lines, port.x, port.y, z + side, port.x, port.y, plane, color);
        }
        scene.groups.push_back(std::move(ports));
    }

    // The cut's outline and the mounting holes, on the top and bottom surfaces.
    {
        SceneGroup outline{QObject::tr("Outline"), QColor(200, 200, 200)};
        for (const GeometryLoop& l : geometry.cutoutLoops) {
            if (l.size() < 2) continue;
            b.loop(outline.lines, l, top, outline.swatch);
            b.loop(outline.lines, l, bottom, outline.swatch);
        }
        for (const GeometryLoop& l : geometry.npthHoles) {
            if (l.size() < 2) continue;
            b.loop(outline.lines, l, top, outline.swatch);
            b.loop(outline.lines, l, bottom, outline.swatch);
        }
        scene.groups.push_back(std::move(outline));
    }

    // The solder mask, which hides the copper under it, so it starts hidden.
    const QColor mask(30, 140, 70, 110);
    if (!geometry.topMask.empty()) {
        SceneGroup group{QObject::tr("Top solder mask"), mask, false, true};
        for (const GeometryTriangle& t : geometry.topMask) b.triangle(group.triangles, t[0], t[1], t[2], context.topMaskZ, mask);
        scene.groups.push_back(std::move(group));
    }
    if (!geometry.bottomMask.empty()) {
        SceneGroup group{QObject::tr("Bottom solder mask"), mask, false, true};
        for (const GeometryTriangle& t : geometry.bottomMask) {
            b.triangle(group.triangles, t[0], t[1], t[2], context.bottomMaskZ, mask);
        }
        scene.groups.push_back(std::move(group));
    }

    // The FDTD mesh: its lines on three faces of the domain -- the board's top surface, and the far
    // side and left end -- and the absorbing boundary's inner box.
    if (geometry.gridX.size() >= 2 && geometry.gridY.size() >= 2 && geometry.gridZ.size() >= 2) {
        const auto& gx = geometry.gridX;
        const auto& gy = geometry.gridY;
        const auto& gz = geometry.gridZ;
        SceneGroup mesh{QObject::tr("Mesh"), QColor(80, 200, 255, 90), false, true};
        const QColor lineColor = mesh.swatch;
        const double x0 = gx.front(), x1 = gx.back(), y0 = gy.front(), y1 = gy.back(), z0 = gz.front(), z1 = gz.back();
        for (double x : gx) {
            b.line(mesh.lines, x, y0, top + lift, x, y1, top + lift, lineColor);
            b.line(mesh.lines, x, y1, z0, x, y1, z1, lineColor);
        }
        for (double y : gy) {
            b.line(mesh.lines, x0, y, top + lift, x1, y, top + lift, lineColor);
            b.line(mesh.lines, x0, y, z0, x0, y, z1, lineColor);
        }
        for (double z : gz) {
            b.line(mesh.lines, x0, y1, z, x1, y1, z, lineColor);
            b.line(mesh.lines, x0, y0, z, x0, y1, z, lineColor);
        }
        const QColor boundary(255, 150, 60, 200);
        const double bx[] = {geometry.pmlXMin, geometry.pmlXMax}, by[] = {geometry.pmlYMin, geometry.pmlYMax},
                     bz[] = {geometry.pmlZMin, geometry.pmlZMax};
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                b.line(mesh.lines, bx[0], by[i], bz[j], bx[1], by[i], bz[j], boundary);
                b.line(mesh.lines, bx[i], by[0], bz[j], bx[i], by[1], bz[j], boundary);
                b.line(mesh.lines, bx[i], by[j], bz[0], bx[i], by[j], bz[1], boundary);
            }
        }
        scene.groups.push_back(std::move(mesh));
        const double cells = static_cast<double>(gx.size() - 1) * static_cast<double>(gy.size() - 1) *
                             static_cast<double>(gz.size() - 1);
        scene.gridSummary = QObject::tr("Mesh: %1 x %2 x %3 lines, %4 M cells")
                                .arg(gx.size())
                                .arg(gy.size())
                                .arg(gz.size())
                                .arg(cells / 1e6, 0, 'f', 2);
    }

    const SceneVertex low = b.vertex(geometry.xMin, geometry.yMin, std::min(context.bottomMaskZ, bottom), Qt::white);
    const SceneVertex high = b.vertex(geometry.xMax, geometry.yMax, std::max(context.topMaskZ, top), Qt::white);
    scene.boundsMin = QVector3D(low.x, low.y, low.z);
    scene.boundsMax = QVector3D(high.x, high.y, high.z);
    return scene;
}

} // namespace kiems_gui
