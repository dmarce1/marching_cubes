#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <silo.h>

#include "marching_cubes/silo_reader.hpp"

namespace marching_cubes {
namespace {

size_t constexpr none = std::numeric_limits<size_t>::max();

Bounds block_bounds(DensityBlock const &b) {
    return {{b.edges[0].front(), b.edges[1].front(), b.edges[2].front()},
        {b.edges[0].back(), b.edges[1].back(), b.edges[2].back()}};
}
Bounds unite(Bounds a, Bounds b) {
    for (size_t d = 0; d < 3; ++d) {
        a.lo[d] = std::min(a.lo[d], b.lo[d]);
        a.hi[d] = std::max(a.hi[d], b.hi[d]);
    }
    return a;
}
bool contains(Bounds b, Vec3 p) {
    for (size_t d = 0; d < 3; ++d) {
        if (p[d] < b.lo[d] || p[d] > b.hi[d]) { return false; }
    }
    return true;
}
bool interior_overlap(Bounds a, Bounds b) {
    for (size_t d = 0; d < 3; ++d) {
        double const width = std::min(a.hi[d] - a.lo[d], b.hi[d] - b.lo[d]);
        double const tolerance = 1e-10 * width;
        if (std::min(a.hi[d], b.hi[d]) - std::max(a.lo[d], b.lo[d]) <= tolerance) { return false; }
    }
    return true;
}
size_t cell(std::vector<double> const &edges, double x) {
    auto it = std::upper_bound(edges.begin(), edges.end(), x);
    if (it == edges.begin()) { return 0; }
    return std::min(size_t(it - edges.begin() - 1), edges.size() - 2);
}
double center(std::vector<double> const &edges, size_t i) { return 0.5 * (edges[i] + edges[i + 1]); }
size_t index(DensityBlock const &b, std::array<size_t, 3> i) {
    return (i[2] * (b.edges[1].size() - 1) + i[1]) * (b.edges[0].size() - 1) + i[0];
}
double value(void const *p, int type, size_t i) {
    if (!p) { throw std::runtime_error("Silo array is null"); }
    if (type == DB_FLOAT) { return static_cast<float const *>(p)[i]; }
    if (type == DB_DOUBLE) { return static_cast<double const *>(p)[i]; }
    throw std::runtime_error("Silo array is not float or double");
}
struct FileCloser { void operator()(DBfile *p) const { if (p) { DBClose(p); } } };
struct MeshCloser { void operator()(DBmultimesh *p) const { if (p) { DBFreeMultimesh(p); } } };
struct MultiVarCloser { void operator()(DBmultivar *p) const { if (p) { DBFreeMultivar(p); } } };
struct QuadMeshCloser { void operator()(DBquadmesh *p) const { if (p) { DBFreeQuadmesh(p); } } };
struct QuadVarCloser { void operator()(DBquadvar *p) const { if (p) { DBFreeQuadvar(p); } } };
using File = std::unique_ptr<DBfile, FileCloser>;
using MultiMesh = std::unique_ptr<DBmultimesh, MeshCloser>;
using MultiVar = std::unique_ptr<DBmultivar, MultiVarCloser>;
using QuadMesh = std::unique_ptr<DBquadmesh, QuadMeshCloser>;
using QuadVar = std::unique_ptr<DBquadvar, QuadVarCloser>;

std::pair<std::string, std::string> reference(std::string const &ref, std::filesystem::path const &master) {
    size_t const colon = ref.find(':');
    if (colon == std::string::npos) { return {master.string(), ref}; }
    auto file = std::filesystem::path(ref.substr(0, colon));
    if (file.is_relative()) { file = master.parent_path() / file; }
    return {file.lexically_normal().string(), ref.substr(colon + 1)};
}

}

SiloDataset::SiloDataset(std::vector<DensityBlock> blocks) : blocks_(std::move(blocks)) {
    if (blocks_.empty()) { throw std::runtime_error("dataset contains no blocks"); }
    peak_ = -std::numeric_limits<double>::infinity();
    for (auto const &b : blocks_) {
        size_t count = 1;
        for (size_t d = 0; d < 3; ++d) {
            if (b.edges[d].size() < 2) { throw std::runtime_error("block has no cells"); }
            for (size_t i = 0; i < b.edges[d].size(); ++i) {
                if (!std::isfinite(b.edges[d][i]) || (i && !(b.edges[d][i] > b.edges[d][i - 1]))) {
                    throw std::runtime_error("block edges must be finite and strictly increasing");
                }
            }
            for (size_t i = 1; i < b.edges[d].size(); ++i) {
                min_spacing_[d] = std::min(min_spacing_[d], b.edges[d][i] - b.edges[d][i - 1]);
            }
            if (count > std::numeric_limits<size_t>::max() / (b.edges[d].size() - 1)) {
                throw std::runtime_error("block dimensions overflow");
            }
            count *= b.edges[d].size() - 1;
        }
        if (count != b.values.size()) { throw std::runtime_error("block value count does not match cells"); }
        for (double v : b.values) {
            if (!std::isfinite(v)) { throw std::runtime_error("nonfinite density"); }
            peak_ = std::max(peak_, v);
        }
        block_bounds_.push_back(block_bounds(b));
    }
    bounds_ = block_bounds_.front();
    for (auto b : block_bounds_) { bounds_ = unite(bounds_, b); }
    order_.resize(blocks_.size());
    std::iota(order_.begin(), order_.end(), size_t(0));
    build(0, order_.size());
    for (size_t i = 0; i < blocks_.size(); ++i) { verify_overlaps(i, 0); }
}
int SiloDataset::build(size_t begin, size_t end) {
    Bounds b = block_bounds_[order_[begin]];
    for (size_t i = begin + 1; i < end; ++i) { b = unite(b, block_bounds_[order_[i]]); }
    int const node = static_cast<int>(nodes_.size());
    nodes_.push_back({b, begin, end});
    if (end - begin > 8) {
        size_t axis = 0;
        for (size_t d = 1; d < 3; ++d) {
            if (b.hi[d] - b.lo[d] > b.hi[axis] - b.lo[axis]) { axis = d; }
        }
        size_t const middle = begin + (end - begin) / 2;
        std::nth_element(order_.begin() + begin, order_.begin() + middle, order_.begin() + end,
            [&](size_t a, size_t c) {
                double const x = block_bounds_[a].lo[axis] + block_bounds_[a].hi[axis];
                double const y = block_bounds_[c].lo[axis] + block_bounds_[c].hi[axis];
                return x < y || (x == y && a < c);
            });
        int const left = build(begin, middle), right = build(middle, end);
        nodes_[node].left = left; nodes_[node].right = right;
    }
    return node;
}
void SiloDataset::verify_overlaps(size_t block, int n) const {
    Node const &node = nodes_[n];
    if (!interior_overlap(block_bounds_[block], node.bounds)) { return; }
    if (node.left >= 0) { verify_overlaps(block, node.left); verify_overlaps(block, node.right); return; }
    for (size_t j = node.begin; j < node.end; ++j) {
        size_t const other = order_[j];
        if (other > block && interior_overlap(block_bounds_[block], block_bounds_[other])) {
            throw std::runtime_error("overlapping mesh blocks; expected nonoverlapping AMR leaves");
        }
    }
}
size_t SiloDataset::locate(Vec3 p) const {
    if (!contains(bounds_, p)) { throw std::out_of_range("sample outside dataset bounds"); }
    size_t best = none;
    std::vector<int> stack{0};
    while (!stack.empty()) {
        int const n = stack.back(); stack.pop_back();
        Node const &node = nodes_[n];
        if (!contains(node.bounds, p)) { continue; }
        if (node.left >= 0) { stack.push_back(node.right); stack.push_back(node.left); continue; }
        for (size_t i = node.begin; i < node.end; ++i) {
            size_t const b = order_[i];
            if (!contains(block_bounds_[b], p)) { continue; }
            bool owner = true;
            for (size_t d = 0; d < 3; ++d) {
                if (p[d] == block_bounds_[b].hi[d] && p[d] != bounds_.hi[d]) { owner = false; }
            }
            if (owner && (best == none || b < best)) { best = b; }
        }
    }
    if (best == none) { throw std::runtime_error("sample falls in a gap between mesh blocks"); }
    return best;
}

double SiloDataset::sample(Vec3 p, Reconstruction mode) const {
    return sample_impl(p, mode, nullptr);
}

double SiloDataset::sample_impl(Vec3 p, Reconstruction mode, SamplingStats *stats) const {
    for (size_t d = 0; d < 3; ++d) {
        if (!std::isfinite(p[d])) { throw std::out_of_range("sample coordinate is nonfinite"); }
    }
    size_t const owner = locate(p);
    DensityBlock const &b = blocks_[owner];
    std::array<size_t, 3> i{};
    for (size_t d = 0; d < 3; ++d) { i[d] = cell(b.edges[d], p[d]); }
    double const fallback = b.values[index(b, i)];
    if (mode == Reconstruction::piecewise_constant) { return fallback; }
    // Eight surrounding cell centres, including matching-resolution neighbours.
    std::array<std::array<double, 2>, 3> c{};
    std::array<double, 3> t{};
    for (size_t d = 0; d < 3; ++d) {
        double const here = center(b.edges[d], i[d]);
        double const width = b.edges[d][i[d] + 1] - b.edges[d][i[d]];
        bool const lower = p[d] < here;
        double neighbour;
        if (lower && i[d] > 0) { neighbour = center(b.edges[d], i[d] - 1); }
        else if (!lower && i[d] + 2 < b.edges[d].size()) { neighbour = center(b.edges[d], i[d] + 1); }
        else { neighbour = here + (lower ? -width : width); }
        c[d] = lower ? std::array<double, 2>{neighbour, here} : std::array<double, 2>{here, neighbour};
        // Clamp at the outer domain boundary to the nearest cell centre.
        for (double &x : c[d]) { x = std::clamp(x, bounds_.lo[d], bounds_.hi[d]); }
        t[d] = c[d][0] == c[d][1] ? 0 : std::clamp((p[d] - c[d][0]) / (c[d][1] - c[d][0]), 0.0, 1.0);
    }
    double result = 0;
    for (size_t k = 0; k < 2; ++k) {
        for (size_t j = 0; j < 2; ++j) {
            for (size_t x = 0; x < 2; ++x) {
                Vec3 q{c[0][x], c[1][j], c[2][k]};
                size_t const nb = locate(q);
                DensityBlock const &other = blocks_[nb];
                std::array<size_t, 3> ci{};
                for (size_t d = 0; d < 3; ++d) {
                    ci[d] = cell(other.edges[d], q[d]);
                    double const target_width = b.edges[d][i[d] + 1] - b.edges[d][i[d]];
                    double const actual_width = other.edges[d][ci[d] + 1] - other.edges[d][ci[d]];
                    // A coarse/fine interface has no unique same-level stencil.
                    if (std::abs(actual_width - target_width) > 1e-8 * target_width) {
                        if (stats) { ++stats->coarse_fine_fallback_samples; }
                        return fallback;
                    }
                }
                double const weight = (x ? t[0] : 1 - t[0]) * (j ? t[1] : 1 - t[1]) * (k ? t[2] : 1 - t[2]);
                result += weight * other.values[index(other, ci)];
            }
        }
    }
    return result;
}
ScalarGrid SiloDataset::resample(Bounds b, std::array<size_t, 3> dims, Reconstruction mode, SamplingStats *stats) const {
    size_t count = 1;
    for (size_t d = 0; d < 3; ++d) {
        if (dims[d] < 2 || !(b.hi[d] > b.lo[d]) || b.lo[d] < bounds_.lo[d] || b.hi[d] > bounds_.hi[d]) {
            throw std::invalid_argument("resampling bounds or dimensions invalid");
        }
        if (count > std::numeric_limits<size_t>::max() / dims[d]) { throw std::invalid_argument("grid dimensions overflow"); }
        count *= dims[d];
    }
    ScalarGrid grid{b, dims, std::vector<double>(count)};
    for (size_t k = 0; k < dims[2]; ++k) {
        for (size_t j = 0; j < dims[1]; ++j) {
            for (size_t i = 0; i < dims[0]; ++i) {
                Vec3 p = grid.position(i, j, k);
                if (i + 1 == dims[0]) { p.x = b.hi.x; }
                if (j + 1 == dims[1]) { p.y = b.hi.y; }
                if (k + 1 == dims[2]) { p.z = b.hi.z; }
                grid.values[grid.index(i, j, k)] = sample_impl(p, mode, stats);
            }
        }
    }
    return grid;
}

SiloDataset SiloDataset::load(std::string const &path, std::vector<std::string> const &variables) {
    if (variables.empty()) { throw std::invalid_argument("at least one density variable is required"); }
    std::filesystem::path const master = std::filesystem::absolute(path).lexically_normal();
    File file(DBOpen(master.string().c_str(), DB_UNKNOWN, DB_READ));
    if (!file) { throw std::runtime_error("cannot open Silo master: " + master.string()); }
    MultiMesh mesh(DBGetMultimesh(file.get(), "quadmesh"));
    if (!mesh || mesh->nblocks <= 0 || !mesh->meshnames) {
        throw std::runtime_error("missing explicit quadmesh multimesh: " + master.string());
    }
    std::vector<MultiVar> vars;
    for (auto const &name : variables) {
        MultiVar var(DBGetMultivar(file.get(), name.c_str()));
        if (!var || var->nvars != mesh->nblocks || !var->varnames) {
            throw std::runtime_error("missing or inconsistent Silo multivar: " + name);
        }
        vars.push_back(std::move(var));
    }
    std::unordered_map<std::string, File> files;
    auto get_file = [&](std::string const &name) -> DBfile * {
        auto it = files.find(name);
        if (it == files.end()) {
            File leaf(DBOpen(name.c_str(), DB_UNKNOWN, DB_READ));
            if (!leaf) { throw std::runtime_error("cannot open referenced Silo file: " + name); }
            it = files.emplace(name, std::move(leaf)).first;
        }
        return it->second.get();
    };
    std::vector<DensityBlock> blocks;
    blocks.reserve(size_t(mesh->nblocks));
    for (int n = 0; n < mesh->nblocks; ++n) {
        if (!mesh->meshnames[n]) { throw std::runtime_error("missing mesh reference at block " + std::to_string(n)); }
        auto const mr = reference(mesh->meshnames[n], master);
        QuadMesh qm(DBGetQuadmesh(get_file(mr.first), mr.second.c_str()));
        if (!qm) { throw std::runtime_error("cannot read referenced quadmesh " + mr.first + ":" + mr.second); }
        if (qm->ndims != 3 || qm->coordtype != DB_COLLINEAR || (qm->datatype != DB_FLOAT && qm->datatype != DB_DOUBLE)) {
            throw std::runtime_error("only 3D rectilinear float/double quadmeshes are supported");
        }
        DensityBlock b;
        size_t count = 1;
        for (size_t d = 0; d < 3; ++d) {
            if (qm->dims[d] < 2 || !qm->coords[d]) { throw std::runtime_error("invalid quadmesh coordinates"); }
            if (qm->min_index[d] != 0 || qm->max_index[d] != qm->dims[d] - 1) {
                throw std::runtime_error("quadmesh ghost zones are unsupported");
            }
            b.edges[d].resize(size_t(qm->dims[d]));
            for (size_t j = 0; j < b.edges[d].size(); ++j) { b.edges[d][j] = value(qm->coords[d], qm->datatype, j); }
            if (count > std::numeric_limits<size_t>::max() / (b.edges[d].size() - 1)) {
                throw std::runtime_error("quadmesh dimensions overflow");
            }
            count *= b.edges[d].size() - 1;
        }
        b.values.assign(count, 0.0);
        for (size_t field = 0; field < vars.size(); ++field) {
            if (!vars[field]->varnames[n]) { throw std::runtime_error("missing variable block reference: " + variables[field]); }
            auto const vr = reference(vars[field]->varnames[n], master);
            auto const mesh_dir = std::filesystem::path(mr.second).parent_path();
            auto const var_dir = std::filesystem::path(vr.second).parent_path();
            if (mr.first != vr.first || mesh_dir != var_dir) {
                throw std::runtime_error("variable block does not match its quadmesh: " + variables[field]);
            }
            QuadVar qv(DBGetQuadvar(get_file(vr.first), vr.second.c_str()));
            if (!qv) { throw std::runtime_error("cannot read referenced density variable " + vr.first + ":" + vr.second); }
            if (qv->ndims != 3 || qv->centering != DB_ZONECENT || qv->nvals != 1 || !qv->vals ||
                qv->nels < 0 || size_t(qv->nels) != count || (qv->datatype != DB_FLOAT && qv->datatype != DB_DOUBLE)) {
                throw std::runtime_error("density must be a scalar, zone-centered 3D float/double variable");
            }
            std::array<size_t, 3> dims{};
            for (size_t d = 0; d < 3; ++d) {
                dims[d] = b.edges[d].size() - 1;
                if (qv->dims[d] != int(dims[d]) || qv->min_index[d] != 0 || qv->max_index[d] != int(dims[d]) - 1) {
                    throw std::runtime_error("density dimensions or ghost zones do not match mesh");
                }
            }
            // Silo's supported x-fast order is explicit, avoiding silent axis permutation.
            if (qv->stride[0] != 1 || qv->stride[1] != int(dims[0]) || qv->stride[2] != int(dims[0] * dims[1])) {
                throw std::runtime_error("density is not stored in supported x-fast axis order");
            }
            for (size_t j = 0; j < count; ++j) { b.values[j] += value(qv->vals[0], qv->datatype, j); }
        }
        blocks.push_back(std::move(b));
    }
    SiloDataset result(std::move(blocks));
    if (DBGetComponentType(file.get(), "quadmesh", "dtime") == DB_DOUBLE) {
        void *p = DBGetComponent(file.get(), "quadmesh", "dtime");
        if (p) { result.time_ = *static_cast<double *>(p); std::free(p); }
    }
    if (DBGetComponentType(file.get(), "quadmesh", "cycle") == DB_INT) {
        void *p = DBGetComponent(file.get(), "quadmesh", "cycle");
        if (p) { result.cycle_ = *static_cast<int *>(p); std::free(p); }
    }
    return result;
}

}
