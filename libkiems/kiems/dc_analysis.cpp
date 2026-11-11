#include "dc_analysis.hpp"

#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Delaunay_mesh_face_base_2.h>
#include <CGAL/Delaunay_mesh_size_criteria_2.h>
#include <CGAL/Delaunay_mesh_vertex_base_2.h>
#include <CGAL/Delaunay_mesher_2.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>

#include "logging.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <tuple>

namespace kiems::dc {

namespace {

using K = CGAL::Exact_predicates_inexact_constructions_kernel;
using VertexBase = CGAL::Triangulation_vertex_base_with_info_2<int, K, CGAL::Delaunay_mesh_vertex_base_2<K>>;
using FaceBase = CGAL::Delaunay_mesh_face_base_2<K>;
using Tds = CGAL::Triangulation_data_structure_2<VertexBase, FaceBase>;
// Exact_predicates_tag: constraints may cross (a pad's outline over the copper's edge); their
// intersections are computed approximately, which is all a mesh needs.
using CDT = CGAL::Constrained_Delaunay_triangulation_2<K, Tds, CGAL::Exact_predicates_tag>;
using Criteria = CGAL::Delaunay_mesh_size_criteria_2<CDT>;
using Mesher = CGAL::Delaunay_mesher_2<CDT, Criteria>;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
/// The link between a part's pin and the node inside it that draws a shared current: 10 uOhm,
/// stiff enough that the copper alone decides the split.
constexpr double kPackageTie = 1e5;
/// How close to a pad's or ring's outline a node counts as on it, mm.
constexpr double kOnEdge = 1e-6;

// ---------------------------------------------------------------------------------------------
// Geometry helpers

double distanceToSegment(const Cu::Position& p, const Cu::Position& a, const Cu::Position& b) {
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double length2 = dx * dx + dy * dy;
    double t = length2 > 0 ? ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / length2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x() - (a.x() + t * dx), p.y() - (a.y() + t * dy));
}

/// Inside `polygons` (holes excluded) or on one of their outlines.
bool insideOrOn(const Cu::PolygonSet& polygons, const Cu::Position& p) {
    bool inside = false;
    for (const Cu::Polygon& loop : polygons) {
        const std::size_t n = loop.size();
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Cu::Position& a = loop[i];
            const Cu::Position& b = loop[j];
            if (distanceToSegment(p, a, b) <= kOnEdge) return true;
            if ((a.y() > p.y()) != (b.y() > p.y()) &&
                p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()) {
                inside = !inside;
            }
        }
    }
    return inside;
}

Cu::BoundingBox<double> boundsOf(const Cu::PolygonSet& polygons) {
    Cu::BoundingBox<double> box;
    for (const Cu::Polygon& loop : polygons) {
        for (const Cu::Position& p : loop) {
            box.xMin = std::min(box.xMin, p.x());
            box.xMax = std::max(box.xMax, p.x());
            box.yMin = std::min(box.yMin, p.y());
            box.yMax = std::max(box.yMax, p.y());
        }
    }
    return box;
}

Cu::Polygon circle(double x, double y, double radius, int segments) {
    Cu::Polygon loop;
    for (int i = 0; i < segments; ++i) {
        const double a = 2 * std::numbers::pi * i / segments;
        loop.emplace_back(x + radius * std::cos(a), y + radius * std::sin(a));
    }
    return loop;
}

void insertLoop(CDT& cdt, const Cu::Polygon& loop) {
    if (loop.size() < 3) return;
    std::vector<CDT::Vertex_handle> vertices;
    vertices.reserve(loop.size());
    for (const Cu::Position& p : loop) vertices.push_back(cdt.insert(K::Point_2(p.x(), p.y())));
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto a = vertices[i], b = vertices[(i + 1) % vertices.size()];
        if (a != b) cdt.insert_constraint(a, b);
    }
}

// ---------------------------------------------------------------------------------------------
// Meshing

/// The grid outlines are snapped to before meshing, mm. Copper unions leave edges a few nanometres
/// long, and points a nanometre apart where pieces meet; refinement can't split such an edge, and
/// spins forever trying. Nothing finer than a micrometre matters to the current.
constexpr double kSnap = 1e-3;

/// `copper` snapped to kSnap, with repeated points, and points that bend the outline by less than
/// half the grid, removed.
Cu::PolygonSet cleanOutline(const Cu::PolygonSet& copper) {
    Cu::PolygonSet cleaned;
    for (const Cu::Polygon& loop : copper) {
        Cu::Polygon snapped;
        for (const Cu::Position& p : loop) {
            const Cu::Position q(std::round(p.x() / kSnap) * kSnap, std::round(p.y() / kSnap) * kSnap);
            if (snapped.empty() || q.x() != snapped.back().x() || q.y() != snapped.back().y()) snapped.push_back(q);
        }
        while (snapped.size() > 1 && snapped.front().x() == snapped.back().x() && snapped.front().y() == snapped.back().y()) {
            snapped.pop_back();
        }
        // Drop nearly straight-through points until none are left.
        for (bool changed = true; changed && snapped.size() >= 3;) {
            changed = false;
            for (std::size_t i = 0; i < snapped.size() && snapped.size() >= 3; ++i) {
                const Cu::Position& a = snapped[(i + snapped.size() - 1) % snapped.size()];
                const Cu::Position& b = snapped[i];
                const Cu::Position& c = snapped[(i + 1) % snapped.size()];
                if (distanceToSegment(b, a, c) < kSnap / 2) {
                    snapped.erase(snapped.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    --i;
                }
            }
        }
        if (snapped.size() >= 3 && std::abs(Cu::signedArea(snapped)) > kSnap * kSnap) cleaned.push_back(std::move(snapped));
    }
    return cleaned;
}

/// Of `points`, those clear of the copper's outline and of each other. A point (all but) on a
/// constrained edge would make a near-degenerate triangle whose circumcentre the refinement can't
/// compute; the outline's own vertices already give the mesh nodes there.
std::vector<Cu::Position> usablePoints(const Cu::PolygonSet& copper, const std::vector<Cu::Position>& points) {
    constexpr double kClearance = kSnap;
    // The outline's segments, bucketed on a grid for nearby lookups.
    const Cu::BoundingBox<double> box = boundsOf(copper);
    if (!(box.xMax > box.xMin)) return {};
    const double cell = std::max({box.xMax - box.xMin, box.yMax - box.yMin, 1e-3}) / 256;
    const int columns = static_cast<int>((box.xMax - box.xMin) / cell) + 1;
    const int rows = static_cast<int>((box.yMax - box.yMin) / cell) + 1;
    std::vector<std::vector<std::pair<Cu::Position, Cu::Position>>> buckets(static_cast<std::size_t>(columns) *
                                                                            static_cast<std::size_t>(rows));
    const auto column = [&](double x) { return std::clamp(static_cast<int>((x - box.xMin) / cell), 0, columns - 1); };
    const auto row = [&](double y) { return std::clamp(static_cast<int>((y - box.yMin) / cell), 0, rows - 1); };
    for (const Cu::Polygon& loop : copper) {
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const Cu::Position& a = loop[i];
            const Cu::Position& b = loop[(i + 1) % loop.size()];
            for (int c = column(std::min(a.x(), b.x()) - kClearance); c <= column(std::max(a.x(), b.x()) + kClearance); ++c) {
                for (int r = row(std::min(a.y(), b.y()) - kClearance); r <= row(std::max(a.y(), b.y()) + kClearance); ++r) {
                    buckets[static_cast<std::size_t>(r) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(c)]
                        .emplace_back(a, b);
                }
            }
        }
    }
    std::vector<Cu::Position> usable;
    std::set<std::pair<long long, long long>> taken;
    for (const Cu::Position& p : points) {
        if (p.x() < box.xMin || p.x() > box.xMax || p.y() < box.yMin || p.y() > box.yMax) continue;
        const auto key = std::pair{std::llround(p.x() / kClearance), std::llround(p.y() / kClearance)};
        if (taken.contains(key)) continue;
        const auto& nearby = buckets[static_cast<std::size_t>(row(p.y())) * static_cast<std::size_t>(columns) +
                                     static_cast<std::size_t>(column(p.x()))];
        if (std::any_of(nearby.begin(), nearby.end(),
                        [&](const auto& segment) { return distanceToSegment(p, segment.first, segment.second) < kClearance; })) {
            continue;
        }
        taken.insert(key);
        usable.push_back(p);
    }
    return usable;
}

struct LayerMesh {
    std::vector<Cu::Position> nodes;
    std::vector<std::array<int, 3>> triangles;
};

/// Meshes `copper`, with `points` (on pads and annular rings) as vertices, so every pad and ring has
/// nodes to tie together. They're points rather than constrained outlines: a pad's outline mostly
/// runs along the copper's own, and nearly coincident constraints can keep the refinement from ever
/// finishing. Fails if the mesh would pass `maxVertices`.
std::expected<LayerMesh, std::string> meshLayer(const Cu::PolygonSet& copper, const std::vector<Cu::Position>& points,
                                                double maxElement, std::size_t maxVertices) {
    CDT cdt;
    for (const Cu::Polygon& loop : copper) insertLoop(cdt, loop);
    for (const Cu::Position& p : usablePoints(copper, points)) cdt.insert(K::Point_2(p.x(), p.y()));

    // The domain is the copper: faces whose centre lies in it.
    const Cu::PointLocator locator(copper);
    for (auto face = cdt.finite_faces_begin(); face != cdt.finite_faces_end(); ++face) {
        const K::Point_2 c = CGAL::centroid(face->vertex(0)->point(), face->vertex(1)->point(), face->vertex(2)->point());
        face->set_in_domain(locator.contains(Cu::Position(c.x(), c.y())));
    }
    // A minimum angle of about 20.7 degrees, and no edge longer than maxElement: narrow copper
    // gets small elements from its own outline, pours get large ones.
    Mesher mesher(cdt, Criteria(0.125, maxElement));
    mesher.init(true);
    for (std::size_t step = 0; !mesher.is_refinement_done(); ++step) {
        mesher.try_one_step_refine_mesh();
        if (step % 4096 == 0 && cdt.number_of_vertices() > maxVertices) {
            return std::unexpected("its mesh grew past " + std::to_string(maxVertices) + " nodes");
        }
    }

    LayerMesh mesh;
    for (auto vertex = cdt.finite_vertices_begin(); vertex != cdt.finite_vertices_end(); ++vertex) vertex->info() = -1;
    for (auto face = cdt.finite_faces_begin(); face != cdt.finite_faces_end(); ++face) {
        if (!face->is_in_domain()) continue;
        std::array<int, 3> triangle{};
        for (int k = 0; k < 3; ++k) {
            const auto vertex = face->vertex(k);
            if (vertex->info() < 0) {
                vertex->info() = static_cast<int>(mesh.nodes.size());
                mesh.nodes.emplace_back(vertex->point().x(), vertex->point().y());
            }
            triangle[k] = vertex->info();
        }
        mesh.triangles.push_back(triangle);
    }
    return mesh;
}

// ---------------------------------------------------------------------------------------------
// Sparse symmetric system and its solver

struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(std::size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int i) {
        while (parent[static_cast<std::size_t>(i)] != i) {
            parent[static_cast<std::size_t>(i)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(i)])];
            i = parent[static_cast<std::size_t>(i)];
        }
        return i;
    }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
    }
};

/// A symmetric matrix in compressed rows, both triangles stored, columns sorted.
struct Csr {
    int n = 0;
    std::vector<int> start, column;
    std::vector<double> value;

    /// Sums repeated (row, column) entries.
    static Csr build(int n, std::vector<std::tuple<int, int, double>>& entries) {
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b) : std::get<1>(a) < std::get<1>(b);
        });
        Csr m;
        m.n = n;
        m.start.assign(static_cast<std::size_t>(n) + 1, 0);
        int lastRow = -1, lastColumn = -1;
        for (const auto& [row, col, v] : entries) {
            if (row == lastRow && col == lastColumn) {
                m.value.back() += v;
                continue;
            }
            m.column.push_back(col);
            m.value.push_back(v);
            ++m.start[static_cast<std::size_t>(row) + 1];
            lastRow = row;
            lastColumn = col;
        }
        for (std::size_t i = 1; i < m.start.size(); ++i) m.start[i] += m.start[i - 1];
        return m;
    }

    void multiply(const std::vector<double>& x, std::vector<double>& y) const {
        for (int i = 0; i < n; ++i) {
            double sum = 0;
            for (int k = start[static_cast<std::size_t>(i)]; k < start[static_cast<std::size_t>(i) + 1]; ++k) {
                sum += value[static_cast<std::size_t>(k)] * x[static_cast<std::size_t>(column[static_cast<std::size_t>(k)])];
            }
            y[static_cast<std::size_t>(i)] = sum;
        }
    }
};

/// Incomplete Cholesky with no fill (IC(0)): L on the lower triangle's pattern, so L L^T ~ A.
struct IncompleteCholesky {
    int n = 0;
    std::vector<int> start, column; // rows of L, columns ascending, the diagonal last
    std::vector<double> value;

    /// Fails (false) if a pivot isn't positive; the caller then shifts the diagonal and retries.
    bool factor(const Csr& a, double shift) {
        n = a.n;
        start.assign(static_cast<std::size_t>(n) + 1, 0);
        column.clear();
        value.clear();
        for (int i = 0; i < n; ++i) {
            for (int k = a.start[static_cast<std::size_t>(i)]; k < a.start[static_cast<std::size_t>(i) + 1]; ++k) {
                const int j = a.column[static_cast<std::size_t>(k)];
                if (j > i) break;
                column.push_back(j);
                value.push_back(j == i ? a.value[static_cast<std::size_t>(k)] * (1 + shift) : a.value[static_cast<std::size_t>(k)]);
            }
            start[static_cast<std::size_t>(i) + 1] = static_cast<int>(column.size());
        }
        for (int i = 0; i < n; ++i) {
            const int rowStart = start[static_cast<std::size_t>(i)], rowEnd = start[static_cast<std::size_t>(i) + 1];
            for (int p = rowStart; p < rowEnd; ++p) {
                const int k = column[static_cast<std::size_t>(p)];
                // The dot product of rows i and k of L, over the columns before k.
                double sum = 0;
                int q = rowStart, r = start[static_cast<std::size_t>(k)];
                const int kEnd = start[static_cast<std::size_t>(k) + 1] - 1; // row k without its diagonal
                while (q < p && r < kEnd) {
                    const int cq = column[static_cast<std::size_t>(q)], cr = column[static_cast<std::size_t>(r)];
                    if (cq == cr) {
                        sum += value[static_cast<std::size_t>(q)] * value[static_cast<std::size_t>(r)];
                        ++q;
                        ++r;
                    } else if (cq < cr) {
                        ++q;
                    } else {
                        ++r;
                    }
                }
                if (k < i) {
                    value[static_cast<std::size_t>(p)] =
                        (value[static_cast<std::size_t>(p)] - sum) / value[static_cast<std::size_t>(start[static_cast<std::size_t>(k) + 1] - 1)];
                } else {
                    const double pivot = value[static_cast<std::size_t>(p)] - sum;
                    if (!(pivot > 0)) return false;
                    value[static_cast<std::size_t>(p)] = std::sqrt(pivot);
                }
            }
        }
        return true;
    }

    /// z = (L L^T)^-1 r.
    void apply(const std::vector<double>& r, std::vector<double>& z) const {
        z = r;
        for (int i = 0; i < n; ++i) {
            const int end = start[static_cast<std::size_t>(i) + 1] - 1;
            double sum = z[static_cast<std::size_t>(i)];
            for (int p = start[static_cast<std::size_t>(i)]; p < end; ++p) {
                sum -= value[static_cast<std::size_t>(p)] * z[static_cast<std::size_t>(column[static_cast<std::size_t>(p)])];
            }
            z[static_cast<std::size_t>(i)] = sum / value[static_cast<std::size_t>(end)];
        }
        for (int i = n - 1; i >= 0; --i) {
            const int end = start[static_cast<std::size_t>(i) + 1] - 1;
            z[static_cast<std::size_t>(i)] /= value[static_cast<std::size_t>(end)];
            const double zi = z[static_cast<std::size_t>(i)];
            for (int p = start[static_cast<std::size_t>(i)]; p < end; ++p) {
                z[static_cast<std::size_t>(column[static_cast<std::size_t>(p)])] -= value[static_cast<std::size_t>(p)] * zi;
            }
        }
    }
};

double dot(const std::vector<double>& a, const std::vector<double>& b) {
    double sum = 0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += a[i] * b[i];
    return sum;
}

/// Solves A x = b for a symmetric positive definite A by IC(0)-preconditioned conjugate gradients.
/// Returns the iterations taken, or -1 if it didn't converge.
int conjugateGradients(const Csr& a, const std::vector<double>& b, std::vector<double>& x) {
    const std::size_t n = static_cast<std::size_t>(a.n);
    x.assign(n, 0.0);
    if (n == 0) return 0;
    IncompleteCholesky preconditioner;
    bool factored = preconditioner.factor(a, 0);
    for (double shift = 1e-4; !factored && shift < 10; shift *= 4) factored = preconditioner.factor(a, shift);
    if (!factored) return -1;
    std::vector<double> r = b, z(n), p(n), q(n);
    preconditioner.apply(r, z);
    p = z;
    double rz = dot(r, z);
    const double target = 1e-11 * std::sqrt(dot(b, b));
    const int limit = std::max(1000, static_cast<int>(4 * std::sqrt(static_cast<double>(n))) * 20);
    for (int iteration = 1; iteration <= limit; ++iteration) {
        a.multiply(p, q);
        const double alpha = rz / dot(p, q);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * q[i];
        }
        if (std::sqrt(dot(r, r)) <= target) return iteration;
        preconditioner.apply(r, z);
        const double rzNext = dot(r, z);
        const double beta = rzNext / rz;
        rz = rzNext;
        for (std::size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    }
    return -1;
}

} // namespace

double barrelResistance(double resistivity, double lengthMm, double drillMm, double platingMm) {
    const double length = lengthMm * 1e-3, drill = drillMm * 1e-3, plating = platingMm * 1e-3;
    return resistivity * length / (std::numbers::pi * (drill + plating) * plating);
}

std::expected<Result, std::string> solve(const Problem& problem) {
    const Options& options = problem.options;
    const double rho = options.resistivity * (1 + 0.00393 * (options.temperatureC - 20));
    const std::size_t layerCount = problem.layers.size();
    if (layerCount == 0) return std::unexpected("There's no copper to analyse.");

    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); };
    // --- Mesh each layer, with its pads and annular rings on element edges.
    std::vector<Cu::PolygonSet> copper(layerCount);
    std::vector<LayerMesh> meshes(layerCount);
    std::vector<int> nodeOffset(layerCount + 1, 0);
    for (std::size_t l = 0; l < layerCount; ++l) {
        copper[l] = cleanOutline(Cu::unionPolygons(problem.layers[l].copper));
        // A node at each pad's corners and centre, and around each ring.
        std::vector<Cu::Position> extra;
        for (const Terminal& terminal : problem.terminals) {
            if (terminal.layer != static_cast<int>(l)) continue;
            const Cu::BoundingBox<double> box = boundsOf(terminal.pad);
            extra.emplace_back((box.xMin + box.xMax) / 2, (box.yMin + box.yMax) / 2);
            // Along its outline closely enough that the mesh follows it.
            const double spacing = std::min(options.maxElementMm, std::max(box.xMax - box.xMin, box.yMax - box.yMin)) / 4;
            for (const Cu::Polygon& loop : terminal.pad) {
                for (std::size_t i = 0; i < loop.size(); ++i) {
                    const Cu::Position& a = loop[i];
                    const Cu::Position& b = loop[(i + 1) % loop.size()];
                    const int pieces = std::max(1, static_cast<int>(std::ceil(std::hypot(b.x() - a.x(), b.y() - a.y()) / spacing)));
                    for (int k = 0; k < pieces; ++k) {
                        const double t = static_cast<double>(k) / pieces;
                        extra.emplace_back(a.x() + t * (b.x() - a.x()), a.y() + t * (b.y() - a.y()));
                    }
                }
            }
        }
        for (const Barrel& barrel : problem.barrels) {
            if (static_cast<int>(l) < barrel.firstLayer || static_cast<int>(l) > barrel.lastLayer) continue;
            const Cu::Polygon ring = circle(barrel.x, barrel.y, std::max(barrel.ringMm, barrel.drillMm) / 2 * 0.9, 8);
            extra.insert(extra.end(), ring.begin(), ring.end());
        }
        if (!copper[l].empty()) {
            const auto started = Clock::now();
            std::size_t vertices = 0;
            for (const Cu::Polygon& loop : copper[l]) vertices += loop.size();
            Cu::logInfo("DC: meshing " + problem.layers[l].name + ": " + std::to_string(copper[l].size()) + " loops, " +
                        std::to_string(vertices) + " vertices, " + std::to_string(extra.size()) + " pad and ring points");
            std::expected<LayerMesh, std::string> mesh;
            try {
                mesh = meshLayer(copper[l], extra, options.maxElementMm, 3'000'000);
            } catch (const std::exception& exception) {
                mesh = std::unexpected(std::string(exception.what()));
            }
            if (!mesh) return std::unexpected(problem.layers[l].name + " couldn't be meshed: " + mesh.error());
            meshes[l] = std::move(*mesh);
            double area = 0;
            for (const auto& t : meshes[l].triangles) {
                const Cu::Position &p0 = meshes[l].nodes[static_cast<std::size_t>(t[0])], &p1 = meshes[l].nodes[static_cast<std::size_t>(t[1])],
                                   &p2 = meshes[l].nodes[static_cast<std::size_t>(t[2])];
                area += 0.5 * std::abs((p1.x() - p0.x()) * (p2.y() - p0.y()) - (p2.x() - p0.x()) * (p1.y() - p0.y()));
            }
            Cu::logInfo("DC: meshed " + problem.layers[l].name + ": " + std::to_string(meshes[l].nodes.size()) + " nodes, " +
                        std::to_string(meshes[l].triangles.size()) + " elements, " + std::to_string(area) + " mm^2 of copper, in " +
                        std::to_string(seconds(started)) + " s");
        }
        nodeOffset[l + 1] = nodeOffset[l] + static_cast<int>(meshes[l].nodes.size());
    }
    const int nodeCount = nodeOffset[layerCount];
    if (nodeCount == 0) return std::unexpected("There's no copper to analyse.");

    // --- Equipotential pads and rings: their nodes share one unknown.
    UnionFind merged(static_cast<std::size_t>(nodeCount));
    const auto nodesIn = [&](std::size_t layer, const Cu::PolygonSet& region) {
        std::vector<int> found;
        const Cu::BoundingBox<double> box = boundsOf(region);
        const LayerMesh& mesh = meshes[layer];
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            const Cu::Position& p = mesh.nodes[i];
            if (p.x() < box.xMin - kOnEdge || p.x() > box.xMax + kOnEdge || p.y() < box.yMin - kOnEdge ||
                p.y() > box.yMax + kOnEdge) {
                continue;
            }
            if (insideOrOn(region, p)) found.push_back(nodeOffset[layer] + static_cast<int>(i));
        }
        return found;
    };
    // A source's pad is held at its voltage throughout. A load's is one node too, unless loads draw
    // their current spread over the pad's copper instead.
    std::vector<int> terminalNode(problem.terminals.size(), -1);
    std::vector<std::vector<int>> terminalNodes(problem.terminals.size());
    for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
        const Terminal& terminal = problem.terminals[t];
        if (terminal.layer < 0 || terminal.layer >= static_cast<int>(layerCount)) continue;
        terminalNodes[t] = nodesIn(static_cast<std::size_t>(terminal.layer), terminal.pad);
        const std::vector<int>& nodes = terminalNodes[t];
        if (terminal.kind == Terminal::Kind::Source || options.equipotentialLoadPads || terminal.package >= 0) {
            for (int node : nodes) merged.unite(nodes.front(), node);
        }
        if (!nodes.empty()) terminalNode[t] = nodes.front();
    }
    // Each barrel's node on each layer it reaches copper on, top to bottom.
    std::vector<std::vector<std::pair<int, int>>> barrelNodes(problem.barrels.size()); // (layer, node)
    for (std::size_t b = 0; b < problem.barrels.size(); ++b) {
        const Barrel& barrel = problem.barrels[b];
        const Cu::PolygonSet ring{circle(barrel.x, barrel.y, std::max(barrel.ringMm, barrel.drillMm) / 2, 16)};
        for (int l = std::max(0, barrel.firstLayer); l <= std::min(barrel.lastLayer, static_cast<int>(layerCount) - 1); ++l) {
            const std::vector<int> nodes = nodesIn(static_cast<std::size_t>(l), ring);
            for (int node : nodes) merged.unite(nodes.front(), node);
            if (!nodes.empty()) barrelNodes[b].emplace_back(l, nodes.front());
        }
    }
    std::vector<int> dof(static_cast<std::size_t>(nodeCount));
    std::map<int, int> rootDof;
    for (int i = 0; i < nodeCount; ++i) {
        const int root = merged.find(i);
        const auto [it, added] = rootDof.emplace(root, static_cast<int>(rootDof.size()));
        dof[static_cast<std::size_t>(i)] = it->second;
    }
    // A node inside each part whose pins share its current.
    int partNodes = static_cast<int>(rootDof.size());
    std::map<int, int> packageDof;
    for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
        const Terminal& terminal = problem.terminals[t];
        if (terminal.kind != Terminal::Kind::Load || terminal.package < 0 || terminalNode[t] < 0) continue;
        if (!packageDof.contains(terminal.package)) packageDof[terminal.package] = partNodes++;
    }
    const int dofCount = partNodes;

    // --- Assemble: each triangle's linear-element conductances, and each barrel span's resistor.
    std::vector<std::tuple<int, int, double>> entries;
    const auto couple = [&](int a, int b, double g) {
        if (a == b || g == 0) return;
        entries.emplace_back(a, a, g);
        entries.emplace_back(b, b, g);
        entries.emplace_back(a, b, -g);
        entries.emplace_back(b, a, -g);
    };
    for (std::size_t l = 0; l < layerCount; ++l) {
        const double sheet = problem.layers[l].thicknessMm * 1e-3 / rho; // siemens per square
        const LayerMesh& mesh = meshes[l];
        for (const auto& t : mesh.triangles) {
            const Cu::Position &p0 = mesh.nodes[static_cast<std::size_t>(t[0])], &p1 = mesh.nodes[static_cast<std::size_t>(t[1])],
                               &p2 = mesh.nodes[static_cast<std::size_t>(t[2])];
            const double b[3] = {p1.y() - p2.y(), p2.y() - p0.y(), p0.y() - p1.y()};
            const double c[3] = {p2.x() - p1.x(), p0.x() - p2.x(), p1.x() - p0.x()};
            const double area = 0.5 * std::abs(b[0] * c[1] - b[1] * c[0]);
            if (!(area > 1e-14)) continue; // a sliver CGAL left on a constraint: nothing flows through it
            for (int i = 0; i < 3; ++i) {
                for (int j = i + 1; j < 3; ++j) {
                    // K_ij = sheet (b_i b_j + c_i c_j) / (4 A); a conductance of -K_ij between them.
                    const double kij = sheet * (b[i] * b[j] + c[i] * c[j]) / (4 * area);
                    couple(dof[static_cast<std::size_t>(nodeOffset[l] + t[i])], dof[static_cast<std::size_t>(nodeOffset[l] + t[j])], -kij);
                }
            }
        }
    }
    struct Span {
        std::size_t barrel;
        int a, b;
        double resistance;
    };
    std::vector<Span> spans;
    Result result;
    result.barrels.resize(problem.barrels.size());
    for (std::size_t b = 0; b < problem.barrels.size(); ++b) {
        const Barrel& barrel = problem.barrels[b];
        BarrelResult& out = result.barrels[b];
        out.name = barrel.name;
        const auto& nodes = barrelNodes[b];
        out.connected = nodes.size() >= 2;
        for (std::size_t k = 0; k + 1 < nodes.size(); ++k) {
            const double length = std::abs(problem.layers[static_cast<std::size_t>(nodes[k].first)].zMm -
                                           problem.layers[static_cast<std::size_t>(nodes[k + 1].first)].zMm);
            const double r = barrelResistance(rho, length, barrel.drillMm, options.platingMm);
            const int a = dof[static_cast<std::size_t>(nodes[k].second)], c = dof[static_cast<std::size_t>(nodes[k + 1].second)];
            out.resistance += r;
            if (a != c && r > 0) {
                couple(a, c, 1 / r);
                spans.push_back({b, a, c, r});
            }
        }
    }
    // Each shared pin's link to its part's node.
    const auto packaged = [&](std::size_t t) {
        return problem.terminals[t].kind == Terminal::Kind::Load && problem.terminals[t].package >= 0 && terminalNode[t] >= 0;
    };
    for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
        if (!packaged(t)) continue;
        couple(dof[static_cast<std::size_t>(terminalNode[t])], packageDof.at(problem.terminals[t].package), kPackageTie);
    }
    Csr full = Csr::build(dofCount, entries);

    // --- Sources fix their unknowns; loads draw current from theirs.
    std::vector<double> fixed(static_cast<std::size_t>(dofCount), kNaN);
    std::vector<double> drawn(static_cast<std::size_t>(dofCount), 0.0);
    result.terminals.resize(problem.terminals.size());
    for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
        const Terminal& terminal = problem.terminals[t];
        TerminalResult& out = result.terminals[t];
        out.name = terminal.name;
        out.kind = terminal.kind;
        if (terminalNode[t] < 0) {
            result.warnings.push_back(terminal.name + "'s pad isn't on the net's copper.");
            continue;
        }
        const std::size_t d = static_cast<std::size_t>(dof[static_cast<std::size_t>(terminalNode[t])]);
        if (terminal.kind == Terminal::Kind::Source) {
            if (!std::isnan(fixed[d]) && fixed[d] != terminal.value) {
                result.warnings.push_back(terminal.name + " is joined to another source at a different voltage; it's held at " +
                                          std::to_string(fixed[d]) + " V.");
            } else {
                fixed[d] = terminal.value;
            }
        } else if (terminal.package >= 0) {
            drawn[static_cast<std::size_t>(packageDof.at(terminal.package))] += terminal.value;
        } else {
            const auto& nodes = terminalNodes[t];
            for (int node : nodes) {
                drawn[static_cast<std::size_t>(dof[static_cast<std::size_t>(node)])] += terminal.value / static_cast<double>(nodes.size());
            }
        }
    }

    // --- Copper with no source can't be solved: find what each source reaches.
    UnionFind reach(static_cast<std::size_t>(dofCount));
    for (int i = 0; i < dofCount; ++i) {
        for (int k = full.start[static_cast<std::size_t>(i)]; k < full.start[static_cast<std::size_t>(i) + 1]; ++k) {
            if (full.value[static_cast<std::size_t>(k)] != 0) reach.unite(i, full.column[static_cast<std::size_t>(k)]);
        }
    }
    std::set<int> sourced;
    for (int i = 0; i < dofCount; ++i) {
        if (!std::isnan(fixed[static_cast<std::size_t>(i)])) sourced.insert(reach.find(i));
    }
    if (sourced.empty()) return std::unexpected("No source is on the net's copper, so there's nothing to drive it.");
    const auto solvable = [&](int d) { return sourced.contains(reach.find(d)); };
    for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
        if (terminalNode[t] < 0) continue;
        const int d = dof[static_cast<std::size_t>(terminalNode[t])];
        result.terminals[t].connected = solvable(d);
        if (!result.terminals[t].connected) {
            result.warnings.push_back(problem.terminals[t].name + " isn't connected to a source through the net's copper.");
        }
    }

    // --- Solve for the free unknowns: A_ff x = -drawn_f - A_fd v_d.
    std::vector<int> freeIndex(static_cast<std::size_t>(dofCount), -1);
    std::vector<int> freeDofs;
    for (int i = 0; i < dofCount; ++i) {
        if (solvable(i) && std::isnan(fixed[static_cast<std::size_t>(i)])) {
            freeIndex[static_cast<std::size_t>(i)] = static_cast<int>(freeDofs.size());
            freeDofs.push_back(i);
        }
    }
    std::vector<std::tuple<int, int, double>> freeEntries;
    std::vector<double> rhs(freeDofs.size(), 0.0);
    for (std::size_t f = 0; f < freeDofs.size(); ++f) {
        const int i = freeDofs[f];
        rhs[f] = -drawn[static_cast<std::size_t>(i)];
        for (int k = full.start[static_cast<std::size_t>(i)]; k < full.start[static_cast<std::size_t>(i) + 1]; ++k) {
            const int j = full.column[static_cast<std::size_t>(k)];
            const double v = full.value[static_cast<std::size_t>(k)];
            if (freeIndex[static_cast<std::size_t>(j)] >= 0) freeEntries.emplace_back(static_cast<int>(f), freeIndex[static_cast<std::size_t>(j)], v);
            else if (!std::isnan(fixed[static_cast<std::size_t>(j)])) rhs[f] -= v * fixed[static_cast<std::size_t>(j)];
        }
    }
    const Csr reduced = Csr::build(static_cast<int>(freeDofs.size()), freeEntries);
    std::vector<double> x;
    const auto solveStarted = Clock::now();
    result.iterations = conjugateGradients(reduced, rhs, x);
    Cu::logInfo("DC: solved " + std::to_string(freeDofs.size()) + " unknowns in " + std::to_string(result.iterations) +
                " iterations, " + std::to_string(seconds(solveStarted)) + " s");
    result.unknowns = static_cast<int>(freeDofs.size());
    if (result.iterations < 0) return std::unexpected("The solver didn't converge.");
    std::vector<double> v(static_cast<std::size_t>(dofCount), kNaN);
    for (int i = 0; i < dofCount; ++i) {
        if (!solvable(i)) continue;
        v[static_cast<std::size_t>(i)] =
            std::isnan(fixed[static_cast<std::size_t>(i)]) ? x[static_cast<std::size_t>(freeIndex[static_cast<std::size_t>(i)])] : fixed[static_cast<std::size_t>(i)];
    }

    // --- What the solution means: node voltages, element current densities, terminal and barrel
    // currents, and the power lost.
    std::vector<double> flow(static_cast<std::size_t>(dofCount), 0.0); // A v: current into the copper at each unknown
    for (int i = 0; i < dofCount; ++i) {
        if (!solvable(i)) continue;
        double sum = 0;
        for (int k = full.start[static_cast<std::size_t>(i)]; k < full.start[static_cast<std::size_t>(i) + 1]; ++k) {
            sum += full.value[static_cast<std::size_t>(k)] * v[static_cast<std::size_t>(full.column[static_cast<std::size_t>(k)])];
        }
        flow[static_cast<std::size_t>(i)] = sum;
        result.powerLoss += sum * v[static_cast<std::size_t>(i)];
    }
    for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
        TerminalResult& out = result.terminals[t];
        out.voltage = kNaN;
        if (terminalNode[t] < 0 || !out.connected) continue;
        const std::size_t d = static_cast<std::size_t>(dof[static_cast<std::size_t>(terminalNode[t])]);
        // The pad's average: one value when it's a single node.
        double sum = 0;
        for (int node : terminalNodes[t]) sum += v[static_cast<std::size_t>(dof[static_cast<std::size_t>(node)])];
        out.voltage = sum / static_cast<double>(terminalNodes[t].size());
        out.current = problem.terminals[t].kind == Terminal::Kind::Source ? flow[d] : problem.terminals[t].value;
        // A shared pin's share: what flows through its link into the part.
        if (packaged(t)) {
            out.current = kPackageTie * (v[d] - v[static_cast<std::size_t>(packageDof.at(problem.terminals[t].package))]);
        }
    }
    // Sources sharing a node share its current equally.
    {
        std::map<std::size_t, int> sourcesAt;
        for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
            if (problem.terminals[t].kind == Terminal::Kind::Source && terminalNode[t] >= 0) {
                ++sourcesAt[static_cast<std::size_t>(dof[static_cast<std::size_t>(terminalNode[t])])];
            }
        }
        for (std::size_t t = 0; t < problem.terminals.size(); ++t) {
            if (problem.terminals[t].kind != Terminal::Kind::Source || terminalNode[t] < 0) continue;
            result.terminals[t].current /= sourcesAt[static_cast<std::size_t>(dof[static_cast<std::size_t>(terminalNode[t])])];
        }
    }
    for (const Span& span : spans) {
        const double current = std::abs(v[static_cast<std::size_t>(span.a)] - v[static_cast<std::size_t>(span.b)]) / span.resistance;
        if (!std::isnan(current)) result.barrels[span.barrel].current = std::max(result.barrels[span.barrel].current, current);
    }
    result.layers.resize(layerCount);
    for (std::size_t l = 0; l < layerCount; ++l) {
        LayerResult& out = result.layers[l];
        const LayerMesh& mesh = meshes[l];
        out.name = problem.layers[l].name;
        out.nodes = mesh.nodes;
        out.triangles = mesh.triangles;
        out.voltage.resize(mesh.nodes.size());
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            out.voltage[i] = v[static_cast<std::size_t>(dof[static_cast<std::size_t>(nodeOffset[l] + static_cast<int>(i))])];
        }
        out.currentDensity.resize(mesh.triangles.size());
        for (std::size_t e = 0; e < mesh.triangles.size(); ++e) {
            const auto& t = mesh.triangles[e];
            const Cu::Position &p0 = mesh.nodes[static_cast<std::size_t>(t[0])], &p1 = mesh.nodes[static_cast<std::size_t>(t[1])],
                               &p2 = mesh.nodes[static_cast<std::size_t>(t[2])];
            const double b[3] = {p1.y() - p2.y(), p2.y() - p0.y(), p0.y() - p1.y()};
            const double c[3] = {p2.x() - p1.x(), p0.x() - p2.x(), p1.x() - p0.x()};
            const double twiceArea = b[0] * c[1] - b[1] * c[0];
            double gx = 0, gy = 0;
            for (int k = 0; k < 3; ++k) {
                const double vk = out.voltage[static_cast<std::size_t>(t[k])];
                gx += vk * b[k];
                gy += vk * c[k];
            }
            // grad V in V/mm; J = -grad V / rho, in A/mm^2 (V/mm -> V/m is 1e3, A/m^2 -> A/mm^2 is 1e-6).
            const double scale = twiceArea != 0 ? -1e-3 / (rho * twiceArea) : kNaN;
            out.currentDensity[e] = {gx * scale, gy * scale};
            const double magnitude = std::hypot(gx * scale, gy * scale);
            if (!std::isnan(magnitude)) out.maxCurrentDensity = std::max(out.maxCurrentDensity, magnitude);
        }
    }
    return result;
}

void to_json(nlohmann::json& j, const Result& result) {
    const auto number = [](double value) { return std::isnan(value) ? nlohmann::json() : nlohmann::json(value); };
    j = nlohmann::json::object();
    j["power_loss"] = result.powerLoss;
    j["unknowns"] = result.unknowns;
    j["iterations"] = result.iterations;
    j["warnings"] = result.warnings;
    auto& terminals = j["terminals"] = nlohmann::json::array();
    for (const TerminalResult& t : result.terminals) {
        terminals.push_back({{"name", t.name},
                             {"kind", t.kind == Terminal::Kind::Source ? "source" : "load"},
                             {"voltage", number(t.voltage)},
                             {"current", t.current},
                             {"connected", t.connected}});
    }
    auto& barrels = j["barrels"] = nlohmann::json::array();
    for (const BarrelResult& b : result.barrels) {
        if (!b.connected) continue;
        barrels.push_back({{"name", b.name}, {"current", b.current}, {"resistance", b.resistance}});
    }
    auto& layers = j["layers"] = nlohmann::json::array();
    for (const LayerResult& l : result.layers) {
        double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
        for (double value : l.voltage) {
            if (std::isnan(value)) continue;
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        layers.push_back({{"name", l.name},
                          {"nodes", l.nodes.size()},
                          {"triangles", l.triangles.size()},
                          {"min_voltage", std::isfinite(minimum) ? nlohmann::json(minimum) : nlohmann::json()},
                          {"max_voltage", std::isfinite(maximum) ? nlohmann::json(maximum) : nlohmann::json()},
                          {"max_current_density", l.maxCurrentDensity}});
    }
}

} // namespace kiems::dc
