#pragma once

#include "marching_cubes/types.hpp"

namespace marching_cubes {

// Extract a triangulated level set from nodal, x-fastest scalar values.
// Values above isovalue are inside; normals point toward decreasing values.
// Zero gradients fall back to adjacent oriented face normals where available;
// singular or unused vertices can still have zero normals.
// Face-ambiguous cases use a shared bilinear asymptotic decider. Interior
// ambiguous cases are not resolved as MC33: each boundary loop is capped
// independently. Fan centers are geometric averages and need not lie on the
// exact trilinear level set; a fan need not follow the trilinear topology.
// Exact-level samples are treated as outside and can make a singular surface.
// Invalid dimensions, storage, geometry, values or isovalue throw; so does a
// mesh too large for its uint32_t triangle indices.
Mesh extract(ScalarGrid const &grid, double isovalue);

}
