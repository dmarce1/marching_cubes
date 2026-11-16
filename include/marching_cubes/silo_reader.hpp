#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "marching_cubes/types.hpp"

namespace marching_cubes {

// Cell edges along each axis and zone-centered values in x-fast order.
struct DensityBlock {
    std::array<std::vector<double>, 3> edges;
    std::vector<double> values;
};

enum class Reconstruction { piecewise_constant, trilinear };

// Optional cumulative counters for a resampling operation.
struct SamplingStats {
    size_t coarse_fine_fallback_samples = 0;
};

class SiloDataset {
public:
    explicit SiloDataset(std::vector<DensityBlock> blocks);
    static SiloDataset load(std::string const &path,
        std::vector<std::string> const &variables = {"rho_1", "rho_2", "rho_3", "rho_4", "rho_5"});

    Bounds bounds() const { return bounds_; }
    double time() const { return time_; }
    int cycle() const { return cycle_; }
    size_t block_count() const { return blocks_.size(); }
    double density_peak() const { return peak_; }
    Vec3 min_cell_spacing() const { return min_spacing_; }
    double sample(Vec3 point, Reconstruction mode) const;
    // When provided, counters are incremented, not reset.
    ScalarGrid resample(Bounds bounds, std::array<size_t, 3> dims, Reconstruction mode,
        SamplingStats *stats = nullptr) const;

private:
    struct Node { Bounds bounds; size_t begin, end; int left = -1, right = -1; };
    std::vector<DensityBlock> blocks_;
    std::vector<Bounds> block_bounds_;
    std::vector<size_t> order_;
    std::vector<Node> nodes_;
    Bounds bounds_{};
    double time_ = std::numeric_limits<double>::quiet_NaN();
    int cycle_ = -1;
    double peak_ = 0;
    Vec3 min_spacing_{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    double sample_impl(Vec3 point, Reconstruction mode, SamplingStats *stats) const;
    int build(size_t begin, size_t end);
    size_t locate(Vec3 point) const;
    void verify_overlaps(size_t block, int node) const;
};

}
