#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <marching_cubes/types.hpp>

namespace marching_cubes {

struct AnnotationConfig {
    double initial_period_seconds = 0;
    double time_unit_seconds = 1;
    double length_unit_cm = 1;
    double density_unit_g_cm3 = 1;
    double solar_radius_cm = 6.957e10;
    double ruler_length_cm = 0; // Zero chooses a 1/2/5 length automatically.
    std::string title;
};

struct AnnotatedImage {
    std::vector<unsigned char> rgb;
    size_t width = 0;
    size_t height = 0;
    nlohmann::json metadata;
};

std::array<size_t, 2> annotated_dimensions(size_t scene_width, size_t scene_height);
void validate_annotation_config(AnnotationConfig const &config);
// Checks dimensions, physical scale, and ruler fit before creating frames.
void validate_annotation_layout(RenderOptions const &render, AnnotationConfig const &config);
AnnotatedImage annotate(std::vector<unsigned char> const &rgb, size_t scene_width,
                        size_t scene_height, RenderOptions const &render,
                        nlohmann::json const &frame, AnnotationConfig const &config);
void write_png(AnnotatedImage const &image, std::string const &path);
AnnotatedImage read_png(std::string const &path);

struct VideoOptions {
    double fps = 24;
    int64_t bitrate = 6000000;
    // Return true to abort; no partial result should be published by the caller.
    std::function<bool()> cancelled;
    std::function<void(size_t, size_t)> progress;
};

// Checks rate representability, bitrate, and availability of libx264.
void validate_video_options(VideoOptions const &options);

// Writes directly to a new path, which the caller should atomically publish.
void encode_mp4(std::vector<std::string> const &png_files, std::string const &output,
                VideoOptions const &options);

} // namespace marching_cubes
