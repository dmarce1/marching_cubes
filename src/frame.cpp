#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "marching_cubes/extract.hpp"
#include "marching_cubes/frame.hpp"
#include "marching_cubes/render.hpp"
#include "marching_cubes/silo_reader.hpp"
#include "marching_cubes/types.hpp"

namespace marching_cubes {
namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
using nlohmann::json;

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double length(Vec3 v) { return std::hypot(v.x, v.y, v.z); }
Vec3 unit(Vec3 v) {
    double const n = length(v);
    if (!(n > 0) || !std::isfinite(n)) {
        throw std::invalid_argument("camera and light directions must be finite and nonzero");
    }
    return {v.x / n, v.y / n, v.z / n};
}
json vec(Vec3 v) { return json::array({v.x, v.y, v.z}); }
json finite_or_null(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Topology {
    size_t boundary = 0, nonmanifold = 0, inconsistent_winding = 0;
};
Topology topology(Mesh const &mesh) {
    struct Uses {
        size_t count = 0;
        int orientation = 0;
    };
    std::unordered_map<uint64_t, Uses> edges;
    for (auto const &tri : mesh.triangles) {
        for (size_t k = 0; k < 3; ++k) {
            uint32_t const a = std::min(tri[k], tri[(k + 1) % 3]);
            uint32_t const b = std::max(tri[k], tri[(k + 1) % 3]);
            Uses &uses = edges[(uint64_t(a) << 32) | b];
            ++uses.count;
            uses.orientation += tri[k] == a ? 1 : -1;
        }
    }
    Topology result;
    for (auto const &entry : edges) {
        Uses const &uses = entry.second;
        result.boundary += uses.count == 1;
        result.nonmanifold += uses.count > 2;
        result.inconsistent_winding += uses.count == 2 && uses.orientation != 0;
    }
    return result;
}
size_t outside_view(Mesh const &mesh, RenderOptions const &options) {
    Vec3 const view = unit(options.camera.normal);
    Vec3 const right = unit(cross(unit(options.camera.up), view));
    Vec3 const up = unit(cross(view, right));
    double const half_width =
        options.camera.scale * (double(options.width) / double(options.height));
    size_t count = 0;
    for (Vec3 p : mesh.positions) {
        Vec3 const relative = p - options.camera.focus;
        double const x = dot(relative, right);
        double const y = dot(relative, up);
        if (!finite(relative) || !std::isfinite(x) || !std::isfinite(y)) {
            throw std::runtime_error("viewport coordinates exceed numeric range");
        }
        count += std::abs(x) > half_width || std::abs(y) > options.camera.scale;
    }
    return count;
}
size_t boundary_above(ScalarGrid const &grid, double iso) {
    size_t count = 0;
    for (size_t k = 0; k < grid.dims[2]; ++k) {
        for (size_t j = 0; j < grid.dims[1]; ++j) {
            for (size_t i = 0; i < grid.dims[0]; ++i) {
                if ((i == 0 || j == 0 || k == 0 || i + 1 == grid.dims[0] || j + 1 == grid.dims[1] ||
                     k + 1 == grid.dims[2]) &&
                    grid.values[grid.index(i, j, k)] >= iso) {
                    ++count;
                }
            }
        }
    }
    return count;
}
void write_obj(Mesh const &mesh, fs::path const &path) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    out << std::setprecision(17) << "# marching_cubes: stored mesh coordinates; density "
        << mesh.isovalue << '\n';
    for (Vec3 v : mesh.positions) {
        out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
    }
    for (Vec3 n : mesh.normals) {
        out << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
    }
    for (auto const &triangle : mesh.triangles) {
        out << "f";
        for (uint32_t index : triangle) {
            // Cast before adding one: uint32_t's largest index is valid.
            out << ' ' << (uint64_t(index) + 1) << "//" << (uint64_t(index) + 1);
        }
        out << '\n';
    }
    out.close();
    if (!out) {
        throw std::runtime_error("Failed writing " + path.string());
    }
}

} // namespace

void validate_frame_config(FrameConfig const &config) {
    size_t count = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        double const lo = config.bounds.lo[axis];
        double const hi = config.bounds.hi[axis];
        if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo) || !std::isfinite(hi - lo)) {
            throw std::invalid_argument("sampling bounds must be finite and increasing");
        }
        size_t const n = config.samples[axis];
        if (n < 2 || n > 2048 || count > 200000000 / n) {
            throw std::invalid_argument(
                "sampling dimensions must be 2..2048 and at most 200 million nodes");
        }
        count *= n;
    }
    if (!std::isfinite(config.reference) || config.reference <= 0 || config.levels.empty() ||
        config.levels.size() > 4 || config.levels.size() != config.opacities.size()) {
        throw std::invalid_argument(
            "expected a positive reference and one to four levels with matching opacities");
    }
    for (size_t i = 0; i < config.levels.size(); ++i) {
        double const level = config.levels[i];
        double const opacity = config.opacities[i];
        double const iso = level * config.reference;
        if (!std::isfinite(level) || level <= 0 || !std::isfinite(iso) || iso <= 0 ||
            !std::isfinite(opacity) || opacity < 0 || opacity > 1) {
            throw std::invalid_argument("invalid surface density level or opacity");
        }
    }
    if (config.fields.empty()) {
        throw std::invalid_argument("at least one density field is required");
    }
    std::set<std::string> unique_fields;
    for (std::string const &field : config.fields) {
        if (field.empty() || !unique_fields.insert(field).second) {
            throw std::invalid_argument("density field names must be nonempty and unique");
        }
    }
    if (config.reconstruction != Reconstruction::piecewise_constant &&
        config.reconstruction != Reconstruction::trilinear) {
        throw std::invalid_argument("unknown reconstruction");
    }
    if (config.time_override && !std::isfinite(*config.time_override)) {
        throw std::invalid_argument("time override must be finite");
    }
    RenderOptions const &o = config.rendering;
    if (o.width < 16 || o.width > 8192 || o.height < 16 || o.height > 8192 || o.supersampling < 1 ||
        o.supersampling > 4 || !finite(o.camera.focus) || !finite(o.camera.normal) ||
        !finite(o.camera.up) || !finite(o.light_direction) || !finite(o.background) ||
        !std::isfinite(o.camera.scale) || o.camera.scale <= 0 ||
        !std::isfinite(o.camera.scale * (double(o.width) / double(o.height)))) {
        throw std::invalid_argument("invalid image dimensions, camera, light or background");
    }
    Vec3 const camera = unit(o.camera.normal);
    Vec3 const up = unit(o.camera.up);
    (void)unit(o.light_direction);
    if (length(cross(camera, up)) < 1e-12) {
        throw std::invalid_argument("camera up and normal are parallel");
    }
}

FrameResult build_frame(fs::path const &input, FrameConfig const &config,
                        std::optional<fs::path> obj_directory) {
    validate_frame_config(config);
    if (input.empty()) {
        throw std::invalid_argument("input path is empty");
    }
    if (obj_directory) {
        if (!fs::is_directory(*obj_directory)) {
            throw std::invalid_argument("OBJ output directory must already exist");
        }
        for (size_t i = 0; i < config.levels.size(); ++i) {
            fs::path const path = *obj_directory / ("surface_" + std::to_string(i) + ".obj");
            if (fs::symlink_status(path).type() != fs::file_type::not_found) {
                throw std::invalid_argument("OBJ output file already exists");
            }
        }
    }
    fs::path const source = fs::absolute(input);
    auto const read_start = Clock::now();
    SiloDataset data = SiloDataset::load(source.string(), config.fields);
    double const read_seconds = seconds(read_start);
    auto const sample_start = Clock::now();
    SamplingStats stats;
    ScalarGrid const grid =
        data.resample(config.bounds, config.samples, config.reconstruction, &stats);
    double const sample_seconds = seconds(sample_start);
    auto const extract_start = Clock::now();
    std::array<Vec3, 4> const colors{
        {{0.06, 0.85, 0.15}, {1, 0.78, 0.06}, {0.95, 0.06, 0.03}, {0.04, 0.22, 1}}};
    std::vector<Surface> surfaces;
    json surface_metadata = json::array();
    for (size_t i = 0; i < config.levels.size(); ++i) {
        double const iso = config.reference * config.levels[i];
        Surface surface;
        surface.mesh = extract(grid, iso);
        surface.material.color = colors[i];
        surface.material.opacity = config.opacities[i];
        Topology const t = topology(surface.mesh);
        surface_metadata.push_back(
            {{"isovalue", iso},
             {"vertices", surface.mesh.positions.size()},
             {"triangles", surface.mesh.triangles.size()},
             {"boundary_edges", t.boundary},
             {"nonmanifold_edges", t.nonmanifold},
             {"inconsistent_winding_edges", t.inconsistent_winding},
             {"boundary_samples_above", boundary_above(grid, iso)},
             {"vertices_outside_viewport", outside_view(surface.mesh, config.rendering)},
             {"fraction", config.levels[i]},
             {"opacity", surface.material.opacity},
             {"specular", surface.material.specular},
             {"shininess", surface.material.shininess},
             {"color", vec(surface.material.color)}});
        surfaces.push_back(std::move(surface));
    }
    if (std::all_of(surfaces.begin(), surfaces.end(),
                    [](Surface const &s) { return s.mesh.triangles.empty(); })) {
        throw std::runtime_error("All isosurfaces empty; check levels and bounds");
    }
    if (obj_directory) {
        for (size_t i = 0; i < surfaces.size(); ++i) {
            write_obj(surfaces[i].mesh, *obj_directory / ("surface_" + std::to_string(i) + ".obj"));
        }
    }
    double const extract_seconds = seconds(extract_start);
    auto const render_start = Clock::now();
    FrameResult result;
    if (!config.no_render) {
        result.rgb = render_rgb(surfaces, config.rendering);
    }
    double const render_seconds = seconds(render_start);
    double const time = config.time_override ? *config.time_override : data.time();
    std::string const reconstruction =
        config.reconstruction == Reconstruction::piecewise_constant ? "constant" : "trilinear";
    result.metadata = {
        {"schema", 1},
        {"source", source.string()},
        {"time", finite_or_null(time)},
        {"raw_master_time", finite_or_null(data.time())},
        {"time_overridden", config.time_override.has_value()},
        {"cycle", data.cycle()},
        {"density_reference", config.reference},
        {"blocks", data.block_count()},
        {"density_peak", data.density_peak()},
        {"fields", config.fields},
        {"reconstruction", reconstruction},
        {"reconstruction_note", config.reconstruction == Reconstruction::piecewise_constant
                                    ? "Containing leaf cell value"
                                    : "Cell centered trilinear across equal size blocks; owner "
                                      "cell constant at coarse-fine stencils"},
        {"coarse_fine_fallback_samples", stats.coarse_fine_fallback_samples},
        {"minimum_source_cell_spacing", vec(data.min_cell_spacing())},
        {"sample_spacing", vec(grid.spacing())},
        {"bounds_lo", vec(config.bounds.lo)},
        {"bounds_hi", vec(config.bounds.hi)},
        {"samples", config.samples},
        {"camera_focus", vec(config.rendering.camera.focus)},
        {"camera_normal", vec(config.rendering.camera.normal)},
        {"camera_up", vec(config.rendering.camera.up)},
        {"camera_scale", config.rendering.camera.scale},
        {"image_width", config.rendering.width},
        {"image_height", config.rendering.height},
        {"supersampling", config.rendering.supersampling},
        {"light_direction", vec(config.rendering.light_direction)},
        {"background", vec(config.rendering.background)},
        {"surfaces", std::move(surface_metadata)},
        {"seconds",
         {{"read", read_seconds},
          {"sample", sample_seconds},
          {"extract", extract_seconds},
          {"render", render_seconds}}}};
    return result;
}

} // namespace marching_cubes
