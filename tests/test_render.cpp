#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <marching_cubes/render.hpp>

using namespace marching_cubes;

namespace {

void require(bool ok, char const *message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

Surface square(double z, Vec3 color, double opacity, double scale = 1) {
    Surface s;
    s.mesh.positions = {{-scale, -scale, z * scale}, {scale, -scale, z * scale},
        {scale, scale, z * scale}, {-scale, scale, z * scale}};
    s.mesh.normals.assign(4, {0, 0, 1});
    s.mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    s.material.color = color;
    s.material.opacity = opacity;
    s.material.specular = 0;
    return s;
}

RenderOptions options(double scale = 1) {
    RenderOptions o;
    o.width = 24;
    o.height = 24;
    o.camera.scale = scale * 2;
    return o;
}

std::array<unsigned char, 3> pixel(std::vector<unsigned char> const &image, size_t x, size_t y) {
    size_t const p = 3 * (y * 24 + x);
    return {image[p], image[p + 1], image[p + 2]};
}

} // namespace

int main() {
    try {
        Surface const red = square(-1, {1, 0, 0}, 1);
        Surface const blue = square(0, {0, 0, 1}, 1);
        Surface const green = square(1, {0, 1, 0}, 0.4);
        auto const o = options();
        auto const forward = render_rgb({red, blue, green}, o);
        auto const reverse = render_rgb({green, blue, red}, o);
        require(forward == reverse, "compositing must be independent of input order");
        auto const center = pixel(forward, 12, 12);
        require(center[0] == 0 && center[1] > 0 && center[2] > 0,
            "a transparent foreground must blend with the opaque surface behind it");
        require(forward == render_rgb({blue, green}, o), "opaque foreground must hide fragments behind it");
        require(pixel(forward, 12, 12) == pixel(forward, 11, 12),
            "adjacent coplanar triangles must not double-count their shared edge");
        require(pixel(forward, 0, 0) == std::array<unsigned char, 3>{0, 0, 0}, "background must be black");
        Surface tilted = blue;
        tilted.mesh.normals[0] = tilted.mesh.normals[3] = {1, 0, 0.2};
        auto const smooth = render_rgb({tilted}, o);
        require(pixel(smooth, 8, 12) != pixel(smooth, 15, 12),
            "interpolated vertex normals must affect lighting across a surface");
        auto const expected = render_rgb({square(0, {0.2, 0.3, 0.8}, 0.5)}, o);
        for (double scale : {1e-300, 1e-100, 1e100, 1e300}) {
            require(expected == render_rgb({square(0, {0.2, 0.3, 0.8}, 0.5, scale)}, options(scale)),
                "orthographic projection must be scale invariant");
        }
        RenderOptions antialias = o;
        antialias.supersampling = 2;
        require(render_rgb({blue}, antialias).size() == 24 * 24 * 3, "supersampling output dimensions");
        auto const filename = std::filesystem::temp_directory_path() /
            ("marching_cubes_render_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
        render_png({blue}, o, filename.string());
        std::ifstream file(filename, std::ios::binary);
        std::array<unsigned char, 8> signature{};
        file.read(reinterpret_cast<char *>(signature.data()), signature.size());
        require(file.gcount() == 8 && signature == std::array<unsigned char, 8>{137, 80, 78, 71, 13, 10, 26, 10},
            "PNG signature");
        file.close();
        std::filesystem::remove(filename);
        bool failed = false;
        try {
            render_png({blue}, o, "/this-directory-does-not-exist/image.png");
        } catch (std::runtime_error const &) {
            failed = true;
        }
        require(failed, "output error must be reported");
        failed = false;
        try {
            RenderOptions bad = o;
            bad.camera.scale = 0;
            render_rgb({blue}, bad);
        } catch (std::invalid_argument const &) {
            failed = true;
        }
        require(failed, "invalid camera must be rejected");
        std::cout << "renderer tests passed\n";
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "renderer test failed: " << e.what() << '\n';
        return 1;
    }
}
