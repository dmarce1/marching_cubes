#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace marching_cubes {

struct Vec3 {
    double x = 0, y = 0, z = 0;
    double &operator[](size_t i) { return i == 0 ? x : i == 1 ? y : z; }
    double operator[](size_t i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x,-a.y,-a.z}; }
inline Vec3 operator*(Vec3 a, double b) { return {a.x*b,a.y*b,a.z*b}; }
inline Vec3 operator*(double b, Vec3 a) { return a*b; }
inline Vec3 operator/(Vec3 a, double b) { return a*(1/b); }
inline double dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline double norm(Vec3 a) { return std::sqrt(dot(a,a)); }
inline Vec3 normalized(Vec3 a) { double const n=norm(a); return n>0 ? a/n : Vec3{0,0,1}; }

struct Bounds { Vec3 lo, hi; };
struct ScalarGrid {
    Bounds bounds;
    std::array<size_t,3> dims{}; // Nodal samples, x fastest.
    std::vector<double> values;
    size_t index(size_t i,size_t j,size_t k) const { return (k*dims[1]+j)*dims[0]+i; }
    Vec3 spacing() const { return {(bounds.hi.x-bounds.lo.x)/(dims[0]-1),
        (bounds.hi.y-bounds.lo.y)/(dims[1]-1),(bounds.hi.z-bounds.lo.z)/(dims[2]-1)}; }
    Vec3 position(size_t i,size_t j,size_t k) const {
        auto const h=spacing(); return bounds.lo+Vec3{i*h.x,j*h.y,k*h.z};
    }
};
struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<std::array<uint32_t,3>> triangles;
    double isovalue=0;
};
struct Material {
    Vec3 color{0.1,0.6,1.0};
    double opacity=1.0;
    double specular=0.5;
    double shininess=60.0;
};
struct Surface { Mesh mesh; Material material; };
struct Camera {
    Vec3 focus{0,0,0};
    Vec3 normal{0,0,1}; // Toward observer.
    Vec3 up{0,1,0};
    double scale=1; // Vertical half-height, orthographic.
};
struct RenderOptions {
    Camera camera;
    size_t width=768, height=768;
    unsigned supersampling=1;
    Vec3 background{0,0,0};
    Vec3 light_direction{-0.45,0.6,1}; // Camera-space direction toward key light.
};
}
