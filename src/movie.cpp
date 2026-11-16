#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "marching_cubes/movie.hpp"
#include "marching_cubes/storage.hpp"

namespace marching_cubes {
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {

bool is_link(fs::path const &p) { return fs::is_symlink(fs::symlink_status(p)); }
void reject_link(fs::path const &p) {
    if (is_link(p)) {
        throw std::runtime_error("Refusing symlink: " + p.string());
    }
}
void remove_owned_file(fs::path const &p) {
    reject_link(p);
    if (fs::exists(p)) {
        if (!fs::is_regular_file(p)) {
            throw std::runtime_error("Expected a regular file: " + p.string());
        }
        fs::remove(p);
    }
}
Json read_json(fs::path const &p) {
    reject_link(p);
    std::ifstream in(p);
    if (!in) {
        throw std::runtime_error("Cannot read " + p.string());
    }
    Json value;
    in >> value;
    return value;
}
struct Lock {
    int fd = -1;
    explicit Lock(fs::path const &p) {
        fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw std::runtime_error("Cannot open movie lock: " + p.string());
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
            throw std::runtime_error("Another process owns this movie scratch directory");
        }
    }
    ~Lock() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    Lock(Lock const &) = delete;
    Lock &operator=(Lock const &) = delete;
};
Json vjson(Vec3 const &v) { return Json::array({v.x, v.y, v.z}); }
Json configuration(MovieConfig const &o, SpacePlan const &plan,
                   std::vector<SourceSnapshot> const &selected) {
    auto const &f = o.frame;
    auto const &a = o.annotations;
    auto const &r = f.rendering;
    Json indices = Json::array();
    for (auto const &s : selected) {
        indices.push_back(s.index);
    }
    return {{"engine", "marching_cubes_native_movie_v1"},
            {"input_dir", fs::canonical(o.input_dir).string()},
            {"scratch", plan.scratch.string()},
            {"output", plan.result.string()},
            {"prefix", o.prefix},
            {"suffix", o.suffix},
            {"indices", indices},
            {"bounds_lo", vjson(f.bounds.lo)},
            {"bounds_hi", vjson(f.bounds.hi)},
            {"samples", f.samples},
            {"reference", f.reference},
            {"levels", f.levels},
            {"opacities", f.opacities},
            {"fields", f.fields},
            {"reconstruction",
             f.reconstruction == Reconstruction::trilinear ? "trilinear" : "constant"},
            {"camera_focus", vjson(r.camera.focus)},
            {"camera_normal", vjson(r.camera.normal)},
            {"camera_up", vjson(r.camera.up)},
            {"camera_scale", r.camera.scale},
            {"width", r.width},
            {"height", r.height},
            {"supersampling", r.supersampling},
            {"light", vjson(r.light_direction)},
            {"background", vjson(r.background)},
            {"initial_period_seconds", a.initial_period_seconds},
            {"time_unit_seconds", a.time_unit_seconds},
            {"length_unit_cm", a.length_unit_cm},
            {"density_unit_g_cm3", a.density_unit_g_cm3},
            {"solar_radius_cm", a.solar_radius_cm},
            {"ruler_length_cm", a.ruler_length_cm},
            {"title", a.title},
            {"zero_time_index0", o.zero_time_index0},
            {"allow_cropped", o.allow_cropped},
            {"fps", o.fps},
            {"bitrate", o.bitrate},
            {"pacing", "equal duration per selected snapshot; no physical-time interpolation"}};
}
Json stamp_json(FileStamp const &s) {
    return {{"path", s.path.string()}, {"size", s.size}, {"mtime_ticks", s.mtime_ticks}};
}
Json source_json(std::vector<Snapshot> const &sources) {
    Json list = Json::array();
    for (auto const &s : sources) {
        Json files = Json::array();
        for (auto const &f : s.files) {
            files.push_back(stamp_json(f));
        }
        list.push_back(
            {{"index", s.index},
             {"master", stamp_json(s.master)},
             {"files", files},
             {"raw_time", std::isfinite(s.raw_time) ? Json(s.raw_time) : Json(nullptr)}});
    }
    return list;
}
void verify_source(Snapshot const &snapshot) {
    auto verify = [](FileStamp const &s) {
        if (fs::canonical(s.path) != s.path || !fs::is_regular_file(s.path) ||
            fs::file_size(s.path) != s.size ||
            fs::last_write_time(s.path).time_since_epoch().count() != s.mtime_ticks) {
            throw std::runtime_error("Source changed during the movie run: " + s.path.string());
        }
    };
    verify(snapshot.master);
    for (auto const &s : snapshot.files) {
        verify(s);
    }
}
std::string frame_name(size_t index) {
    std::ostringstream text;
    text << "frame_" << std::setfill('0') << std::setw(6) << index;
    return text.str();
}
std::vector<SourceSnapshot> selection(MovieConfig const &o) {
    auto const found = discover(o.input_dir, o.prefix, o.suffix);
    if (found.empty()) {
        throw std::runtime_error("No numbered Silo masters found");
    }
    std::vector<size_t> indices;
    if (o.indices) {
        indices = *o.indices;
        if (indices.empty() || !std::is_sorted(indices.begin(), indices.end()) ||
            std::adjacent_find(indices.begin(), indices.end()) != indices.end()) {
            throw std::runtime_error("Indices must be nonempty, increasing and distinct");
        }
    } else {
        size_t const last = o.last.value_or(found.back().index);
        if (!o.stride || o.first > last) {
            throw std::runtime_error("Require stride >= 1 and first <= last");
        }
        if ((last - o.first) / o.stride + 1 > found.size()) {
            throw std::runtime_error("Requested range contains missing snapshot indices");
        }
        for (size_t i = o.first;;) {
            indices.push_back(i);
            if (last - i < o.stride) {
                break;
            }
            i += o.stride;
        }
    }
    std::vector<SourceSnapshot> result;
    for (size_t const index : indices) {
        auto it = std::lower_bound(found.begin(), found.end(), index,
                                   [](SourceSnapshot const &s, size_t i) { return s.index < i; });
        if (it == found.end() || it->index != index) {
            throw std::runtime_error("Missing snapshot index " + std::to_string(index));
        }
        result.push_back(*it);
    }
    return result;
}
Json timing(std::vector<Snapshot> const &sources, MovieConfig const &o) {
    Json out{{"pacing", "equal duration per selected snapshot; no physical-time interpolation"}};
    double previous = 0, low = std::numeric_limits<double>::infinity(), high = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        double const raw = o.zero_time_index0 && sources[i].index == 0 ? 0 : sources[i].raw_time;
        double const time = raw * o.annotations.time_unit_seconds;
        if (!std::isfinite(time)) {
            throw std::runtime_error("Finite time metadata is required for movie annotations");
        }
        if (!std::isfinite(time / o.annotations.initial_period_seconds)) {
            throw std::runtime_error("Time in initial orbital periods exceeds numeric range");
        }
        if (i) {
            double const delta = time - previous;
            if (!(delta > 0)) {
                throw std::runtime_error("Selected snapshot times must increase strictly");
            }
            low = std::min(low, delta);
            high = std::max(high, delta);
        }
        previous = time;
    }
    out["first_time_seconds"] =
        (o.zero_time_index0 && sources.front().index == 0 ? 0 : sources.front().raw_time) *
        o.annotations.time_unit_seconds;
    out["last_time_seconds"] = previous;
    if (sources.size() > 1) {
        out["minimum_interval_seconds"] = low;
        out["maximum_interval_seconds"] = high;
        out["uniform_within_one_percent"] = high <= 1.01 * low;
    }
    return out;
}
std::string gib(uint64_t bytes) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(3) << double(bytes) / 1073741824.;
    return s.str();
}
void print_plan(SpacePlan const &p, size_t count, size_t done, double fps) {
    std::cout << "\nStorage preflight: " << count << " snapshots, " << done << " reusable frames, "
              << double(count) / fps << " seconds of video\n"
              << "Scratch: " << p.scratch << "\nResults: " << p.result << '\n'
              << "Estimated remaining PNG/scratch budget: " << gib(p.estimated_frame_bytes)
              << " GiB\n"
              << "Estimated MP4/result budget: " << gib(p.estimated_movie_bytes) << " GiB\n";
    for (auto const &v : p.volumes) {
        std::cout << "Filesystem at " << v.path << ": need " << gib(v.payload_bytes) << " GiB + "
                  << gib(v.reserve_bytes) << " GiB reserve; available " << gib(v.available_bytes)
                  << " GiB: " << (v.enough ? "OK" : "INSUFFICIENT") << '\n';
    }
    std::cout << "WARNING: estimates are conservative, not reservations. Free space is checked "
                 "again while running.\n"
              << "Source SILOs are read-only; no source copies are written. PNG checkpoints remain "
                 "in local scratch.\n";
}
void check_frame_record(Json const &record, size_t sequence, size_t index, fs::path const &scratch,
                        std::array<size_t, 2> dims) {
    std::string const expected = "frames/" + frame_name(sequence) + ".png";
    if (!record.is_object() || record.at("snapshot_index").get<size_t>() != index ||
        record.at("png").get<std::string>() != expected) {
        throw std::runtime_error("Invalid frame checkpoint");
    }
    auto const path = scratch / expected;
    reject_link(path);
    if (!fs::is_regular_file(path) || file_hash(path) != record.at("png_hash").get<std::string>()) {
        throw std::runtime_error("Missing or damaged frame checkpoint");
    }
    auto const image = read_png(path.string());
    if (image.width != dims[0] || image.height != dims[1]) {
        throw std::runtime_error("Frame dimensions changed");
    }
    auto const &metadata = record.at("metadata");
    if (!metadata.contains("annotations") || !metadata.at("surfaces").is_array()) {
        throw std::runtime_error("Frame metadata is incomplete");
    }
}
void ensure_space(SpacePlan const &p) {
    if (!p.enough) {
        throw std::runtime_error(
            "Insufficient local space for remaining scratch, results and reserve");
    }
}
bool uninitialized_directory(fs::path const &path, bool allow_checkpoint_temps) {
    if (!fs::exists(path)) {
        return true;
    }
    if (!fs::is_directory(path)) {
        return false;
    }
    for (auto const &entry : fs::directory_iterator(path)) {
        auto const name = entry.path().filename().string();
        if (entry.is_symlink() || !entry.is_regular_file()) {
            return false;
        }
        if (name != ".movie.lock" &&
            !(allow_checkpoint_temps && name.starts_with("run.json.tmp."))) {
            return false;
        }
    }
    return true;
}
} // namespace

std::string file_hash(fs::path const &p) {
    reject_link(p);
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot checksum " + p.string());
    }
    uint64_t hash = 14695981039346656037ULL;
    std::array<char, 65536> bytes{};
    while (in) {
        in.read(bytes.data(), bytes.size());
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(bytes[size_t(i)]);
            hash *= 1099511628211ULL;
        }
    }
    if (!in.eof()) {
        throw std::runtime_error("Checksum read failed: " + p.string());
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}
void atomic_json(fs::path const &path, Json const &value) {
    // The temporary name is created exclusively, so an existing file or hard
    // link can never be truncated. Each writer owns only its own temporary.
    reject_link(path);
    std::string const contents = value.dump(2) + '\n';
    std::string const pattern = path.string() + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = ::mkstemp(name.data());
    if (fd < 0) {
        throw std::runtime_error("Cannot create checkpoint temporary: " +
                                 std::error_code(errno, std::generic_category()).message());
    }
    fs::path const temp(name.data());
    try {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            throw std::runtime_error("Cannot protect checkpoint descriptor");
        }
        size_t written = 0;
        while (written < contents.size()) {
            size_t const length =
                std::min(contents.size() - written,
                         static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
            ssize_t const n = ::write(fd, contents.data() + written, length);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                throw std::runtime_error("Checkpoint write failed: " + temp.string());
            }
            written += static_cast<size_t>(n);
        }
        if (::fsync(fd) != 0) {
            throw std::runtime_error("Cannot sync checkpoint");
        }
        int const close_status = ::close(fd);
        fd = -1;
        if (close_status != 0) {
            throw std::runtime_error("Cannot close checkpoint");
        }
        fs::rename(temp, path);
        int const dir = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) {
            throw std::runtime_error("Cannot open checkpoint directory for sync");
        }
        int const status = ::fsync(dir);
        ::close(dir);
        if (status != 0) {
            throw std::runtime_error("Cannot sync checkpoint directory");
        }
    } catch (...) {
        if (fd >= 0) {
            ::close(fd);
        }
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}

int make_movie(MovieConfig const &o) {
    validate_frame_config(o.frame);
    validate_annotation_layout(o.frame.rendering, o.annotations);
    for (double const level : o.frame.levels) {
        double const density = level * o.frame.reference * o.annotations.density_unit_g_cm3;
        if (!(density > 0) || !std::isfinite(density)) {
            throw std::runtime_error("Converted density contour exceeds numeric range");
        }
    }
    if (o.frame.no_render || o.frame.time_override) {
        throw std::runtime_error("Movie does not accept --no-render or a constant --time override");
    }
    if (!std::isfinite(o.fps) || o.fps <= 0 || o.fps > 240 || o.bitrate <= 0 ||
        o.bitrate > 1000000000) {
        throw std::runtime_error("Require 0 < fps <= 240 and 0 < bitrate <= 1e9 bits/s");
    }
    VideoOptions requested_video;
    requested_video.fps = o.fps;
    requested_video.bitrate = o.bitrate;
    validate_video_options(requested_video);
    auto const selected = selection(o);
    std::cout << "Inspecting " << selected.size() << " Silo masters and their referenced files..."
              << std::endl;
    auto const sources = inventory(selected, o.frame.fields);
    auto const sources_json = source_json(sources);
    auto const time_info = timing(sources, o);
    auto const dims = annotated_dimensions(o.frame.rendering.width, o.frame.rendering.height);
    auto plan = preflight(sources, o.scratch, o.output, selected.size(), dims[0], dims[1], o.fps,
                          uint64_t(o.bitrate), o.reserve_bytes);
    auto const config = configuration(o, plan, selected);
    auto const state_path = plan.scratch / "run.json";
    Json state, original_state;
    size_t done = 0;
    bool movie_complete = false;
    bool const have_checkpoint = o.resume && fs::exists(state_path);
    if (have_checkpoint) {
        state = read_json(state_path);
        original_state = state;
        if (state.at("schema") != 2 || state.at("config") != config) {
            throw std::runtime_error("Resume configuration mismatch; use the identical options or "
                                     "a new scratch/output directory");
        }
        if (state.at("sources") != sources_json) {
            throw std::runtime_error("Source files changed since the checkpoint; cannot resume");
        }
        if (!state.at("frames").is_array() || state.at("frames").size() != selected.size()) {
            throw std::runtime_error("Invalid frame checkpoint array");
        }
        reject_link(plan.scratch / "frames");
        for (size_t i = 0; i < selected.size(); ++i) {
            if (state["frames"][i].is_null()) {
                continue;
            }
            try {
                check_frame_record(state["frames"][i], i, selected[i].index, plan.scratch, dims);
                ++done;
            } catch (std::exception const &e) {
                std::cout << "Rebuilding snapshot " << selected[i].index << ": " << e.what()
                          << '\n';
                state["frames"][i] = nullptr;
            }
        }
        if (done == selected.size() && state.contains("movie") && state["movie"].is_object()) {
            auto const movie = plan.result / "movie.mp4";
            reject_link(movie);
            movie_complete = fs::is_regular_file(movie) &&
                             fs::file_size(movie) == state["movie"].at("bytes").get<uintmax_t>() &&
                             file_hash(movie) == state["movie"].at("hash").get<std::string>();
        }
    } else {
        for (auto const &p : {plan.scratch, plan.result}) {
            bool const safe = o.resume
                                  ? uninitialized_directory(p, p == plan.scratch)
                                  : (!fs::exists(p) || (fs::is_directory(p) && fs::is_empty(p)));
            if (!safe) {
                throw std::runtime_error("New movie directory must be absent or empty: " +
                                         p.string() + "; use --resume for existing work");
            }
        }
        state = {{"schema", 2},
                 {"config", config},
                 {"sources", sources_json},
                 {"frames", Json::array()},
                 {"stage", "rendering"},
                 {"movie", nullptr},
                 {"timing", time_info}};
        for (size_t i = 0; i < selected.size(); ++i) {
            state["frames"].push_back(nullptr);
        }
    }
    plan = preflight(sources, plan.scratch, plan.result, selected.size(), dims[0], dims[1], o.fps,
                     uint64_t(o.bitrate), o.reserve_bytes, done, movie_complete);
    print_plan(plan, selected.size(), done, o.fps);
    if (time_info.contains("uniform_within_one_percent") &&
        !time_info["uniform_within_one_percent"].get<bool>()) {
        std::cout << "WARNING: source time intervals vary; every selected snapshot gets equal "
                     "screen duration. Both physical time labels remain exact.\n";
    }
    ensure_space(plan);
    if (o.dry_run) {
        std::cout << "Dry run only; nothing written.\n";
        return 0;
    }
    std::cout << "Continue with this local-space plan? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
        std::cout << "Cancelled; no files written.\n";
        return 0;
    }
    auto cancelled = [&] { return o.cancelled && o.cancelled(); };
    if (cancelled()) {
        return 130;
    }
    fs::create_directories(plan.scratch);
    fs::create_directories(plan.result);
    Lock lock(plan.scratch / ".movie.lock");
    Lock result_lock(plan.result / ".movie.lock");
    if (have_checkpoint) {
        auto const current = read_json(state_path);
        if (current.at("config") != config || current.at("sources") != sources_json ||
            current.at("frames").size() != selected.size()) {
            throw std::runtime_error("Checkpoint changed while awaiting confirmation");
        }
        // If another run advanced it, require another invocation rather than overwriting progress.
        if (current != original_state) {
            throw std::runtime_error("Checkpoint changed concurrently");
        }
    } else if (fs::exists(state_path) || !uninitialized_directory(plan.scratch, o.resume) ||
               !uninitialized_directory(plan.result, false)) {
        throw std::runtime_error("Another run created files while awaiting confirmation");
    }
    ensure_space(preflight(sources, plan.scratch, plan.result, selected.size(), dims[0], dims[1],
                           o.fps, uint64_t(o.bitrate), o.reserve_bytes, done, movie_complete));
    reject_link(plan.scratch / "frames");
    fs::create_directories(plan.scratch / "frames");
    atomic_json(state_path, state);
    if (movie_complete) {
        atomic_json(plan.result / "manifest.json", state);
        std::cout << "Movie is already complete and verified: " << plan.result / "movie.mp4"
                  << '\n';
        return 0;
    }
    state["movie"] = nullptr;
    state["stage"] = "rendering";
    size_t newly_done = 0;
    for (size_t i = 0; i < selected.size(); ++i) {
        if (cancelled()) {
            atomic_json(state_path, state);
            std::cout << "Interrupted; resume with the same options and --resume.\n";
            return 130;
        }
        if (!state["frames"][i].is_null()) {
            continue;
        }
        verify_source(sources[i]);
        ensure_space(preflight(sources, plan.scratch, plan.result, selected.size(), dims[0],
                               dims[1], o.fps, uint64_t(o.bitrate), o.reserve_bytes, done, false));
        std::cout << "Snapshot " << selected[i].index << " (" << i + 1 << '/' << selected.size()
                  << "), " << done << " complete" << std::endl;
        auto f = o.frame;
        if (o.zero_time_index0 && selected[i].index == 0) {
            f.time_override = 0;
        }
        auto result = build_frame(selected[i].master, f);
        verify_source(sources[i]);
        if (cancelled()) {
            std::cout << "Interrupted; current frame will be rebuilt on resume.\n";
            return 130;
        }
        bool cropped = false;
        for (auto const &surface : result.metadata.at("surfaces")) {
            if (surface.at("nonmanifold_edges").get<size_t>() ||
                surface.at("inconsistent_winding_edges").get<size_t>()) {
                throw std::runtime_error(
                    "Invalid surface topology; stopped before checkpointing frame");
            }
            cropped = cropped || surface.at("boundary_edges").get<size_t>() ||
                      surface.at("boundary_samples_above").get<size_t>() ||
                      surface.at("vertices_outside_viewport").get<size_t>();
        }
        if (cropped && !o.allow_cropped) {
            throw std::runtime_error("Snapshot " + std::to_string(selected[i].index) +
                                     " reaches sampling bounds or camera edge; use a larger common "
                                     "box/view or explicitly --allow-cropped");
        }
        auto image = annotate(result.rgb, f.rendering.width, f.rendering.height, f.rendering,
                              result.metadata, o.annotations);
        result.metadata["annotations"] = image.metadata;
        result.metadata["cropped"] = cropped;
        auto const name = frame_name(i);
        auto const temporary = plan.scratch / "frames" / (name + ".tmp.png");
        auto const png = plan.scratch / "frames" / (name + ".png");
        remove_owned_file(temporary);
        reject_link(png);
        write_png(image, temporary.string());
        // A complete PNG is published first; an interrupted manifest update merely rebuilds this
        // frame.
        fs::rename(temporary, png);
        state["frames"][i] = {{"snapshot_index", selected[i].index},
                              {"png", "frames/" + name + ".png"},
                              {"png_hash", file_hash(png)},
                              {"metadata", result.metadata}};
        ++done;
        ++newly_done;
        atomic_json(state_path, state);
        if (o.stop_after && newly_done >= o.stop_after) {
            std::cout << "Stopped after " << newly_done << " new frames; " << done << '/'
                      << selected.size() << " saved. Rerun with --resume to continue.\n";
            return 0;
        }
    }
    ensure_space(preflight(sources, plan.scratch, plan.result, selected.size(), dims[0], dims[1],
                           o.fps, uint64_t(o.bitrate), o.reserve_bytes, done, false));
    std::vector<std::string> files;
    for (auto const &record : state.at("frames")) {
        files.push_back((plan.scratch / record.at("png").get<std::string>()).string());
    }
    state["stage"] = "encoding";
    atomic_json(state_path, state);
    auto const temp = plan.result / "movie.partial.mp4";
    remove_owned_file(temp);
    reject_link(plan.result / "movie.mp4");
    VideoOptions video;
    video.fps = o.fps;
    video.bitrate = o.bitrate;
    video.cancelled = cancelled;
    video.progress = [](size_t i, size_t n) {
        if (i == n || i % 50 == 0) {
            std::cout << "Encoding " << i << '/' << n << std::endl;
        }
    };
    try {
        encode_mp4(files, temp.string(), video);
    } catch (...) {
        if (cancelled()) {
            std::cout << "Encoding interrupted; all PNGs remain resumable.\n";
            return 130;
        }
        throw;
    }
    if (cancelled()) {
        return 130;
    }
    fs::rename(temp, plan.result / "movie.mp4");
    state["movie"] = {{"filename", "movie.mp4"},
                      {"bytes", fs::file_size(plan.result / "movie.mp4")},
                      {"hash", file_hash(plan.result / "movie.mp4")}};
    state["stage"] = "complete";
    atomic_json(state_path, state);
    atomic_json(plan.result / "manifest.json", state);
    std::cout << "Wrote " << selected.size() << " annotated frames and "
              << plan.result / "movie.mp4"
              << "\nKeep scratch for restart; it is not deleted automatically.\n";
    return 0;
}

} // namespace marching_cubes
