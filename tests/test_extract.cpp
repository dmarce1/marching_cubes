#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "marching_cubes/extract.hpp"

namespace {
using marching_cubes::Mesh;
using marching_cubes::ScalarGrid;
using marching_cubes::Vec3;

void check(bool condition, char const *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Function>
ScalarGrid make_grid(std::array<size_t, 3> dimensions, Vec3 lo, Vec3 hi, Function value) {
    ScalarGrid grid;
    grid.dims = dimensions;
    grid.bounds = {lo, hi};
    grid.values.resize(dimensions[0] * dimensions[1] * dimensions[2]);
    for (size_t k = 0; k < dimensions[2]; ++k) {
        for (size_t j = 0; j < dimensions[1]; ++j) {
            for (size_t i = 0; i < dimensions[0]; ++i) {
                Vec3 const p = grid.position(i, j, k);
                grid.values[grid.index(i, j, k)] = value(p);
            }
        }
    }
    return grid;
}

void test_sphere(double sign) {
    auto const grid = make_grid({25, 25, 25}, {-1, -1, -1}, {1, 1, 1}, [sign](Vec3 p) {
        return sign * (0.63 * 0.63 - p.x * p.x - p.y * p.y - p.z * p.z);
    });
    Mesh const mesh = marching_cubes::extract(grid, 0);
    check(!mesh.triangles.empty(), "sphere surface is empty");
    check(mesh.positions.size() == mesh.normals.size(), "missing normals");
    std::map<std::pair<uint32_t, uint32_t>, std::array<int, 2>> edges;
    for (auto const &tri : mesh.triangles) {
        for (size_t i = 0; i < 3; ++i) {
            uint32_t const a = tri[i];
            uint32_t const b = tri[(i + 1) % 3];
            check(a < mesh.positions.size() && b < mesh.positions.size(), "invalid triangle index");
            auto const key = std::minmax(a, b);
            auto &count = edges[{key.first, key.second}];
            ++count[0];
            count[1] += a < b ? 1 : -1;
        }
        Vec3 const a = mesh.positions[tri[0]];
        Vec3 const b = mesh.positions[tri[1]];
        Vec3 const c = mesh.positions[tri[2]];
        Vec3 const area = marching_cubes::cross(b - a, c - a);
        check(marching_cubes::norm(area) > 0, "degenerate triangle");
        check(sign * marching_cubes::dot(area, a + b + c) > 0, "sphere winding is not outward");
    }
    for (auto const &edge : edges) {
        check(edge.second[0] == 2, "sphere has a boundary or nonmanifold edge");
        check(edge.second[1] == 0, "sphere triangle orientations disagree across an edge");
    }
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        check(sign * marching_cubes::dot(mesh.positions[i], mesh.normals[i]) > 0, "sphere normal is not outward");
        check(std::abs(marching_cubes::norm(mesh.normals[i]) - 1) < 1e-12, "normal is not unit length");
    }
}

void test_exact_node_plane() {
    auto const grid = make_grid({5, 5, 5}, {-1, -1, -1}, {1, 1, 1}, [](Vec3 p) { return p.x; });
    Mesh const mesh = marching_cubes::extract(grid, 0);
    check(!mesh.triangles.empty(), "exact-node plane is empty");
    std::map<std::pair<uint32_t, uint32_t>, std::array<int, 2>> edges;
    for (auto const &triangle : mesh.triangles) {
        Vec3 const a = mesh.positions[triangle[0]];
        Vec3 const b = mesh.positions[triangle[1]];
        Vec3 const c = mesh.positions[triangle[2]];
        check(marching_cubes::dot(marching_cubes::cross(b - a, c - a), {-1, 0, 0}) > 0,
              "exact-node triangle is degenerate or has incorrect winding");
        for (size_t i = 0; i < 3; ++i) {
            uint32_t const u = triangle[i];
            uint32_t const v = triangle[(i + 1) % 3];
            auto const edge = std::minmax(u, v);
            auto &count = edges[{edge.first, edge.second}];
            ++count[0];
            count[1] += u < v ? 1 : -1;
        }
    }
    for (auto const &edge : edges) {
        check(edge.second[0] <= 2, "exact-node plane has a nonmanifold edge");
        if (edge.second[0] == 2) {
            check(edge.second[1] == 0, "exact-node plane winding is inconsistent");
        } else {
            Vec3 const a = mesh.positions[edge.first.first];
            Vec3 const b = mesh.positions[edge.first.second];
            check((a.y == -1 && b.y == -1) || (a.y == 1 && b.y == 1) ||
                  (a.z == -1 && b.z == -1) || (a.z == 1 && b.z == 1),
                  "exact-node plane has a crack away from its boundary");
        }
    }
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        check(mesh.positions[i].x == 0, "exact-node intersection is displaced");
        check(mesh.normals[i].x < -0.999999999, "exact-node normal is incorrect");
    }
}

void test_plane() {
    auto const grid = make_grid({8, 9, 10}, {-1, -1, -1}, {1, 1, 1}, [](Vec3 p) {
        return p.x + 2 * p.y + 3 * p.z - 0.271;
    });
    Mesh const mesh = marching_cubes::extract(grid, 0);
    check(!mesh.triangles.empty(), "plane surface is empty");
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        Vec3 const p = mesh.positions[i];
        Vec3 const n = mesh.normals[i];
        check(std::abs(p.x + 2 * p.y + 3 * p.z - 0.271) < 1e-12, "plane intersection is inaccurate");
        check(marching_cubes::dot(n, {-1, -2, -3}) > 3.74165738677 - 1e-10, "plane normal is not outward");
    }
}

void check_ambiguous_shared_face(double positive, double negative) {
    ScalarGrid grid;
    grid.dims = {3, 2, 2};
    grid.bounds = {{0, 0, 0}, {2, 1, 1}};
    grid.values.assign(12, -2);
    grid.values[grid.index(1, 0, 0)] = positive;
    grid.values[grid.index(1, 1, 0)] = negative;
    grid.values[grid.index(1, 1, 1)] = positive;
    grid.values[grid.index(1, 0, 1)] = negative;
    Mesh const a = marching_cubes::extract(grid, 0);
    Mesh const b = marching_cubes::extract(grid, 0);
    check(a.positions.size() == b.positions.size() && a.triangles == b.triangles,
          "mesh topology is not deterministic");
    for (size_t i = 0; i < a.positions.size(); ++i) {
        check(a.positions[i].x == b.positions[i].x && a.positions[i].y == b.positions[i].y &&
              a.positions[i].z == b.positions[i].z, "mesh vertices are not deterministic");
    }
    std::map<std::pair<uint32_t, uint32_t>, std::array<int, 2>> on_face;
    for (auto const &tri : a.triangles) {
        for (size_t i = 0; i < 3; ++i) {
            uint32_t const u = tri[i];
            uint32_t const v = tri[(i + 1) % 3];
            if (a.positions[u].x == 1 && a.positions[v].x == 1) {
                auto const edge = std::minmax(u, v);
                auto &count = on_face[{edge.first, edge.second}];
                ++count[0];
                count[1] += u < v ? 1 : -1;
            }
        }
    }
    check(on_face.size() == 2, "ambiguous face has an unexpected segment count");
    auto edge_number = [](Vec3 p) {
        if (p.z == 0) { return 0; }
        if (p.y == 1) { return 1; }
        if (p.z == 1) { return 2; }
        if (p.y == 0) { return 3; }
        throw std::runtime_error("face crossing is not on a face edge");
    };
    std::set<std::pair<int, int>> pairings;
    for (auto const &edge : on_face) {
        check(edge.second[0] == 2, "ambiguous face does not share segments across cells");
        check(edge.second[1] == 0, "ambiguous face has inconsistent directed edges");
        int const u = edge_number(a.positions[edge.first.first]);
        int const v = edge_number(a.positions[edge.first.second]);
        auto const pair = std::minmax(u, v);
        pairings.emplace(pair.first, pair.second);
    }
    std::set<std::pair<int, int>> const expected = positive * positive >= negative * negative
        ? std::set<std::pair<int, int>>{{0, 1}, {2, 3}}
        : std::set<std::pair<int, int>>{{0, 3}, {1, 2}};
    check(pairings == expected, "asymptotic face decider chose the wrong pairing");
}

void test_irregular_closed_winding() {
    ScalarGrid grid;
    grid.dims = {4, 4, 4};
    grid.bounds = {{0, 0, 0}, {3, 3, 3}};
    grid.values.assign(64, -1);
    grid.values[grid.index(1, 1, 1)] = -5.38967438069845;
    grid.values[grid.index(2, 1, 1)] = 0.014663351467722397;
    grid.values[grid.index(1, 2, 1)] = -0.4723915309990746;
    grid.values[grid.index(2, 2, 1)] = -0.18626904383260756;
    grid.values[grid.index(1, 1, 2)] = -38.62984966367557;
    grid.values[grid.index(2, 1, 2)] = 2.707405859287783;
    grid.values[grid.index(1, 2, 2)] = -0.08564884672015885;
    grid.values[grid.index(2, 2, 2)] = -0.021810938790796243;
    Mesh const mesh = marching_cubes::extract(grid, 0);
    check(!mesh.triangles.empty(), "irregular closed surface is empty");
    std::map<std::pair<uint32_t, uint32_t>, std::array<int, 2>> edges;
    for (auto const &tri : mesh.triangles) {
        for (size_t i = 0; i < 3; ++i) {
            uint32_t const u = tri[i];
            uint32_t const v = tri[(i + 1) % 3];
            auto const edge = std::minmax(u, v);
            auto &count = edges[{edge.first, edge.second}];
            ++count[0];
            count[1] += u < v ? 1 : -1;
        }
    }
    for (auto const &edge : edges) {
        check(edge.second[0] == 2, "irregular closed surface is not a two-manifold");
        check(edge.second[1] == 0, "irregular surface has inconsistent directed edges");
    }
}

void test_invalid_input() {
    auto grid = make_grid({3, 3, 3}, {-1, -1, -1}, {1, 1, 1}, [](Vec3 p) { return p.x; });
    bool rejected = false;
    grid.values[0] = std::numeric_limits<double>::quiet_NaN();
    try {
        marching_cubes::extract(grid, 0);
    } catch (std::invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "nonfinite sample was accepted");
    grid.values[0] = 0;
    rejected = false;
    try {
        marching_cubes::extract(grid, std::numeric_limits<double>::infinity());
    } catch (std::invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "nonfinite isovalue was accepted");
    grid.values.pop_back();
    rejected = false;
    try {
        marching_cubes::extract(grid, 0);
    } catch (std::invalid_argument const &) {
        rejected = true;
    }
    check(rejected, "incorrect input size was accepted");
}
}

int main() {
    try {
        test_sphere(1);
        test_sphere(-1);
        test_plane();
        test_exact_node_plane();
        check_ambiguous_shared_face(3, -1);
        check_ambiguous_shared_face(1, -3);
        check_ambiguous_shared_face(1, -1);
        test_irregular_closed_winding();
        test_invalid_input();
        std::cout << "marching cubes extraction tests passed\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
