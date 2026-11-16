#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "marching_cubes/silo_reader.hpp"
#include "marching_cubes/types.hpp"

namespace marching_cubes {

struct FrameConfig {
    Bounds bounds{};
    std::array<size_t, 3> samples{};
    double reference = 0;
    std::vector<double> levels{0.5, 0.1, 0.001, 0.00001};
    std::vector<double> opacities{1.0, 0.22, 0.12, 0.07};
    std::vector<std::string> fields{"rho_1", "rho_2", "rho_3", "rho_4", "rho_5"};
    Reconstruction reconstruction = Reconstruction::trilinear;
    RenderOptions rendering{};
    std::optional<double> time_override;
    bool no_render = false;
};

struct FrameResult {
    // RGB rows are top to bottom. Empty if no_render is true.
    std::vector<unsigned char> rgb;
    nlohmann::json metadata;
};

// Validates all configuration independent of the Silo file; throws on error.
void validate_frame_config(FrameConfig const &config);

// Caller owns output-directory creation and PNG encoding. If obj_directory is
// present it must already exist, and surface_N.obj paths must not exist.
// Individual levels may be empty; an all-empty set throws.
FrameResult build_frame(std::filesystem::path const &input, FrameConfig const &config,
                        std::optional<std::filesystem::path> obj_directory = std::nullopt);

} // namespace marching_cubes
