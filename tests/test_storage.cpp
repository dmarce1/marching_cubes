#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <silo.h>
#include <sys/stat.h>
#include <unistd.h>

#include "marching_cubes/storage.hpp"

namespace fs = std::filesystem;
using namespace marching_cubes;
namespace {
void require(bool condition, std::string const &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class F> void must_throw(F fn, std::string const &message) {
    try {
        fn();
    } catch (std::exception const &) {
        return;
    }
    throw std::runtime_error(message);
}
void create_master(fs::path const &path, std::string const &leaf, double time) {
    DBfile *db = DBCreate(path.string().c_str(), DB_CLOBBER, DB_LOCAL, "storage fixture", DB_HDF5);
    require(db, "create fixture master");
    std::string const mesh = leaf + ":/block/quadmesh";
    std::string const rho = leaf + ":/block/rho_1";
    char const *meshes[] = {mesh.c_str()}, *vars[] = {rho.c_str()};
    int mt[] = {DB_QUADMESH}, vt[] = {DB_QUADVAR};
    DBoptlist *opts = DBMakeOptlist(1);
    DBAddOption(opts, DBOPT_DTIME, &time);
    bool const ok = DBPutMultimesh(db, "quadmesh", 1, meshes, mt, opts) >= 0 &&
                    DBPutMultivar(db, "rho_1", 1, vars, vt, nullptr) >= 0;
    DBFreeOptlist(opts);
    DBClose(db);
    require(ok, "write fixture master");
}
struct Temp {
    fs::path path;
    Temp() {
        path = fs::temp_directory_path() / ("mc_storage_test_" + std::to_string(getpid()));
        require(fs::create_directory(path), "create temp directory");
    }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
} // namespace
int main() {
    try {
        Temp temp;
        auto const source = temp.path / "source";
        fs::create_directory(source);
        {
            std::ofstream out(source / "leaf.silo");
            out << "exists";
        }
        create_master(source / "X.10.silo", "leaf.silo", 7.5);
        create_master(source / "X.2.silo", "leaf.silo", 2.5);
        auto found = discover(source);
        require(found.size() == 2 && found[0].index == 2 && found[1].index == 10,
                "numeric discovery order");
        {
            std::ofstream dup(source / "X.02.silo");
            dup << 'x';
        }
        must_throw([&] { (void)discover(source); }, "duplicate indices accepted");
        fs::remove(source / "X.02.silo");
        auto snapshots = inventory(found, {"rho_1"});
        require(snapshots.size() == 2 && snapshots[0].files.size() == 1, "inventory leaf refs");
        require(snapshots[0].files[0].path == fs::canonical(source / "leaf.silo"),
                "inventory canonical leaf");
        require(std::abs(snapshots[0].raw_time - 2.5) < 1e-14, "raw master time");
        must_throw([&] { (void)inventory(found, {"missing"}); }, "missing field accepted");
        fs::remove(source / "leaf.silo");
        must_throw([&] { (void)inventory(found, {"rho_1"}); }, "missing external leaf accepted");
        {
            std::ofstream out(source / "leaf.silo");
            out << "exists";
        }
        snapshots = inventory(found, {"rho_1"});
        must_throw(
            [&] {
                (void)preflight(snapshots, temp.path / "scratch", temp.path / "results", 2, 32, 32,
                                25, 5000000);
            },
            "source filesystem accepted");
        must_throw([&] { (void)validate_destination(snapshots, temp.path / "scratch"); },
                   "single destination source filesystem accepted");
        fs::create_directory_symlink(source, temp.path / "alias");
        must_throw(
            [&] {
                (void)preflight(snapshots, temp.path / "alias" / "scratch", temp.path / "result", 2,
                                32, 32, 25, 5000000);
            },
            "source symlink accepted");
        must_throw(
            [&] {
                (void)preflight(snapshots, temp.path / "scratch", temp.path / "scratch" / "result",
                                2, 32, 32, 25, 5000000);
            },
            "overlapping destinations accepted");
        must_throw(
            [&] {
                (void)preflight(snapshots, temp.path / "scratch", temp.path / "result", 2, 32, 32,
                                0.0, 5000000);
            },
            "zero fps accepted");
        must_throw(
            [&] {
                (void)preflight(snapshots, temp.path / "scratch", temp.path / "result", 2, 32, 32,
                                25, 5000000, 0, 3);
            },
            "too many completed frames accepted");
        // Export accepts missing source files and never opens them; a missing
        // source tree still protects its path and nearest existing filesystem.
        std::vector<fs::path> const offline{source / "missing" / "tree" / "X.0.silo"};
        must_throw([&] { (void)validate_export_destination({}, temp.path / "out"); },
                   "empty recorded source list accepted");
        must_throw(
            [&] {
                (void)validate_export_destination(offline, source / "missing" / "tree" / "out");
            },
            "offline source tree accepted");
        must_throw([&] { (void)validate_export_destination(offline, temp.path / "out"); },
                   "offline source ancestor filesystem accepted");
        must_throw(
            [&] {
                (void)validate_export_destination({source / "leaf.silo"},
                                                  temp.path / "alias" / "out");
            },
            "export source symlink accepted");
        struct stat stmp{}, shome{};
        fs::path const home = "/home";
        if (::stat(temp.path.c_str(), &stmp) == 0 && ::stat(home.c_str(), &shome) == 0 &&
            stmp.st_dev != shome.st_dev) {
            // Preflight is read-only: these are prospective paths and are never created.
            fs::path const prefix = home / ("mc_storage_plan_" + std::to_string(getpid()));
            require(validate_destination(snapshots, prefix / "scratch") ==
                        fs::weakly_canonical(prefix / "scratch"),
                    "valid single destination");
            require(validate_export_destination(offline, prefix / "movie") ==
                        fs::weakly_canonical(prefix / "movie"),
                    "offline source on other filesystem was rejected");
            auto full = preflight(snapshots, prefix / "scratch", prefix / "results", 2, 320, 240,
                                  29.97, 5000000, 1024);
            require(full.same_device && full.volumes.size() == 1,
                    "shared target volume accounting");
            require(full.volumes[0].reserve_bytes == 1024, "explicit reserve");
            require(full.estimated_movie_bytes >= 2 * full.estimated_png_bytes, "raw movie bound");
            auto resumed = preflight(snapshots, prefix / "scratch", prefix / "results", 2, 320, 240,
                                     29.97, 5000000, 1024, 1);
            require(resumed.volumes[0].payload_bytes < full.volumes[0].payload_bytes,
                    "resume space accounting");
            require(resumed.estimated_frame_bytes < full.estimated_frame_bytes,
                    "remaining scratch estimate");
            auto completed = preflight(snapshots, prefix / "scratch", prefix / "results", 2, 320,
                                       240, 29.97, 5000000, 1024, 2, true);
            require(completed.volumes[0].payload_bytes < resumed.volumes[0].payload_bytes,
                    "completed movie accounting");
            must_throw(
                [&] {
                    (void)preflight(snapshots, prefix / "scratch", prefix / "results", 2,
                                    std::numeric_limits<size_t>::max(), 240, 25, 5000000);
                },
                "size overflow accepted");
            {
                std::ofstream out(source / "leaf.silo", std::ios::app);
                out << "changed";
            }
            must_throw(
                [&] {
                    (void)preflight(snapshots, prefix / "scratch", prefix / "results", 2, 32, 32,
                                    25, 5000000);
                },
                "changed source accepted");
        }
        std::cout << "Storage tests passed\n";
    } catch (std::exception const &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
