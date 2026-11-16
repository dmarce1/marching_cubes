# marching_cubes

Standalone C++ Silo-to-isosurface movie pipeline. No VisIt or VTK extraction or
rendering dependency. Work on main unless another implementation chat owns it.
Coordinate changes through docs/handoff.md. Do not modify the source Silo data.

Use standard / dependency / project include groups separated by blank lines,
trailing const, unqualified size_t and braces around every loop body. Preserve
the configured QualifierAlignment: Right. Build and tests share the user's CPU
budget with every other chat; helpers may not independently start builds.

Tests must check geometric and rendering invariants. Preserve high physical
coordinate precision. Never disguise reconstruction or mesh smoothing as exact
preservation of the finite-volume data. State current topology limitations.
