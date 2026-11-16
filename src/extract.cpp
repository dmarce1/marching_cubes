#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "marching_cubes/extract.hpp"

namespace marching_cubes {
namespace {

struct LongVec {
    long double x, y, z;
};

LongVec operator+(LongVec a, LongVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
LongVec operator-(LongVec a, LongVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
LongVec operator*(LongVec a, long double b) { return {a.x * b, a.y * b, a.z * b}; }
LongVec cross_long(LongVec a, LongVec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
long double dot_long(LongVec a, LongVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
LongVec as_long(Vec3 v) { return {v.x, v.y, v.z}; }

// Scaling before conversion protects the normalization from overflow/underflow.
Vec3 unit(LongVec v) {
    long double const scale = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    if (!(scale > 0) || !std::isfinite(scale)) {
        return {0, 0, 0};
    }
    v = v * (1 / scale);
    long double const length = std::sqrt(dot_long(v, v));
    return {static_cast<double>(v.x / length), static_cast<double>(v.y / length),
            static_cast<double>(v.z / length)};
}

struct EdgeKey {
    size_t first, second;
    bool operator==(EdgeKey const &other) const {
        return first == other.first && second == other.second;
    }
};
struct EdgeHash {
    size_t operator()(EdgeKey const &key) const {
        size_t const a = std::hash<size_t>{}(key.first);
        size_t const b = std::hash<size_t>{}(key.second);
        return a ^ (b + size_t{0x9e3779b9} + (a << 6) + (a >> 2));
    }
};

struct OrientationSets {
    std::vector<size_t> parent;
    std::vector<unsigned char> rank;
    // parity[i] is the required flip relative to parent[i].
    std::vector<unsigned char> parity;

    explicit OrientationSets(size_t count) : parent(count), rank(count, 0), parity(count, 0) {
        for (size_t i = 0; i < count; ++i) {
            parent[i] = i;
        }
    }

    std::pair<size_t, unsigned char> find(size_t i) {
        if (parent[i] != i) {
            auto const result = find(parent[i]);
            parity[i] ^= result.second;
            parent[i] = result.first;
        }
        return {parent[i], parity[i]};
    }

    bool constrain(size_t a, size_t b, unsigned char different) {
        auto const left = find(a);
        auto const right = find(b);
        if (left.first == right.first) {
            return (left.second ^ right.second) == different;
        }
        size_t root_a = left.first;
        size_t root_b = right.first;
        unsigned char const root_parity = left.second ^ right.second ^ different;
        if (rank[root_a] > rank[root_b]) {
            std::swap(root_a, root_b);
        }
        parent[root_a] = root_b;
        parity[root_a] = root_parity;
        if (rank[root_a] == rank[root_b]) {
            ++rank[root_b];
        }
        return true;
    }
};

struct OrientedEdge {
    size_t triangle;
    bool forward;
    unsigned int incidence;
};

constexpr std::array<std::array<int, 3>, 8> corners{{
    {{0, 0, 0}}, {{1, 0, 0}}, {{1, 1, 0}}, {{0, 1, 0}},
    {{0, 0, 1}}, {{1, 0, 1}}, {{1, 1, 1}}, {{0, 1, 1}}
}};
constexpr std::array<std::array<int, 2>, 12> edges{{
    {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}, {{4, 5}}, {{5, 6}},
    {{6, 7}}, {{7, 4}}, {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}}
}};
// Opposite sides of adjacent cells use the same global cyclic orientation.
constexpr std::array<std::array<int, 4>, 6> face_corners{{
    {{0, 1, 2, 3}}, {{4, 5, 6, 7}}, {{0, 1, 5, 4}},
    {{3, 2, 6, 7}}, {{0, 3, 7, 4}}, {{1, 2, 6, 5}}
}};
constexpr std::array<std::array<int, 4>, 6> face_edges{{
    {{0, 1, 2, 3}}, {{4, 5, 6, 7}}, {{0, 9, 4, 8}},
    {{2, 10, 6, 11}}, {{3, 11, 7, 8}}, {{1, 10, 5, 9}}
}};

void validate(ScalarGrid const &grid, double iso) {
    if (!std::isfinite(iso)) {
        throw std::invalid_argument("isovalue must be finite");
    }
    size_t count = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        size_t const n = grid.dims[axis];
        double const lo = grid.bounds.lo[axis];
        double const hi = grid.bounds.hi[axis];
        if (n < 2 || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) {
            throw std::invalid_argument("grid dimensions and bounds must be finite and increasing");
        }
        if (count > std::numeric_limits<size_t>::max() / n) {
            throw std::invalid_argument("grid dimensions overflow");
        }
        count *= n;
    }
    if (grid.values.size() != count) {
        throw std::invalid_argument("grid values size does not match dimensions");
    }
    for (double value : grid.values) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("grid values must be finite");
        }
    }
}

struct Extractor {
    ScalarGrid const &grid;
    double iso;
    Mesh mesh;
    std::unordered_map<EdgeKey, uint32_t, EdgeHash> cache;
    std::array<long double, 3> h;

    Extractor(ScalarGrid const &input, double level) : grid(input), iso(level) {
        mesh.isovalue = level;
        for (size_t axis = 0; axis < 3; ++axis) {
            h[axis] = (static_cast<long double>(grid.bounds.hi[axis]) - grid.bounds.lo[axis]) /
                static_cast<long double>(grid.dims[axis] - 1);
        }
    }

    LongVec gradient(std::array<size_t, 3> p) const {
        LongVec g{0, 0, 0};
        long double *components[] = {&g.x, &g.y, &g.z};
        for (size_t axis = 0; axis < 3; ++axis) {
            auto low = p;
            auto high = p;
            if (low[axis] > 0) {
                --low[axis];
            }
            if (high[axis] + 1 < grid.dims[axis]) {
                ++high[axis];
            }
            long double const a = grid.values[grid.index(low[0], low[1], low[2])];
            long double const b = grid.values[grid.index(high[0], high[1], high[2])];
            // Negative gradient points out of the high-valued interior.
            *components[axis] = (a - b) / (h[axis] * (high[axis] - low[axis]));
        }
        return g;
    }

    uint32_t append(Vec3 position, Vec3 normal) {
        if (mesh.positions.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("mesh exceeds uint32_t vertex capacity");
        }
        auto const index = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.push_back(position);
        mesh.normals.push_back(normal);
        return index;
    }

    uint32_t vertex(std::array<size_t, 3> a, std::array<size_t, 3> b) {
        size_t const ia = grid.index(a[0], a[1], a[2]);
        size_t const ib = grid.index(b[0], b[1], b[2]);
        double const va = grid.values[ia];
        double const vb = grid.values[ib];
        EdgeKey key{std::min(ia, ib), std::max(ia, ib)};
        if (va == iso) {
            key = {ia, ia};
        } else if (vb == iso) {
            key = {ib, ib};
        }
        auto const found = cache.find(key);
        if (found != cache.end()) {
            return found->second;
        }
        long double const difference = static_cast<long double>(vb) - va;
        long double const t = (static_cast<long double>(iso) - va) / difference;
        Vec3 position{};
        for (size_t axis = 0; axis < 3; ++axis) {
            long double const coord = static_cast<long double>(grid.bounds.lo[axis]) + h[axis] *
                (static_cast<long double>(a[axis]) + t * (static_cast<long double>(b[axis]) - a[axis]));
            position[axis] = static_cast<double>(coord);
        }
        LongVec const normal = gradient(a) * (1 - t) + gradient(b) * t;
        uint32_t const index = append(position, unit(normal));
        cache.emplace(key, index);
        return index;
    }

    void add_triangle(uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || c == a) {
            return;
        }
        LongVec const ab = as_long(mesh.positions[b]) - as_long(mesh.positions[a]);
        LongVec const ac = as_long(mesh.positions[c]) - as_long(mesh.positions[a]);
        LongVec const n = cross_long(ab, ac);
        if (n.x == 0 && n.y == 0 && n.z == 0) {
            return;
        }
        mesh.triangles.push_back({a, b, c});
    }

    void cap(std::vector<uint32_t> loop) {
        loop.erase(std::unique(loop.begin(), loop.end()), loop.end());
        if (loop.size() > 1 && loop.front() == loop.back()) {
            loop.pop_back();
        }
        if (loop.size() < 3) {
            return;
        }
        LongVec center{0, 0, 0};
        LongVec avg_normal{0, 0, 0};
        for (uint32_t v : loop) {
            center = center + as_long(mesh.positions[v]);
            avg_normal = avg_normal + as_long(mesh.normals[v]);
        }
        center = center * (1.0L / loop.size());
        LongVec area{0, 0, 0};
        for (size_t n = 0; n < loop.size(); ++n) {
            LongVec const a = as_long(mesh.positions[loop[n]]) - center;
            LongVec const b = as_long(mesh.positions[loop[(n + 1) % loop.size()]]) - center;
            area = area + cross_long(a, b);
        }
        if (dot_long(area, avg_normal) < 0) {
            std::reverse(loop.begin(), loop.end());
            area = area * -1;
        }
        if (loop.size() == 3) {
            add_triangle(loop[0], loop[1], loop[2]);
            return;
        }
        Vec3 normal = unit(avg_normal);
        if (normal.x == 0 && normal.y == 0 && normal.z == 0) {
            normal = unit(area);
        }
        uint32_t const middle = append({static_cast<double>(center.x), static_cast<double>(center.y),
                                        static_cast<double>(center.z)}, normal);
        for (size_t n = 0; n < loop.size(); ++n) {
            add_triangle(middle, loop[n], loop[(n + 1) % loop.size()]);
        }
    }

    void cell(size_t i, size_t j, size_t k) {
        std::array<std::array<size_t, 3>, 8> points{};
        std::array<long double, 8> values{};
        for (size_t c = 0; c < corners.size(); ++c) {
            points[c] = {i + size_t(corners[c][0]), j + size_t(corners[c][1]), k + size_t(corners[c][2])};
            values[c] = static_cast<long double>(grid.values[grid.index(points[c][0], points[c][1], points[c][2])]) - iso;
        }
        std::array<bool, 12> crossing{};
        for (size_t e = 0; e < edges.size(); ++e) {
            crossing[e] = (values[edges[e][0]] > 0) != (values[edges[e][1]] > 0);
        }
        std::array<std::array<int, 2>, 12> adjacency{};
        std::array<int, 12> degree{};
        auto connect = [&](int a, int b) {
            if (degree[a] >= 2 || degree[b] >= 2) {
                throw std::logic_error("invalid cell contour connectivity");
            }
            adjacency[a][degree[a]++] = b;
            adjacency[b][degree[b]++] = a;
        };
        for (size_t f = 0; f < face_edges.size(); ++f) {
            std::array<int, 4> active{};
            size_t count = 0;
            for (int edge : face_edges[f]) {
                if (crossing[edge]) {
                    active[count++] = edge;
                }
            }
            if (count == 2) {
                connect(active[0], active[1]);
            } else if (count == 4) {
                auto const &fc = face_corners[f];
                auto const &fe = face_edges[f];
                long double const scale = std::max({std::abs(values[fc[0]]), std::abs(values[fc[1]]),
                                                    std::abs(values[fc[2]]), std::abs(values[fc[3]])});
                long double const determinant = (values[fc[0]] / scale) * (values[fc[2]] / scale) -
                    (values[fc[1]] / scale) * (values[fc[3]] / scale);
                if (determinant >= 0) {
                    connect(fe[0], fe[1]);
                    connect(fe[2], fe[3]);
                } else {
                    connect(fe[3], fe[0]);
                    connect(fe[1], fe[2]);
                }
            }
        }
        std::array<bool, 12> visited{};
        for (size_t start = 0; start < edges.size(); ++start) {
            if (!crossing[start] || visited[start]) {
                continue;
            }
            std::vector<uint32_t> loop;
            int current = static_cast<int>(start);
            int previous = -1;
            do {
                if (degree[current] != 2 || visited[current]) {
                    throw std::logic_error("invalid cell contour loop");
                }
                visited[current] = true;
                auto const &edge = edges[current];
                loop.push_back(vertex(points[edge[0]], points[edge[1]]));
                int const next = adjacency[current][0] == previous ? adjacency[current][1] : adjacency[current][0];
                previous = current;
                current = next;
            } while (current != static_cast<int>(start));
            cap(std::move(loop));
        }
    }

    void repair_zero_normals() {
        auto const missing = [](Vec3 n) { return n.x == 0 && n.y == 0 && n.z == 0; };
        if (!std::any_of(mesh.normals.begin(), mesh.normals.end(), missing)) {
            return;
        }
        std::vector<LongVec> sums(mesh.positions.size(), LongVec{0, 0, 0});
        for (auto const &triangle : mesh.triangles) {
            Vec3 const a = mesh.positions[triangle[0]];
            LongVec const ab = as_long(mesh.positions[triangle[1]]) - as_long(a);
            LongVec const ac = as_long(mesh.positions[triangle[2]]) - as_long(a);
            LongVec const face = as_long(unit(cross_long(ab, ac)));
            for (uint32_t vertex_index : triangle) {
                if (missing(mesh.normals[vertex_index])) {
                    sums[vertex_index] = sums[vertex_index] + face;
                }
            }
        }
        for (size_t i = 0; i < mesh.normals.size(); ++i) {
            if (missing(mesh.normals[i])) {
                mesh.normals[i] = unit(sums[i]);
            }
        }
    }

    void orient_components() {
        OrientationSets sets(mesh.triangles.size());
        std::unordered_map<EdgeKey, OrientedEdge, EdgeHash> edge_owners;
        for (size_t t = 0; t < mesh.triangles.size(); ++t) {
            auto const &triangle = mesh.triangles[t];
            for (size_t e = 0; e < 3; ++e) {
                uint32_t const a = triangle[e];
                uint32_t const b = triangle[(e + 1) % 3];
                EdgeKey const key{std::min(a, b), std::max(a, b)};
                bool const forward = a < b;
                auto const inserted = edge_owners.emplace(key, OrientedEdge{t, forward, 1});
                if (!inserted.second) {
                    OrientedEdge &owner = inserted.first->second;
                    if (++owner.incidence > 2) {
                        throw std::runtime_error("isosurface contains a nonmanifold edge");
                    }
                    unsigned char const different = owner.forward == forward ? 1 : 0;
                    if (!sets.constrain(owner.triangle, t, different)) {
                        throw std::runtime_error("isosurface component has inconsistent orientation");
                    }
                }
            }
        }
        // Aggregate alignment uses the interpolated outward-gradient normals.
        // A zero score leaves the deterministic initial component orientation.
        std::vector<long double> score(mesh.triangles.size(), 0);
        for (size_t t = 0; t < mesh.triangles.size(); ++t) {
            auto const &triangle = mesh.triangles[t];
            auto const component = sets.find(t);
            LongVec const a = as_long(mesh.positions[triangle[0]]);
            LongVec const ab = as_long(mesh.positions[triangle[1]]) - a;
            LongVec const ac = as_long(mesh.positions[triangle[2]]) - a;
            LongVec const normals = as_long(mesh.normals[triangle[0]]) +
                as_long(mesh.normals[triangle[1]]) + as_long(mesh.normals[triangle[2]]);
            long double const alignment = dot_long(cross_long(ab, ac), normals);
            score[component.first] += component.second ? -alignment : alignment;
        }
        for (size_t t = 0; t < mesh.triangles.size(); ++t) {
            auto const component = sets.find(t);
            bool const flip = (component.second != 0) != (score[component.first] < 0);
            if (flip) {
                std::swap(mesh.triangles[t][1], mesh.triangles[t][2]);
            }
        }
    }
};

} // namespace

Mesh extract(ScalarGrid const &grid, double isovalue) {
    validate(grid, isovalue);
    Extractor extractor(grid, isovalue);
    for (size_t k = 0; k + 1 < grid.dims[2]; ++k) {
        for (size_t j = 0; j + 1 < grid.dims[1]; ++j) {
            for (size_t i = 0; i + 1 < grid.dims[0]; ++i) {
                extractor.cell(i, j, k);
            }
        }
    }
    extractor.orient_components();
    extractor.repair_zero_normals();
    return std::move(extractor.mesh);
}

} // namespace marching_cubes
