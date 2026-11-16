#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <silo.h>
#include <unistd.h>

#include "marching_cubes/silo_reader.hpp"

using namespace marching_cubes;

namespace {
void require(bool p, std::string const &message) { if (!p) { throw std::runtime_error(message); } }
void near(double a, double b, std::string const &message) {
    require(std::abs(a - b) < 1e-10, message + ": got " + std::to_string(a) + " expected " + std::to_string(b));
}
template<class F> void throws(F fn, std::string const &message) {
    try { fn(); } catch (std::exception const &) { return; }
    throw std::runtime_error(message);
}
DensityBlock block(double x0, double x1, size_t nx, double y0, double y1, size_t ny,
    double z0, double z1, size_t nz, bool affine) {
    DensityBlock b;
    std::array<double, 3> const lo{x0, y0, z0}, hi{x1, y1, z1};
    std::array<size_t, 3> const dims{nx, ny, nz};
    for (size_t d = 0; d < 3; ++d) {
        for (size_t i = 0; i <= dims[d]; ++i) { b.edges[d].push_back(lo[d] + (hi[d] - lo[d]) * i / dims[d]); }
    }
    for (size_t k = 0; k < nz; ++k) {
        for (size_t j = 0; j < ny; ++j) {
            for (size_t i = 0; i < nx; ++i) {
                double const x = (b.edges[0][i] + b.edges[0][i+1]) / 2;
                double const y = (b.edges[1][j] + b.edges[1][j+1]) / 2;
                double const z = (b.edges[2][k] + b.edges[2][k+1]) / 2;
                b.values.push_back(affine ? 1 + 2*x + 3*y + 4*z : 7);
            }
        }
    }
    return b;
}
void check_silo(int r, std::string const &what) { require(r >= 0, what); }
void fixture(std::filesystem::path const &path) {
    DBfile *f = DBCreate(path.string().c_str(), DB_CLOBBER, DB_LOCAL, "reader test", DB_HDF5);
    require(f, "create Silo fixture");
    for (int n = 0; n < 2; ++n) {
        std::string const dir = std::string("b") + std::to_string(n);
        check_silo(DBMkDir(f, dir.c_str()), "mkdir");
        check_silo(DBSetDir(f, dir.c_str()), "setdir");
        double x[] = {double(n), n + 0.5, double(n+1)}, y[] = {0, 1, 2, 3}, z[] = {0, 1, 2};
        double *coords[] = {x,y,z};
        int md[] = {3,4,3}, vd[] = {2,3,2};
        check_silo(DBPutQuadmesh(f,"mesh",nullptr,coords,md,3,DB_DOUBLE,DB_COLLINEAR,nullptr), "put mesh");
        float a[12]; double b[12];
        for (int k = 0; k < 2; ++k) {
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 2; ++i) {
                    size_t const q = size_t((k*3+j)*2+i);
                    a[q] = float(n*10 + k*4 + j*2 + i); b[q] = 0.25;
                }
            }
        }
        check_silo(DBPutQuadvar1(f,"a","mesh",a,vd,3,nullptr,0,DB_FLOAT,DB_ZONECENT,nullptr), "put a");
        check_silo(DBPutQuadvar1(f,"b","mesh",b,vd,3,nullptr,0,DB_DOUBLE,DB_ZONECENT,nullptr), "put b");
        check_silo(DBSetDir(f,".."), "return root");
    }
    char const *meshes[] = {"/b0/mesh", "/b1/mesh"};
    char const *as[] = {"/b0/a", "/b1/a"}, *bs[] = {"/b0/b", "/b1/b"};
    int types[] = {DB_QUADMESH,DB_QUADMESH}, vartypes[] = {DB_QUADVAR,DB_QUADVAR};
    int cycle = 123; double time = 5.5;
    DBoptlist *opts = DBMakeOptlist(2);
    DBAddOption(opts, DBOPT_DTIME, &time); DBAddOption(opts, DBOPT_CYCLE, &cycle);
    check_silo(DBPutMultimesh(f,"quadmesh",2,meshes,types,opts), "put multimesh");
    check_silo(DBPutMultivar(f,"a",2,as,vartypes,nullptr), "put multivar a");
    check_silo(DBPutMultivar(f,"b",2,bs,vartypes,nullptr), "put multivar b");
    DBFreeOptlist(opts); DBClose(f);
}
}
int main() {
    try {
        SiloDataset same({block(0,1,2, 0,1,2, 0,1,2, true), block(1,2,2, 0,1,2, 0,1,2, true)});
        require(std::isnan(same.time()), "unspecified time must remain unknown");
        for (double x : {0.8, 0.999, 1.0, 1.001, 1.2}) {
            near(same.sample({x, 0.5, 0.5}, Reconstruction::trilinear), 1 + 2*x + 3*0.5 + 4*0.5, "same-level seam affine");
        }
        near(same.sample({1.0,0.25,0.25}, Reconstruction::piecewise_constant), 1 + 2*1.25 + 3*0.25 + 4*0.25, "boundary owns right block");
        throws([&]{ same.sample({-0.1,0.5,0.5}, Reconstruction::piecewise_constant); }, "outside accepted");
        auto g = same.resample({{0,0,0},{2,1,1}}, {3,3,3}, Reconstruction::trilinear);
        near(g.values[g.index(1,1,1)], 6.5, "resampling x-fast grid");
        SamplingStats same_stats;
        same.resample({{0,0,0},{2,1,1}}, {9,5,5}, Reconstruction::trilinear, &same_stats);
        require(same_stats.coarse_fine_fallback_samples == 0, "same-level fallback count");
        SiloDataset mixed({block(0,1,2, 0,1,2, 0,1,2, false), block(1,2,4, 0,1,4, 0,1,4, false)});
        for (double x : {0.9, 1.0, 1.1}) { near(mixed.sample({x,0.5,0.5}, Reconstruction::trilinear), 7, "mixed resolution constant"); }
        SamplingStats mixed_stats;
        auto mixed_grid = mixed.resample({{0,0,0},{2,1,1}}, {9,5,5}, Reconstruction::trilinear, &mixed_stats);
        require(mixed_stats.coarse_fine_fallback_samples > 0, "mixed-level fallback not counted");
        require(mixed_stats.coarse_fine_fallback_samples <= mixed_grid.values.size(), "count exceeds sample count");
        size_t const prior_count = mixed_stats.coarse_fine_fallback_samples;
        mixed.resample({{0,0,0},{2,1,1}}, {9,5,5}, Reconstruction::piecewise_constant, &mixed_stats);
        require(mixed_stats.coarse_fine_fallback_samples == prior_count, "constant mode changed fallback count");
        mixed.resample({{0,0,0},{2,1,1}}, {9,5,5}, Reconstruction::trilinear, &mixed_stats);
        require(mixed_stats.coarse_fine_fallback_samples == 2 * prior_count, "statistics should accumulate");
        throws([&]{ SiloDataset bad({block(0,1,2,0,1,2,0,1,2,false),block(0.5,1.5,2,0,1,2,0,1,2,false)}); }, "overlap accepted");
        SiloDataset gap({block(0,1,2,0,1,2,0,1,2,false),block(2,3,2,0,1,2,0,1,2,false)});
        throws([&]{gap.sample({1.5,0.5,0.5}, Reconstruction::piecewise_constant);}, "gap accepted");
        auto const p = std::filesystem::temp_directory_path() / ("marching_cubes_silo_" + std::to_string(getpid()) + ".silo");
        fixture(p);
        try {
            auto data = SiloDataset::load(p.string(), {"a","b"});
            require(data.block_count() == 2, "block count"); near(data.time(),5.5,"master time"); require(data.cycle()==123,"master cycle");
            near(data.sample({1.75,2.5,1.5}, Reconstruction::piecewise_constant), 19.25, "stored axes and variable sum");
            throws([&]{SiloDataset::load(p.string(), {"missing"});}, "missing variable accepted");
            std::filesystem::remove(p);
        } catch (...) { std::filesystem::remove(p); throw; }
        std::cout << "Silo reader tests passed\n";
    } catch (std::exception const &e) { std::cerr << e.what() << '\n'; return 1; }
}
