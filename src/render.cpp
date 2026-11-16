#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>

#include <marching_cubes/render.hpp>

namespace marching_cubes {
namespace {

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double length(Vec3 v) {
    return std::hypot(v.x, v.y, v.z);
}

Vec3 unit(Vec3 v, Vec3 fallback) {
    double const n = length(v);
    return std::isfinite(n) && n > 0 ? Vec3{v.x / n, v.y / n, v.z / n} : fallback;
}

double clamp01(double x) {
    return std::clamp(x, 0.0, 1.0);
}

struct Projected {
    double x, y, depth;
    Vec3 normal;
};

struct Fragment {
    double depth;
    float r, g, b, alpha;
    size_t next;
};

double edge(Projected const &a, Projected const &b, double x, double y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

bool inclusive_edge(Projected const &a, Projected const &b) {
    double const dy = b.y - a.y;
    return dy < 0 || (dy == 0 && b.x > a.x);
}

bool contains(double e, bool inclusive) {
    return e > 0 || (e == 0 && inclusive);
}

Vec3 shade(Vec3 n, Material const &m, Vec3 light) {
    n = unit(n, {0, 0, 1});
    if (n.z < 0) {
        n = -n;
    }
    Vec3 const fill = unit({0.6, -0.3, 1.0}, {0, 0, 1});
    double const diffuse = 0.18 + 0.69 * std::max(0.0, dot(n, light))
        + 0.13 * std::max(0.0, dot(n, fill));
    Vec3 const halfway = unit(light + Vec3{0, 0, 1}, {0, 0, 1});
    double const shine = m.specular * std::pow(std::max(0.0, dot(n, halfway)), m.shininess);
    return {clamp01(m.color.x * diffuse + shine),
        clamp01(m.color.y * diffuse + shine), clamp01(m.color.z * diffuse + shine)};
}

void validate(std::vector<Surface> const &surfaces, RenderOptions const &o) {
    if (!o.width || !o.height || !o.supersampling || o.supersampling > 8 ||
        !finite(o.camera.focus) || !finite(o.camera.normal) || !finite(o.camera.up) ||
        !finite(o.light_direction) || !finite(o.background) || !std::isfinite(o.camera.scale) ||
        o.camera.scale <= 0 || length(o.camera.normal) == 0 || length(o.camera.up) == 0 ||
        length(o.light_direction) == 0 || !std::isfinite(length(o.camera.normal)) ||
        !std::isfinite(length(o.camera.up)) || !std::isfinite(length(o.light_direction))) {
        throw std::invalid_argument("invalid render dimensions, camera, light or background");
    }
    Vec3 const normal = unit(o.camera.normal, {0, 0, 1});
    Vec3 const up = unit(o.camera.up, {0, 1, 0});
    if (length(cross(up, normal)) < 1e-12) {
        throw std::invalid_argument("camera up and normal are parallel");
    }
    size_t const max = std::numeric_limits<size_t>::max();
    if (o.width > max / o.supersampling || o.height > max / o.supersampling ||
        o.width * o.supersampling > max / (o.height * o.supersampling) ||
        o.width > max / o.height || o.width * o.height > max / 3) {
        throw std::invalid_argument("render dimensions overflow");
    }
    // Screen coordinates are converted to integer bounds for rasterization.
    if (o.width * o.supersampling > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        o.height * o.supersampling > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("render dimensions exceed supported range");
    }
    for (Surface const &s : surfaces) {
        Material const &m = s.material;
        if (!finite(m.color) || !std::isfinite(m.opacity) || m.opacity < 0 || m.opacity > 1 ||
            !std::isfinite(m.specular) || m.specular < 0 || !std::isfinite(m.shininess) || m.shininess <= 0 ||
            (!s.mesh.normals.empty() && s.mesh.normals.size() != s.mesh.positions.size())) {
            throw std::invalid_argument("invalid surface material or normals");
        }
        for (Vec3 p : s.mesh.positions) {
            if (!finite(p)) {
                throw std::invalid_argument("nonfinite surface position");
            }
        }
        for (Vec3 n : s.mesh.normals) {
            if (!finite(n)) {
                throw std::invalid_argument("nonfinite surface normal");
            }
        }
        for (auto const &tri : s.mesh.triangles) {
            if (tri[0] >= s.mesh.positions.size() || tri[1] >= s.mesh.positions.size() ||
                tri[2] >= s.mesh.positions.size()) {
                throw std::invalid_argument("surface triangle index out of bounds");
            }
        }
    }
}

} // namespace

std::vector<unsigned char> render_rgb(std::vector<Surface> const &surfaces, RenderOptions const &o) {
    validate(surfaces, o);
    size_t const w = o.width * o.supersampling, h = o.height * o.supersampling;
    size_t const none = std::numeric_limits<size_t>::max();
    std::vector<size_t> heads(w * h, none);
    std::vector<Fragment> fragments;
    Vec3 const view = unit(o.camera.normal, {0, 0, 1});
    Vec3 const right = unit(cross(unit(o.camera.up, {0, 1, 0}), view), {1, 0, 0});
    Vec3 const up = unit(cross(view, right), {0, 1, 0});
    Vec3 const light = unit(o.light_direction, {0, 0, 1});
    double const pixels_per_scale = static_cast<double>(h) * 0.5;
    for (Surface const &surface : surfaces) {
        Mesh const &mesh = surface.mesh;
        if (surface.material.opacity == 0) {
            continue;
        }
        std::vector<Projected> points;
        points.reserve(mesh.positions.size());
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            Vec3 const offset = mesh.positions[i] - o.camera.focus;
            Vec3 const relative{offset.x / o.camera.scale, offset.y / o.camera.scale, offset.z / o.camera.scale};
            Vec3 const n = mesh.normals.empty() ? Vec3{} : mesh.normals[i];
            Projected const p{static_cast<double>(w) * 0.5 + dot(relative, right) * pixels_per_scale,
                static_cast<double>(h) * 0.5 - dot(relative, up) * pixels_per_scale,
                dot(relative, view), {dot(n, right), dot(n, up), dot(n, view)}};
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.depth)) {
                throw std::invalid_argument("projected geometry exceeds numeric range");
            }
            points.push_back(p);
        }
        for (auto const &indices : mesh.triangles) {
            Projected a = points[indices[0]], b = points[indices[1]], c = points[indices[2]];
            double area = edge(a, b, c.x, c.y);
            if (!std::isfinite(area)) {
                throw std::invalid_argument("projected triangle exceeds numeric range");
            }
            if (area == 0) {
                continue;
            }
            if (area < 0) {
                std::swap(b, c);
                area = -area;
            }
            // When normals are absent, use a face normal in camera coordinates.
            if (mesh.normals.empty()) {
                Vec3 const u{b.x - a.x, a.y - b.y, b.depth - a.depth};
                Vec3 const v{c.x - a.x, a.y - c.y, c.depth - a.depth};
                // x/y are in pixels; convert depth to the same units.
                Vec3 const face = unit(cross({u.x, u.y, u.z * pixels_per_scale},
                    {v.x, v.y, v.z * pixels_per_scale}), {0, 0, 1});
                a.normal = b.normal = c.normal = face;
            }
            double const minx = std::max(0.0, std::floor(std::min({a.x, b.x, c.x})));
            double const maxx = std::min(static_cast<double>(w - 1), std::ceil(std::max({a.x, b.x, c.x})));
            double const miny = std::max(0.0, std::floor(std::min({a.y, b.y, c.y})));
            double const maxy = std::min(static_cast<double>(h - 1), std::ceil(std::max({a.y, b.y, c.y})));
            if (minx > maxx || miny > maxy) {
                continue;
            }
            bool const ab = inclusive_edge(a, b), bc = inclusive_edge(b, c), ca = inclusive_edge(c, a);
            for (int y = static_cast<int>(miny); y <= static_cast<int>(maxy); ++y) {
                for (int x = static_cast<int>(minx); x <= static_cast<int>(maxx); ++x) {
                    double const px = x + 0.5, py = y + 0.5;
                    double const ea = edge(b, c, px, py), eb = edge(c, a, px, py), ec = edge(a, b, px, py);
                    if (!contains(ea, bc) || !contains(eb, ca) || !contains(ec, ab)) {
                        continue;
                    }
                    double const wa = ea / area, wb = eb / area, wc = ec / area;
                    double const depth = wa * a.depth + wb * b.depth + wc * c.depth;
                    Vec3 const color = shade(a.normal * wa + b.normal * wb + c.normal * wc, surface.material, light);
                    size_t const pixel = static_cast<size_t>(y) * w + static_cast<size_t>(x);
                    fragments.push_back({depth, static_cast<float>(color.x), static_cast<float>(color.y),
                        static_cast<float>(color.z), static_cast<float>(surface.material.opacity), heads[pixel]});
                    heads[pixel] = fragments.size() - 1;
                }
            }
        }
    }
    std::vector<unsigned char> out(o.width * o.height * 3);
    std::vector<size_t> order;
    auto const cmp = [&fragments](size_t a, size_t b) {
        Fragment const &x = fragments[a], &y = fragments[b];
        if (x.depth != y.depth) { return x.depth > y.depth; }
        if (x.alpha != y.alpha) { return x.alpha > y.alpha; }
        if (x.r != y.r) { return x.r > y.r; }
        if (x.g != y.g) { return x.g > y.g; }
        return x.b > y.b;
    };
    for (size_t py = 0; py < o.height; ++py) {
        for (size_t px = 0; px < o.width; ++px) {
            Vec3 sum{};
            for (size_t sy = 0; sy < o.supersampling; ++sy) {
                for (size_t sx = 0; sx < o.supersampling; ++sx) {
                    size_t const pixel = (py * o.supersampling + sy) * w + px * o.supersampling + sx;
                    order.clear();
                    for (size_t i = heads[pixel]; i != none; i = fragments[i].next) {
                        order.push_back(i);
                    }
                    std::sort(order.begin(), order.end(), cmp);
                    Vec3 color{};
                    double transmittance = 1;
                    for (size_t i : order) {
                        Fragment const &f = fragments[i];
                        double const contribution = transmittance * f.alpha;
                        color = color + Vec3{f.r, f.g, f.b} * contribution;
                        transmittance *= 1 - f.alpha;
                        if (transmittance == 0) { break; }
                    }
                    sum = sum + color + o.background * transmittance;
                }
            }
            double const inv = 1.0 / (o.supersampling * o.supersampling);
            for (size_t c = 0; c < 3; ++c) {
                double const srgb = std::pow(clamp01(sum[c] * inv), 1.0 / 2.2);
                out[(py * o.width + px) * 3 + c] = static_cast<unsigned char>(std::lround(255 * srgb));
            }
        }
    }
    return out;
}

void render_png(std::vector<Surface> const &surfaces, RenderOptions const &o, std::string const &path) {
    if (path.empty() || o.width > PNG_UINT_31_MAX || o.height > PNG_UINT_31_MAX) {
        throw std::invalid_argument("invalid PNG output path or dimensions");
    }
    std::vector<unsigned char> const rgb = render_rgb(surfaces, o);
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(o.width);
    image.height = static_cast<png_uint_32>(o.height);
    image.format = PNG_FORMAT_RGB;
    if (!png_image_write_to_file(&image, path.c_str(), 0, rgb.data(), 0, nullptr)) {
        std::string const message = image.message;
        png_image_free(&image);
        throw std::runtime_error("cannot write PNG '" + path + "': " + message);
    }
    png_image_free(&image);
}

} // namespace marching_cubes
