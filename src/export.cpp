#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
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
void reject_link(fs::path const &path) {
    if (fs::is_symlink(fs::symlink_status(path))) {
        throw std::runtime_error("Refusing symlink: " + path.string());
    }
}
bool within(fs::path const &child, fs::path const &parent) {
    auto a = child.begin();
    for (auto b = parent.begin(); b != parent.end(); ++b, ++a) {
        if (a == child.end() || *a != *b) {
            return false;
        }
    }
    return true;
}
fs::path ancestor(fs::path path) {
    while (!fs::exists(path)) {
        path = path.parent_path();
    }
    return path;
}
uint64_t bytes(long double n) {
    if (!std::isfinite(n) || n < 0 ||
        n >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        throw std::runtime_error("Preview storage estimate overflow");
    }
    return static_cast<uint64_t>(std::ceil(n));
}
std::string gib(uint64_t n) {
    std::ostringstream s;
    s << std::fixed << std::setprecision(3) << double(n) / 1073741824.;
    return s.str();
}
void check_space(fs::path const &parent, uint64_t required) {
    if (fs::space(ancestor(parent)).available < required) {
        throw std::runtime_error("Insufficient local space for preview copies, MP4 and reserve");
    }
}
void bounded_copy(fs::path const &source, fs::path const &target, uint64_t expected,
                  fs::path const &parent, uint64_t reserve) {
    // The renderer normally never changes committed PNGs. Still cap a copy at
    // its budgeted size if a file is externally damaged or replaced mid-export.
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(target, std::ios::binary);
    if (!in || !out) {
        throw std::runtime_error("Cannot copy preview PNG");
    }
    std::array<char, 65536> buffer{};
    uint64_t remaining = expected;
    while (remaining) {
        auto const length =
            static_cast<std::streamsize>(std::min<uint64_t>(remaining, buffer.size()));
        in.read(buffer.data(), length);
        if (in.gcount() != length) {
            throw std::runtime_error("PNG size changed while copying");
        }
        check_space(parent, reserve + static_cast<uint64_t>(length));
        out.write(buffer.data(), length);
        if (!out) {
            throw std::runtime_error("Cannot write preview PNG copy");
        }
        remaining -= static_cast<uint64_t>(length);
    }
    char extra;
    if (in.get(extra) || !in.eof()) {
        throw std::runtime_error("PNG size changed while copying");
    }
    out.close();
    if (!out) {
        throw std::runtime_error("Cannot finish preview PNG copy");
    }
}
void check_target(fs::path const &path, bool replace) {
    reject_link(path);
    if (fs::exists(path) && (!replace || !fs::is_regular_file(path))) {
        throw std::runtime_error(
            "Preview output already exists; use --replace for a regular file: " + path.string());
    }
}
struct ExportLock {
    int fd = -1;
    explicit ExportLock(fs::path const &path) {
        fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            throw std::runtime_error("Cannot open preview lock");
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
            throw std::runtime_error("Another encoder is writing this preview");
        }
    }
    ~ExportLock() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    ExportLock(ExportLock const &) = delete;
    ExportLock &operator=(ExportLock const &) = delete;
};
struct TemporaryDirectory {
    fs::path path;
    explicit TemporaryDirectory(fs::path const &parent) {
        std::string pattern = (parent / ".marching_cubes_preview.XXXXXX").string();
        std::vector<char> chars(pattern.begin(), pattern.end());
        chars.push_back(0);
        if (!::mkdtemp(chars.data())) {
            throw std::runtime_error("Cannot create preview scratch directory");
        }
        path = chars.data();
    }
    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TemporaryDirectory(TemporaryDirectory const &) = delete;
    TemporaryDirectory &operator=(TemporaryDirectory const &) = delete;
};
void publish(fs::path const &temporary, fs::path const &output, bool replace) {
    reject_link(output);
    if (replace) {
        fs::rename(temporary, output);
    } else {
        // Same filesystem: link publishes without overwriting a concurrently created name.
        if (::link(temporary.c_str(), output.c_str()) != 0) {
            throw std::runtime_error("Cannot publish new preview without overwriting " +
                                     output.string());
        }
        fs::remove(temporary);
    }
}
} // namespace

int encode_checkpoint(EncodeConfig const &o) {
    if (o.scratch.empty() || o.output.empty()) {
        throw std::runtime_error("Encode requires --scratch and --output FILE.mp4");
    }
    fs::path const scratch = fs::canonical(o.scratch);
    fs::path const checkpoint = scratch / "run.json";
    reject_link(checkpoint);
    reject_link(scratch / "frames");
    // Atomic rename by the producer makes this one read a consistent version.
    // Never reread it while copying/encoding: later frames belong to the next export.
    Json snapshot;
    {
        std::ifstream stream(checkpoint);
        if (!stream) {
            throw std::runtime_error("Cannot open movie checkpoint");
        }
        stream >> snapshot;
    }
    if (snapshot.at("schema") != 2 || !snapshot.at("frames").is_array()) {
        throw std::runtime_error("Unsupported movie checkpoint");
    }
    auto const &config = snapshot.at("config");
    auto const &planned = config.at("indices");
    if (!planned.is_array() || planned.size() != snapshot.at("frames").size()) {
        throw std::runtime_error("Invalid checkpoint selection");
    }
    auto const dims =
        annotated_dimensions(config.at("width").get<size_t>(), config.at("height").get<size_t>());
    VideoOptions video;
    video.fps = o.fps.value_or(config.at("fps").get<double>());
    video.bitrate = o.bitrate.value_or(config.at("bitrate").get<int64_t>());
    video.cancelled = o.cancelled;
    validate_video_options(video);
    Json frames = Json::array(), indices = Json::array();
    std::vector<fs::path> pngs;
    std::vector<uint64_t> sizes;
    long double copy_bytes = 0;
    for (size_t i = 0; i < planned.size(); ++i) {
        auto const &record = snapshot.at("frames")[i];
        if (record.is_null()) {
            break;
        }
        std::ostringstream expected;
        expected << "frames/frame_" << std::setw(6) << std::setfill('0') << i << ".png";
        if (record.at("snapshot_index") != planned[i] || record.at("png") != expected.str()) {
            throw std::runtime_error("Invalid committed frame path or order");
        }
        fs::path const path = scratch / expected.str();
        reject_link(path);
        if (!fs::is_regular_file(path) ||
            file_hash(path) != record.at("png_hash").get<std::string>()) {
            throw std::runtime_error(
                "Missing or damaged committed PNG; resume rendering to repair it: " +
                path.string());
        }
        auto const image = read_png(path.string());
        if (image.width != dims[0] || image.height != dims[1]) {
            throw std::runtime_error("Committed PNG dimensions changed");
        }
        sizes.push_back(fs::file_size(path));
        copy_bytes += sizes.back();
        pngs.push_back(path);
        frames.push_back(record);
        indices.push_back(planned[i]);
    }
    if (frames.empty()) {
        throw std::runtime_error("No completed consecutive frames are available yet");
    }
    fs::path const requested = fs::absolute(o.output).lexically_normal();
    if (requested.extension() != ".mp4") {
        throw std::runtime_error("Preview output must end in .mp4");
    }
    reject_link(requested);
    std::vector<fs::path> source_paths;
    for (auto const &source : snapshot.at("sources")) {
        source_paths.emplace_back(source.at("master").at("path").get<std::string>());
        for (auto const &file : source.at("files")) {
            source_paths.emplace_back(file.at("path").get<std::string>());
        }
    }
    fs::path const parent = validate_export_destination(source_paths, requested.parent_path());
    fs::path const output = parent / requested.filename();
    fs::path const manifest = output.string() + ".json";
    fs::path const production =
        fs::weakly_canonical(fs::path(config.at("output").get<std::string>()) / "movie.mp4");
    if (within(output, scratch) || output == production) {
        throw std::runtime_error(
            "Preview must be outside render scratch and cannot replace the production movie");
    }
    check_target(output, o.replace);
    check_target(manifest, o.replace);
    // Budget temporary PNG copies as well as a conservative full new encoded file;
    // an existing preview remains allocated until the replacement is ready.
    long double const raw = static_cast<long double>(dims[0]) * dims[1] * 3 + dims[1];
    long double const rgb_budget = frames.size() * (raw * 1.01L + 65536);
    long double const rate_budget =
        (1.5L * frames.size() / video.fps + 2) * video.bitrate / 8 + 16 * 1048576.L;
    uint64_t const movie_bytes = bytes(std::max(rgb_budget, rate_budget));
    uint64_t const metadata_bytes = bytes(2.L * snapshot.dump().size() + 1048576);
    uint64_t const payload = bytes(copy_bytes + movie_bytes + metadata_bytes);
    uint64_t const reserve =
        o.reserve_bytes ? o.reserve_bytes : std::max<uint64_t>(1073741824, bytes(payload / 10.L));
    uint64_t const required = bytes(static_cast<long double>(payload) + reserve);
    std::cout << "Captured " << frames.size() << '/' << planned.size()
              << " completed consecutive frames, " << frames.size() / video.fps << " seconds at "
              << video.fps << " fps\n"
              << "Preview: " << output
              << "\nTemporary copies + new MP4 + metadata: " << gib(payload) << " GiB + "
              << gib(reserve) << " GiB reserve; available "
              << gib(fs::space(ancestor(parent)).available) << " GiB\n"
              << "WARNING: estimates are not reservations. The renderer keeps running; newly "
                 "completed frames are included on the next invocation.\n";
    check_space(parent, required);
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
    fs::create_directories(parent);
    ExportLock lock(output.string() + ".lock");
    check_target(output, o.replace);
    check_target(manifest, o.replace);
    validate_export_destination(source_paths, parent);
    check_space(parent, required);
    TemporaryDirectory temp(parent);
    std::vector<std::string> copies;
    long double remaining = copy_bytes;
    for (size_t i = 0; i < pngs.size(); ++i) {
        if (cancelled()) {
            return 130;
        }
        check_space(parent, bytes(remaining + movie_bytes + metadata_bytes + reserve));
        fs::path const copy = temp.path / pngs[i].filename();
        reject_link(pngs[i]);
        bounded_copy(pngs[i], copy, sizes[i], parent, reserve);
        if (file_hash(copy) != frames[i].at("png_hash").get<std::string>()) {
            throw std::runtime_error("Frame changed while copying; preview was not replaced");
        }
        remaining -= sizes[i];
        copies.push_back(copy.string());
    }
    video.progress = [&](size_t i, size_t n) {
        check_space(parent, reserve);
        if (i == n || i % 50 == 0) {
            std::cout << "Encoding preview " << i << '/' << n << std::endl;
        }
    };
    fs::path const temporary_movie = temp.path / "movie.mp4";
    try {
        encode_mp4(copies, temporary_movie.string(), video);
    } catch (...) {
        if (cancelled()) {
            return 130;
        }
        throw;
    }
    if (cancelled()) {
        return 130;
    }
    Json const report{{"schema", 1},
                      {"kind", "partial_movie"},
                      {"source_checkpoint", checkpoint.string()},
                      {"frames_total_planned", planned.size()},
                      {"frame_count", frames.size()},
                      {"snapshot_indices", indices},
                      {"frames", frames},
                      {"fps", video.fps},
                      {"bitrate", video.bitrate},
                      {"width", dims[0]},
                      {"height", dims[1]},
                      {"video_bytes", fs::file_size(temporary_movie)},
                      {"video_hash", file_hash(temporary_movie)}};
    atomic_json(temp.path / "manifest.json", report);
    check_target(output, o.replace);
    check_target(manifest, o.replace);
    publish(temporary_movie, output, o.replace);
    publish(temp.path / "manifest.json", manifest, o.replace);
    std::cout << "Wrote preview with " << frames.size() << " frames: " << output
              << "\nRender checkpoints and source data were not changed.\n";
    return 0;
}
} // namespace marching_cubes
