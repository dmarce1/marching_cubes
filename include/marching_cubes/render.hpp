#pragma once

#include <string>
#include <vector>

#include <marching_cubes/types.hpp>

namespace marching_cubes {

// RGB bytes, in row-major order, with the first row at the top.
std::vector<unsigned char> render_rgb(std::vector<Surface> const &surfaces,
    RenderOptions const &options);

// Throws on invalid inputs, allocation failure, or an output error.
void render_png(std::vector<Surface> const &surfaces, RenderOptions const &options,
    std::string const &path);

}
