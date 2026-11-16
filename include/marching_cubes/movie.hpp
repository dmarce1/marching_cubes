#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "marching_cubes/frame.hpp"
#include "marching_cubes/media.hpp"

namespace marching_cubes {

struct MovieConfig {
    std::filesystem::path input_dir, scratch, output;
    std::string prefix = "X.", suffix = ".silo";
    size_t first = 0, stride = 1;
    std::optional<size_t> last;
    std::optional<std::vector<size_t>> indices;
    FrameConfig frame;
    AnnotationConfig annotations;
    double fps = 24;
    int64_t bitrate = 6000000;
    uint64_t reserve_bytes = 0;
    bool zero_time_index0 = false;
    bool allow_cropped = false;
    bool resume = false, dry_run = false;
    size_t stop_after = 0;
    std::function<bool()> cancelled;
};

int make_movie(MovieConfig const &config);
struct EncodeConfig {
    std::filesystem::path scratch, output;
    std::optional<double> fps;
    std::optional<int64_t> bitrate;
    uint64_t reserve_bytes = 0;
    bool replace = false, dry_run = false;
    std::function<bool()> cancelled;
};
// Read one atomic checkpoint snapshot without locking or modifying the renderer.
int encode_checkpoint(EncodeConfig const &config);
void atomic_json(std::filesystem::path const &path, nlohmann::json const &value);
std::string file_hash(std::filesystem::path const &path);

} // namespace marching_cubes
