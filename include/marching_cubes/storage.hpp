#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace marching_cubes {

struct SourceSnapshot {
    size_t index;
    std::filesystem::path master;
};
struct FileStamp {
    std::filesystem::path path; // Canonical, symlinks resolved.
    uintmax_t size;
    int64_t mtime_ticks; // std::filesystem::file_time_type clock ticks.
};
struct Snapshot {
    size_t index;
    FileStamp master;
    std::vector<FileStamp> files; // Unique external references; master is separate.
    double raw_time;              // Master multimesh dtime; NaN if absent.
};

// Discovery sorts numerically and rejects duplicate numeric indices.
std::vector<SourceSnapshot> discover(std::filesystem::path const &directory,
                                     std::string const &prefix = "X.",
                                     std::string const &suffix = ".silo");
// Inspects master metadata only; never opens referenced leaf data contents.
std::vector<Snapshot> inventory(std::vector<SourceSnapshot> const &selected,
                                std::vector<std::string> const &fields);

// Resolves symlinks and validates a prospective local directory against all
// source trees and filesystem devices. Block-device ancestry is best effort
// for stacked filesystems when the kernel exposes their topology in sysfs.
std::filesystem::path validate_destination(std::vector<Snapshot> const &sources,
                                           std::filesystem::path const &destination);

// Validate a directory against recorded original source file paths, without
// opening or requiring the source files. For missing paths, device checks use
// the nearest existing parent and may conservatively reject a safe destination.
// A disconnected drive cannot be identified from its old path alone; physical
// device ancestry and symlink resolution are best effort for unavailable paths.
// Supply original simulation files, not the frame PNGs being exported.
std::filesystem::path
validate_export_destination(std::vector<std::filesystem::path> const &source_files,
                            std::filesystem::path const &destination);

struct SpaceVolume {
    std::filesystem::path path;
    uint64_t payload_bytes = 0;
    uint64_t reserve_bytes = 0;
    uint64_t required_bytes = 0;
    uint64_t available_bytes = 0;
    bool enough = false;
};
struct SpacePlan {
    std::filesystem::path scratch;
    std::filesystem::path result;
    bool same_device = false;
    uint64_t estimated_frame_bytes = 0; // Remaining scratch payload, including a partial frame.
    uint64_t estimated_png_bytes = 0;   // Upper estimate for one PNG.
    uint64_t estimated_movie_bytes = 0;
    std::vector<SpaceVolume> volumes;
    bool enough = false;
};

// Both destinations must be on local filesystems distinct from all source
// devices; source ancestry, destination overlap and symlinks are checked.
// bitrate is bits per second; the video estimate requires an enforced limit.
// An explicit nonzero reserve overrides the default of max(1 GiB, 10%).
SpacePlan preflight(std::vector<Snapshot> const &sources, std::filesystem::path const &scratch,
                    std::filesystem::path const &result, size_t frames, size_t width, size_t height,
                    double fps, uint64_t bitrate, uint64_t reserve_bytes = 0,
                    size_t completed_frames = 0, bool completed_movie = false);

} // namespace marching_cubes
