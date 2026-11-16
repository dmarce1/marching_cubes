#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "marching_cubes/frame.hpp"

namespace {
namespace fs = std::filesystem;
namespace mc = marching_cubes;

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::string pattern = (fs::temp_directory_path() / "marching_cubes_frame_XXXXXX").string();
        std::vector<char> chars(pattern.begin(), pattern.end());
        chars.push_back('\0');
        char *created = mkdtemp(chars.data());
        if (!created) {
            throw std::runtime_error("cannot create temporary frame fixture directory");
        }
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TemporaryDirectory(TemporaryDirectory const &) = delete;
    TemporaryDirectory &operator=(TemporaryDirectory const &) = delete;
};

void make_fixture(fs::path const &executable, fs::path const &output) {
    pid_t const child = fork();
    if (child < 0) {
        throw std::runtime_error("cannot fork fixture generator");
    }
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), output.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("fixture generator failed");
    }
}

void check(bool value, char const *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <typename Function> void rejects_invalid(Function fn) {
    try {
        fn();
    } catch (std::invalid_argument const &) {
        return;
    }
    throw std::runtime_error("expected invalid_argument");
}

mc::FrameConfig configuration() {
    mc::FrameConfig config;
    config.bounds = {{-2, -2, -2}, {2, 2, 2}};
    config.samples = {21, 21, 21};
    config.reference = 1;
    config.levels = {2, 0.3};
    config.opacities = {1, 0.4};
    config.fields = {"rho_1", "rho_2"};
    config.rendering.width = config.rendering.height = 64;
    config.rendering.camera.focus = {0, 0, 0};
    config.rendering.camera.scale = 2.2;
    config.time_override = 0;
    return config;
}

void test_preflight() {
    auto const valid = configuration();
    mc::validate_frame_config(valid);
    auto invalid = valid;
    invalid.samples = {2048, 2048, 2048};
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.samples[0] = 1;
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.bounds.lo.x = std::numeric_limits<double>::quiet_NaN();
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.reference = std::numeric_limits<double>::infinity();
    // The invalid config must be rejected before trying to open the nonexistent file.
    rejects_invalid([&] { mc::build_frame("/nonexistent/frame.silo", invalid); });
    invalid = valid;
    invalid.reference = std::numeric_limits<double>::max();
    invalid.levels[0] = std::numeric_limits<double>::max();
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.opacities.pop_back();
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.fields = {"rho_1", "rho_1"};
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.rendering.camera.up = invalid.rendering.camera.normal;
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
    invalid = valid;
    invalid.time_override = std::numeric_limits<double>::quiet_NaN();
    rejects_invalid([&] { mc::validate_frame_config(invalid); });
}

void test_fixture(fs::path const &input, fs::path const &obj) {
    auto config = configuration();
    mc::FrameResult const frame = mc::build_frame(input, config);
    auto const &meta = frame.metadata;
    check(frame.rgb.size() == 64 * 64 * 3, "incorrect RGB dimensions");
    check(meta.at("schema") == 1 && meta.at("source") == fs::absolute(input).string(),
          "incorrect provenance");
    check(meta.at("time") == 0 && meta.at("time_overridden") == true,
          "incorrect override metadata");
    check(std::abs(meta.at("raw_master_time").get<double>() / 4.49081e-97 - 1) < 1e-12,
          "raw master time not preserved");
    check(meta.at("fields").size() == 2 && meta.at("image_width") == 64,
          "incorrect configuration metadata");
    auto const &surfaces = meta.at("surfaces");
    check(surfaces.size() == 2 && surfaces.at(0).at("triangles") == 0 &&
              surfaces.at(1).at("triangles").get<size_t>() > 0,
          "empty surface reporting failed");
    check(surfaces.at(1).at("boundary_edges") == 0 && surfaces.at(1).at("nonmanifold_edges") == 0 &&
              surfaces.at(1).at("inconsistent_winding_edges") == 0 &&
              surfaces.at(1).at("boundary_samples_above") == 0 &&
              surfaces.at(1).at("vertices_outside_viewport") == 0,
          "incorrect surface diagnostics");
    config.no_render = true;
    check(fs::create_directory(obj), "expected fresh OBJ test directory");
    mc::FrameResult const geometry = mc::build_frame(input, config, obj);
    check(geometry.rgb.empty(), "no_render produced RGB");
    check(fs::is_regular_file(obj / "surface_0.obj") && fs::file_size(obj / "surface_1.obj") > 0,
          "OBJ files missing");
    rejects_invalid([&] { mc::build_frame(input, config, obj); });
    config.levels = {2, 3};
    bool rejected = false;
    try {
        mc::build_frame(input, config);
    } catch (std::runtime_error const &error) {
        rejected = std::string(error.what()).find("All isosurfaces empty") != std::string::npos;
    }
    check(rejected, "all-empty levels must fail");
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("usage: test_frame FIXTURE_EXECUTABLE");
        }
        test_preflight();
        TemporaryDirectory root;
        fs::path const input = root.path / "input";
        make_fixture(fs::canonical(argv[1]), input);
        test_fixture(input / "X.0.silo", root.path / "obj");
        std::cout << "frame API tests passed\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
