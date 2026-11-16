#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "marching_cubes/frame.hpp"
#include "marching_cubes/media.hpp"
#include "marching_cubes/movie.hpp"
#include "marching_cubes/storage.hpp"

namespace mc = marching_cubes;
namespace fs = std::filesystem;
namespace {
std::sig_atomic_t volatile interrupted = 0;
void signal_handler(int) { interrupted = 1; }
double number(std::string const &s) {
    size_t n = 0;
    double const value = std::stod(s, &n);
    if (n != s.size() || !std::isfinite(value)) {
        throw std::runtime_error("Expected finite number: " + s);
    }
    return value;
}
size_t integer(std::string const &s, size_t low, size_t high) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("Expected nonnegative integer: " + s);
    }
    auto const value = std::stoull(s);
    if (value < low || value > high) {
        throw std::runtime_error("Integer outside allowed range: " + s);
    }
    return size_t(value);
}
std::vector<std::string> split(std::string const &s) {
    if (s.empty() || s.back() == ',') {
        throw std::runtime_error("Empty list entry");
    }
    std::vector<std::string> result;
    std::istringstream stream(s);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (part.empty()) {
            throw std::runtime_error("Empty list entry");
        }
        result.push_back(part);
    }
    return result;
}
std::vector<double> numbers(std::string const &s, size_t count = 0) {
    std::vector<double> values;
    for (auto const &part : split(s)) {
        values.push_back(number(part));
    }
    if (count && values.size() != count) {
        throw std::runtime_error("Wrong number of comma-separated entries: " + s);
    }
    return values;
}
mc::Vec3 vec(std::string const &s) {
    auto const v = numbers(s, 3);
    return {v[0], v[1], v[2]};
}
void help() {
    std::cout << R"(marching_cubes: native C++ Silo surfaces, annotations and H.264/MP4 movies

marching_cubes inspect --input X.0.silo [--fields rho_1,...,rho_5]
marching_cubes frame --input X.0.silo --output NEW_DIRECTORY [frame options]
marching_cubes movie --input-dir SILO_DIRECTORY --scratch LOCAL_SCRATCH
  --output LOCAL_RESULTS --initial-period SECONDS [frame options] [movie options]
marching_cubes encode --scratch EXISTING_SCRATCH --output LOCAL_PREVIEW.mp4
  [--fps FPS] [--bitrate BITS_PER_SECOND] [--replace] [--dry-run] [--reserve-gib N]

encode snapshots completed consecutive checkpoint frames while rendering continues.
FPS/bitrate default to the rendering run. --replace atomically refreshes a preview;
the production movie and render scratch are protected. No SILO data is reread.

Required frame options:
  --bounds xmin,xmax,ymin,ymax,zmin,zmax --samples nx,ny,nz --reference DENSITY
Frame options:
  --levels 0.5,0.1,0.001,0.00001 --opacities 1,0.22,0.12,0.07
  --fields rho_1,rho_2,rho_3,rho_4,rho_5 --reconstruction trilinear|constant
  --focus x,y,z --scale CAMERA_HALF_HEIGHT --normal 0,0,1 --up 0,1,0
  --size 768 [or --width 1024 --height 768] --supersampling 1
  --light=-0.45,0.6,1 --background=0,0,0
  --obj --no-render --time VALUE     (frame command only)
Annotation options (frame requires --initial-period to enable annotations):
  --initial-period SECONDS --time-unit-seconds 1 --length-unit-cm 1
  --density-unit-g-cm3 1 --solar-radius-cm 6.957e10 --ruler-cm LENGTH --title TEXT
Movie options:
  --prefix X. --suffix .silo --first 0 --last N --stride 1
  --indices 0,100,273                 (instead of first/last/stride)
  --fps 24 --bitrate 6000000          (bits/s; one encoder thread)
  --zero-time-index0                 (explicitly repair bogus X.0 time)
  --resume                          (repeat identical data/render/encode options)
  --dry-run                         (inventory and disk estimate, no writes)
  --stop-after N                    (save N new frames, then stop before encoding)
  --reserve-gib N                    (default reserve: max(1 GiB, 10% of budget))
  --allow-cropped                    (allow surfaces to meet box/view boundary)

Movies prompt [y/N] after checking local space; only y or Y proceeds.
No --yes bypass. Scratch and results must be separate dedicated directories,
on local storage distinct from the source drive. PNG checkpoints are retained.
Time labels use actual Silo time; each selected snapshot has equal screen time.
Coordinates/reference are in stored units. Samples define a global nodal grid.
The renderer uses one CPU worker and no GPU. No Python or VisIt is used.
)";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            help();
            return 0;
        }
        std::string const command = argv[1];
        if (command != "inspect" && command != "frame" && command != "movie" &&
            command != "encode") {
            throw std::runtime_error("Unknown command " + command);
        }
        std::set<std::string> const flags{"--obj",     "--no-render",        "--resume",
                                          "--dry-run", "--zero-time-index0", "--allow-cropped",
                                          "--replace"};
        std::set<std::string> const encode_options{"--scratch",    "--output",  "--fps",
                                                   "--bitrate",    "--replace", "--dry-run",
                                                   "--reserve-gib"};
        std::set<std::string> const common{"--fields"};
        std::set<std::string> const frame_options{"--bounds",
                                                  "--samples",
                                                  "--reference",
                                                  "--levels",
                                                  "--opacities",
                                                  "--reconstruction",
                                                  "--focus",
                                                  "--scale",
                                                  "--normal",
                                                  "--up",
                                                  "--size",
                                                  "--width",
                                                  "--height",
                                                  "--supersampling",
                                                  "--light",
                                                  "--background",
                                                  "--initial-period",
                                                  "--time-unit-seconds",
                                                  "--length-unit-cm",
                                                  "--density-unit-g-cm3",
                                                  "--solar-radius-cm",
                                                  "--ruler-cm",
                                                  "--title",
                                                  "--output"};
        std::set<std::string> const movie_options{
            "--input-dir", "--scratch", "--prefix",           "--suffix",
            "--first",     "--last",    "--stride",           "--indices",
            "--fps",       "--bitrate", "--reserve-gib",      "--stop-after",
            "--resume",    "--dry-run", "--zero-time-index0", "--allow-cropped"};
        std::map<std::string, std::string> opts;
        for (int i = 2; i < argc; ++i) {
            std::string key = argv[i], value;
            if (key == "--help") {
                help();
                return 0;
            }
            auto const equal = key.find('=');
            if (equal != std::string::npos) {
                value = key.substr(equal + 1);
                key.resize(equal);
            }
            bool const allowed =
                command == "encode"
                    ? encode_options.count(key)
                    : common.count(key) || (command != "movie" && key == "--input") ||
                          (command != "inspect" && frame_options.count(key)) ||
                          (command == "movie" && movie_options.count(key)) ||
                          (command == "frame" &&
                           (key == "--obj" || key == "--no-render" || key == "--time"));
            if (!allowed) {
                throw std::runtime_error("Unknown option for " + command + ": " + key);
            }
            if (flags.count(key)) {
                if (equal != std::string::npos) {
                    throw std::runtime_error("Flag takes no value: " + key);
                }
                value = "true";
            } else if (equal == std::string::npos) {
                if (++i == argc) {
                    throw std::runtime_error("Missing value for " + key);
                }
                value = argv[i];
            }
            if (!opts.emplace(key, value).second) {
                throw std::runtime_error("Duplicate option " + key);
            }
        }
        auto get = [&](std::string const &key, std::string fallback = "") {
            if (opts.count(key)) {
                return opts.at(key);
            }
            if (fallback.empty()) {
                throw std::runtime_error("Required option " + key);
            }
            return fallback;
        };
        if (command == "encode") {
            mc::EncodeConfig options;
            options.scratch = get("--scratch");
            options.output = get("--output");
            if (opts.count("--fps")) {
                options.fps = number(get("--fps"));
            }
            if (opts.count("--bitrate")) {
                options.bitrate = int64_t(integer(get("--bitrate"), 1, 1000000000));
            }
            if (opts.count("--reserve-gib")) {
                long double const bytes =
                    static_cast<long double>(number(get("--reserve-gib"))) * 1073741824.L;
                if (!(bytes >= 1) ||
                    bytes >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
                    throw std::runtime_error("--reserve-gib outside positive byte range");
                }
                options.reserve_bytes = uint64_t(std::ceil(bytes));
            }
            options.replace = opts.count("--replace");
            options.dry_run = opts.count("--dry-run");
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            options.cancelled = [] { return interrupted != 0; };
            return mc::encode_checkpoint(options);
        }
        auto const fields = split(get("--fields", "rho_1,rho_2,rho_3,rho_4,rho_5"));
        if (command == "inspect") {
            auto const data = mc::SiloDataset::load(get("--input"), fields);
            auto const b = data.bounds();
            nlohmann::json const info{{"blocks", data.block_count()},
                                      {"density_peak", data.density_peak()},
                                      {"time", data.time()},
                                      {"cycle", data.cycle()},
                                      {"bounds", {b.lo.x, b.hi.x, b.lo.y, b.hi.y, b.lo.z, b.hi.z}}};
            std::cout << info.dump(2) << '\n';
            return 0;
        }
        mc::FrameConfig frame;
        auto const b = numbers(get("--bounds"), 6);
        frame.bounds = {{b[0], b[2], b[4]}, {b[1], b[3], b[5]}};
        auto const n = split(get("--samples"));
        if (n.size() != 3) {
            throw std::runtime_error("--samples requires nx,ny,nz");
        }
        for (size_t d = 0; d < 3; ++d) {
            frame.samples[d] = integer(n[d], 2, 2048);
        }
        frame.reference = number(get("--reference"));
        frame.levels = numbers(get("--levels", "0.5,0.1,0.001,0.00001"));
        frame.opacities = numbers(get("--opacities", "1,0.22,0.12,0.07"));
        frame.fields = fields;
        auto const reconstruction = get("--reconstruction", "trilinear");
        if (reconstruction != "trilinear" && reconstruction != "constant") {
            throw std::runtime_error("Unknown reconstruction");
        }
        frame.reconstruction = reconstruction == "trilinear"
                                   ? mc::Reconstruction::trilinear
                                   : mc::Reconstruction::piecewise_constant;
        auto &render = frame.rendering;
        if (opts.count("--size") && (opts.count("--width") || opts.count("--height"))) {
            throw std::runtime_error("Use --size or --width/--height");
        }
        render.width = integer(get("--width", get("--size", "768")), 16, 8192);
        render.height = integer(get("--height", get("--size", "768")), 16, 8192);
        render.supersampling = unsigned(integer(get("--supersampling", "1"), 1, 4));
        render.camera.focus =
            opts.count("--focus") ? vec(get("--focus")) : (frame.bounds.lo + frame.bounds.hi) / 2;
        render.camera.scale = opts.count("--scale") ? number(get("--scale"))
                                                    : 0.55 * std::max(b[1] - b[0], b[3] - b[2]);
        render.camera.normal = vec(get("--normal", "0,0,1"));
        render.camera.up = vec(get("--up", "0,1,0"));
        render.light_direction = vec(get("--light", "-0.45,0.6,1"));
        render.background = vec(get("--background", "0,0,0"));
        frame.no_render = opts.count("--no-render");
        if (opts.count("--time")) {
            frame.time_override = number(get("--time"));
        }
        mc::validate_frame_config(frame);
        mc::AnnotationConfig annotations;
        bool const labels = command == "movie" || opts.count("--initial-period");
        if (labels) {
            annotations.initial_period_seconds = number(get("--initial-period"));
        }
        annotations.time_unit_seconds = number(get("--time-unit-seconds", "1"));
        annotations.length_unit_cm = number(get("--length-unit-cm", "1"));
        annotations.density_unit_g_cm3 = number(get("--density-unit-g-cm3", "1"));
        annotations.solar_radius_cm = number(get("--solar-radius-cm", "6.957e10"));
        annotations.ruler_length_cm = number(get("--ruler-cm", "0"));
        annotations.title = opts.count("--title") ? get("--title") : "";
        if (labels) {
            mc::validate_annotation_layout(render, annotations);
        }
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        if (command == "movie") {
            mc::MovieConfig movie;
            movie.input_dir = get("--input-dir");
            movie.scratch = get("--scratch");
            movie.output = get("--output");
            movie.frame = frame;
            movie.annotations = annotations;
            movie.prefix = get("--prefix", "X.");
            movie.suffix = get("--suffix", ".silo");
            movie.first = integer(get("--first", "0"), 0, 1000000000);
            movie.stride = integer(get("--stride", "1"), 1, 1000000000);
            if (opts.count("--last")) {
                movie.last = integer(get("--last"), 0, 1000000000);
            }
            if (opts.count("--indices")) {
                if (opts.count("--first") || opts.count("--last") || opts.count("--stride")) {
                    throw std::runtime_error("--indices cannot combine with first/last/stride");
                }
                movie.indices = std::vector<size_t>{};
                for (auto const &s : split(get("--indices"))) {
                    movie.indices->push_back(integer(s, 0, 1000000000));
                }
            }
            movie.fps = number(get("--fps", "24"));
            movie.bitrate = int64_t(integer(get("--bitrate", "6000000"), 1, 1000000000));
            if (opts.count("--reserve-gib")) {
                long double const bytes =
                    static_cast<long double>(number(get("--reserve-gib"))) * 1073741824.L;
                if (!(bytes >= 1) ||
                    bytes >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
                    throw std::runtime_error("--reserve-gib outside positive byte range");
                }
                movie.reserve_bytes = uint64_t(std::ceil(bytes));
            }
            movie.stop_after = integer(get("--stop-after", "0"), 0, 1000000000);
            movie.resume = opts.count("--resume");
            movie.dry_run = opts.count("--dry-run");
            movie.zero_time_index0 = opts.count("--zero-time-index0");
            movie.allow_cropped = opts.count("--allow-cropped");
            movie.cancelled = [] { return interrupted != 0; };
            return mc::make_movie(movie);
        }
        bool const obj = opts.count("--obj");
        if (frame.no_render && !obj) {
            throw std::runtime_error("--no-render requires --obj");
        }
        auto const input = fs::canonical(get("--input"));
        auto const output = fs::absolute(get("--output"));
        auto const sources = mc::inventory({{0, input}}, fields);
        mc::validate_destination(sources, output);
        if (fs::exists(output) || fs::is_symlink(fs::symlink_status(output))) {
            throw std::runtime_error("Frame output already exists");
        }
        fs::create_directories(output);
        auto result =
            mc::build_frame(input, frame, obj ? std::optional<fs::path>(output) : std::nullopt);
        if (interrupted) {
            return 130;
        }
        if (!frame.no_render) {
            mc::AnnotatedImage image;
            if (labels) {
                image = mc::annotate(result.rgb, render.width, render.height, render,
                                     result.metadata, annotations);
                result.metadata["annotations"] = image.metadata;
            } else {
                image.rgb = std::move(result.rgb);
                image.width = render.width;
                image.height = render.height;
            }
            mc::write_png(image, (output / "frame.png").string());
        }
        mc::atomic_json(output / "frame.json", result.metadata);
        std::cout << "Wrote frame to " << output << '\n';
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "marching_cubes: " << e.what() << '\n';
        return 1;
    }
}
