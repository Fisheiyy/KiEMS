// What the Geometry page draws: one simulation's built geometry, as the pipeline will simulate it.
// The geometry stage (kiems -g) writes ems/geometry/<simulation>/geometry.json -- the board cut to
// the simulation's area (copper per layer, stitching vias, holes, solder mask) and the FDTD mesh's
// grid lines. SimulationGeometry reads it; buildGeometryScene() turns it, with what the board and
// document add (each layer's height in the stackup, its KiCad color, the board's own vias inside
// the cut, and the simulation's ports), into coloured triangles and lines in millimetres, grouped
// the way the page's legend shows and hides them -- the macOS app's GeometryView content.
//
// Coordinates follow the pipeline's: simulation units (0.1 um), X and Y from the board outline's
// lower-left corner with Y up, Z from the board's top surface (0) downwards. The scene is in mm,
// centred on the simulated area in X and Y.
#pragma once

#include "BoardModel.hpp"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <array>
#include <vector>

namespace kiems_gui {

class Document;

struct GeometryPoint {
    double x = 0;
    double y = 0;
};

using GeometryTriangle = std::array<GeometryPoint, 3>;
using GeometryLoop = std::vector<GeometryPoint>;

/// A plated hole: a via or a pad's, round (x2 == x) or a slot from (x, y) to (x2, y2).
struct GeometryVia {
    double x = 0, y = 0, x2 = 0, y2 = 0;
    double drill = 0;
    double ring = 0;
};

/// The contents of one simulation's geometry.json, in simulation units.
struct SimulationGeometry {
    QString error;
    std::vector<std::vector<GeometryTriangle>> layerTriangles; // per metal, F.Cu first
    std::vector<GeometryTriangle> topMask;
    std::vector<GeometryTriangle> bottomMask;
    std::vector<GeometryLoop> cutoutLoops;
    std::vector<GeometryLoop> npthHoles;
    std::vector<GeometryVia> stitchingVias;
    std::vector<GeometryPoint> failedStitchingVias;
    double xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    /// The mesh, once the grid has been generated; empty otherwise.
    std::vector<double> gridX, gridY, gridZ;
    /// The inner faces of the absorbing boundary.
    double pmlXMin = 0, pmlXMax = 0, pmlYMin = 0, pmlYMax = 0, pmlZMin = 0, pmlZMax = 0;

    bool isValid() const { return error.isEmpty(); }
    static SimulationGeometry load(const QString& geometryJsonPath);
};

enum class PortKind { Excited, Load, Probe };

/// Where a simulation's port sits: a pin's pad, on its copper layer, measured against the reference
/// plane.
struct GeometryPort {
    QString name;
    PortKind kind = PortKind::Probe;
    double x = 0, y = 0;            // simulation units, the geometry's frame
    double width = 0, height = 0;   // the pad's, simulation units
    double orientationDegrees = 0;
    int metal = 0;
};

/// What the board and document add to a simulation's geometry.
struct GeometryContext {
    QStringList metalNames;
    std::vector<double> metalZ; // simulation units, top 0 and negative below
    double topMaskZ = 0;        // the mask's outer face; equal to the top metal's when there's none
    double bottomMaskZ = 0;
    std::map<std::string, std::string> layerColors; // layer name -> "#RRGGBB[AA]"
    std::vector<GeometryVia> boardVias;             // every plated hole on the board, in the geometry's frame
    std::vector<GeometryPort> ports;
    int referencePlane = 1;

    /// The context for `simulation` of `document` on `board`.
    static GeometryContext of(const BoardData& board, const Document& document, int simulation);
};

struct SceneVertex {
    float x, y, z;
    float r, g, b, a;
};

/// One legend entry's geometry.
struct SceneGroup {
    QString name;
    QColor swatch;
    bool visibleByDefault = true;
    /// Drawn after the opaque groups, without writing depth, so what's behind shows through.
    bool translucent = false;
    std::vector<SceneVertex> triangles;
    std::vector<SceneVertex> lines;
};

struct GeometryScene {
    std::vector<SceneGroup> groups;
    /// The simulated board's extent, in scene mm, for framing the view.
    QVector3D boundsMin, boundsMax;
    /// The mesh's size ("297 x 338 x 32 lines, 3.1 M cells"); empty without a mesh.
    QString gridSummary;
};

GeometryScene buildGeometryScene(const SimulationGeometry& geometry, const GeometryContext& context);

} // namespace kiems_gui
