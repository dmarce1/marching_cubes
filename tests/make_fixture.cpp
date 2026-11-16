#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <silo.h>

namespace {

void check(bool success, char const *message) {
    if (!success) {
        throw std::runtime_error(message);
    }
}

void write_snapshot(std::filesystem::path const &path, int cycle, double time) {
    DBfile *file = DBCreate(path.string().c_str(), DB_CLOBBER, DB_LOCAL, "pipeline fixture", DB_HDF5);
    check(file != nullptr, "failed to create Silo fixture");
    try {
        check(DBMkDir(file, "block") >= 0 && DBSetDir(file, "block") >= 0, "failed to create block");
        constexpr int n = 16;
        std::array<double, n + 1> axis{};
        for (int i = 0; i <= n; ++i) {
            axis[i] = -2.0 + 4.0 * i / n;
        }
        double *coords[] = {axis.data(), axis.data(), axis.data()};
        int mesh_dims[] = {n + 1, n + 1, n + 1};
        int var_dims[] = {n, n, n};
        check(DBPutQuadmesh(file, "mesh", nullptr, coords, mesh_dims, 3, DB_DOUBLE,
            DB_COLLINEAR, nullptr) >= 0, "failed to write mesh");
        std::vector<double> a(n * n * n), b(n * n * n);
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    double const x = (axis[i] + axis[i + 1]) / 2;
                    double const y = (axis[j] + axis[j + 1]) / 2;
                    double const z = (axis[k] + axis[k + 1]) / 2;
                    double const rho = std::exp(-3 * (x*x + y*y + z*z));
                    size_t const index = static_cast<size_t>((k * n + j) * n + i);
                    a[index] = 0.75 * rho;
                    b[index] = 0.25 * rho;
                }
            }
        }
        check(DBPutQuadvar1(file, "rho_1", "mesh", a.data(), var_dims, 3, nullptr, 0,
            DB_DOUBLE, DB_ZONECENT, nullptr) >= 0, "failed to write rho_1");
        check(DBPutQuadvar1(file, "rho_2", "mesh", b.data(), var_dims, 3, nullptr, 0,
            DB_DOUBLE, DB_ZONECENT, nullptr) >= 0, "failed to write rho_2");
        check(DBSetDir(file, "/") >= 0, "failed to return to root");
        char const *meshes[] = {"/block/mesh"};
        char const *a_names[] = {"/block/rho_1"};
        char const *b_names[] = {"/block/rho_2"};
        int mesh_type[] = {DB_QUADMESH};
        int var_type[] = {DB_QUADVAR};
        DBoptlist *opts = DBMakeOptlist(2);
        check(opts != nullptr, "failed to create options");
        DBAddOption(opts, DBOPT_CYCLE, &cycle);
        DBAddOption(opts, DBOPT_DTIME, &time);
        int const result = DBPutMultimesh(file, "quadmesh", 1, meshes, mesh_type, opts);
        DBFreeOptlist(opts);
        check(result >= 0, "failed to write multimesh");
        check(DBPutMultivar(file, "rho_1", 1, a_names, var_type, nullptr) >= 0,
            "failed to write multivar rho_1");
        check(DBPutMultivar(file, "rho_2", 1, b_names, var_type, nullptr) >= 0,
            "failed to write multivar rho_2");
        int const close_result = DBClose(file);
        file = nullptr;
        check(close_result >= 0, "failed to close Silo fixture");
    } catch (...) {
        if (file) { DBClose(file); }
        throw;
    }
}

}

int main(int argc, char **argv) {
    try {
        check(argc == 2, "usage: make_fixture OUTPUT_DIRECTORY");
        std::filesystem::path const dir = argv[1];
        check(std::filesystem::create_directory(dir), "fixture directory already exists or cannot be created");
        write_snapshot(dir / "X.0.silo", 0, 4.49081e-97);
        write_snapshot(dir / "X.1.silo", 1, 0.5);
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "fixture: " << e.what() << '\n';
        return 1;
    }
}
