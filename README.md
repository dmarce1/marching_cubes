# marching_cubes

Standalone C++20 isodensity surfaces and lit movies from OctoTiger Silo output.
The compiled program handles Silo I/O, reconstruction, custom marching cubes,
smooth normals, transparent CPU rendering, labels, PNG checkpoints, and H.264
MP4 encoding. No Python, VisIt, VTK, subprocess encoder, or GPU is needed.

The style uses a black background and a directional light with specular
highlights. The default shells follow the density fractions and colors in
[D'Souza et al., Figure 3](https://arxiv.org/pdf/astro-ph/0512137#page=40):
0.5 green, 0.1 yellow, 1e-3 red, and 1e-5 blue, relative to a fixed initial
accretor density. Opacity and lighting are adjustable artistic choices; they
are not a recovered historical scene file.

## Build and test

Requires a C++20 compiler, CMake >= 3.20, pkg-config, Silo with HDF5 support,
libpng, nlohmann-json, OpenCV 4 (core/imgproc/imgcodecs), and FFmpeg development
libraries (libavformat/libavcodec/libavutil/libswscale) with the libx264 encoder.
All were available on this workstation. OpenCV draws text and reads/writes PNGs;
FFmpeg's C libraries encode directly inside the C++ process.

```sh
cd /work/dmarce1/marching_cubes
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j1
ctest --test-dir build --output-on-failure -j1
```

All movie stages use one CPU worker; the encoder uses one thread. CPU affinity
also constrains any dependency threads. With War Thunder ON the shared ceiling
is six CPU cores and 25% GPU; this project uses no GPU. Coordinate CPU ownership
with other jobs. The example below uses CPU 19, used by this chat for validation.

The pipeline integration test creates a tiny synthetic Silo set in `/tmp` and
uses the build filesystem for output. It skips if those are the same device,
because the production source-drive safeguard deliberately rejects that setup.
Other unit tests still run. All test fixtures and test code are C++.

## Run the complete q = 0.7 saved sequence

Use the outer q0.7 directory. It contains 274 masters, X.0.silo through
X.273.silo, and all 2,185 referenced external leaf files. The nested q0.7 copy
is incomplete. The program reads source files in place and processes one
snapshot at a time; it does not copy the approximately 288 GiB dataset.

```sh
taskset -c 19 /work/dmarce1/marching_cubes/build/marching_cubes movie \
  --input-dir /run/media/dmarce1/Marcello2/q0.7 \
  --scratch /work/dmarce1/marching_cubes/scratch/q07_full \
  --output /work/dmarce1/marching_cubes/output/q07_full \
  --first 0 --last 273 --fps 12 \
  --bounds=-1.5e10,1.5e10,-2.2e10,9e9,-4e9,4e9 \
  --samples 321,321,97 --reference 2136885.53387 \
  --focus 0,-6.5e9,0 --scale 1.7e10 --size 1024 --supersampling 2 \
  --initial-period 114.1409915 --zero-time-index0 \
  --time-unit-seconds 1 --length-unit-cm 1 --density-unit-g-cm3 1 \
  --title 'q = 0.7 binary merger'
```

The fixed wide view includes the extended final tidal tail. Sampling is a
moderate preview setting, not a resolution-convergence result: increase to
`--samples 641,641,193` for finer geometry at greater memory and runtime cost.
Increasing image size alone does not improve the extracted surface. Every
frame is checked for clipping, nonmanifold edges, and inconsistent winding.
The run stops at a failed frame; no silently cropped movie is published.
If an intermediate frame reaches the box or view boundary, use a new scratch
and output directory with enlarged bounds/scale, or knowingly use
`--allow-cropped`. A different geometry configuration cannot reuse old frames.

Before writing anything, the program lists the selected masters and their
referenced files, estimates remaining PNG scratch plus MP4/result storage,
compares the combined peak requirement on each destination filesystem with
available space, and leaves a reserve (default max(1 GiB, 10% of its budget)).
It displays a warning and asks **`Continue with this local-space plan? [y/N]`**.
Only `y` or `Y` proceeds. Enter, EOF, or any other answer cancels without writing.
There is no `--yes` bypass. Insufficient space is a hard failure. Add `--dry-run`
to the command for inventory and estimates only, without prompting or writing.

Scratch and results must be distinct, dedicated local directories, outside the
source tree and on different storage from the source. Device checks resolve
symlinks and reject the source filesystem, network filesystems, and shared
physical block devices where Linux sysfs exposes the backing disks. Detection
of unusual stacked filesystems is best effort; choose a known local scratch
disk. This workstation's /work NVMe storage and Marcello2 are distinct physical disks.
Both scratch and results may share the same local disk; space is combined.
Do not put unrelated files under the reserved artifact names in these directories.

Space is checked again before each frame and before encoding. Estimates are
conservative budgets, not reservations against other processes. Disk-full and
I/O errors leave completed checkpoints usable. `--reserve-gib` sets an explicit
positive reserve. A clean MP4 is only published after encoding finishes.

## Restart

Rerun the same command with **`--resume`** appended. Keep all data, rendering,
annotation, selection, and encoding options identical. The disk check and
`[y/N]` prompt run again, using remaining work rather than charging again for
already saved frames. `--stop-after N` intentionally saves N new frames and
stops before encoding; remove that option when resuming the full run.

`run.json` is an atomically replaced checkpoint. A complete PNG is published
before recording it. Resume checks its dimensions and FNV-1a content checksum,
reuses valid frames, and rebuilds missing or damaged ones. Source identity is
checked by canonical filename, size, and modification time; this is not a full
content checksum of the 288 GiB input. Changed source stamps or configuration
are rejected. SIGINT/SIGTERM stop at a safe stage boundary (a current Silo read
or frame calculation may finish first). SIGKILL can lose the current frame,
but saved frames survive. If interrupted during encoding, PNGs are reused and
the MP4 is encoded again. A verified completed movie is reused unchanged.
Locks prevent two processes using the same scratch or output at once.

Results contain `movie.mp4` and `manifest.json`. Scratch retains `run.json`,
`frames/frame_NNNNNN.png`, source provenance, and diagnostics. Scratch is never
automatically deleted. Keep it until satisfied with the result. Unique temporary
checkpoint files left by a hard kill are harmless. The dedicated reserved
PNG/partial-video names may be replaced when recovering damaged/incomplete work.

## Units, labels, and other datasets

Every movie frame includes time in initial orbital periods and minutes, a key
showing each shell's density in g/cm^3 and fraction of the fixed reference, and
a ruler labelled in cm and solar radii. Labels live outside the rendered scene.
The ruler uses the actual orthographic camera projection. Its nominal length
is rounded to a display pixel; both exact and rendered lengths are recorded.
The nominal solar radius is 6.957e10 cm
([IAU 2015 B3](https://www.iau.org/common/Uploaded%20files/IAUGA2015-Resolution-B3-recommended-nominal-conversion.pdf)).

- `--initial-period`: physical initial orbital period in **seconds**.
- `--time-unit-seconds`: seconds per stored master time unit.
- `--length-unit-cm`: cm per stored coordinate unit.
- `--density-unit-g-cm3`: g/cm^3 per stored density unit.
- `--reference`: fixed reference density in **stored units**, shared by all frames.
- `--ruler-cm`: explicit ruler length in cm; default chooses a useful 1/2/5 length.

For this historical run P0 = 114.1409915 s is inferred from retained initial
omega/unit information and agrees with later master/leaf time ratios. The
example treats master time as seconds and stored lengths/densities as cgs.
The first master has a bogus near-zero time; `--zero-time-index0` explicitly
sets it to zero and preserves the original in metadata. These are documented
physical-unit assumptions, not values guessed automatically by the renderer.
The last master is 1246.432726 s = 20.773879 min = 10.920114 P0. See the
[data inventory](docs/data_inventory.md) for the historical provenance caveat.

For another compatible dataset, change paths, prefix/suffix (default X./.silo),
range, fields, bounds, reference, and unit conversions. The reader expects a
`quadmesh` multimesh and named scalar, zone-centered multivars on disjoint
rectilinear AMR leaf blocks. `--fields` defaults to rho_1 through rho_5, summed.
`--first`, `--last`, and `--stride` select numbered masters; alternatively use
`--indices 0,200,273`. Missing selected numbers and nonincreasing times fail.
Each selected output gets equal screen duration at `--fps`; no simulation-time
interpolation is performed. Unequal time intervals produce a warning, while
the time labels always use actual source times.

`frame --input PATH --output NEW_DIRECTORY` uses the same geometry options and
can add annotations with `--initial-period`; `--time 0` overrides one timestamp.
`--obj` exports meshes and `--no-render --obj` exports only geometry/metadata.
`inspect --input PATH` reads one full dataset and prints its bounds, time, peak
density, and block count. `--help` lists all options. `--scale` means vertical
camera half-height; `--normal` points toward the observer and `--up` sets the
projected vertical direction. There is no automatic orbital rotation/tracking.
`--levels` and `--opacities` accept matching lists of one to four entries.

## Numerical method and limits

The program samples one global nodal lattice using a bounding-volume tree over
the AMR leaves. Trilinear reconstruction uses neighboring cell centers across
same-resolution block boundaries. A stencil meeting unequal cell widths falls
back to the owner's constant value; each frame records the fallback count.
`--reconstruction constant` uses the containing cell everywhere. Reconstruction
clamps to cell centers at the outer data boundary and rejects gaps.

Cubes use the twelve edge crossings with shared vertices. A bilinear face
decider resolves ambiguous face connectivity, loops are triangulated with
center fans, and shared-edge parity makes component orientation consistent.
Density gradients determine outward smooth normals, with face normals as a
fallback. Orthographic rasterization uses Blinn-Phong shading, per-pixel
transparency sorting, optional supersampling, and gamma encoding.

This is table-free marching cubes, not marching tetrahedra. It is **not MC33**:
not all trilinear interior ambiguities are resolved, and fan centers need not
lie exactly on the trilinear level set. Topology checks do not establish absence
of self-intersections. The coarse-fine fallback is not conservative high-order
AMR reconstruction. Small structures can disappear below lattice spacing.
There is no spatial smoothing; smooth shading does not improve geometric
resolution. Illumination is illustrative, not radiative transfer or ray-traced
shadows. Compare finer sampling before scientific interpretation.

See [validation](docs/validation.md) and [handoff](docs/handoff.md).

## Preview a movie while frames are still rendering

A separate native C++ `encode` command reads the completed consecutive frames
from one atomic snapshot of `run.json`. It can run while the renderer holds its
normal locks. It never modifies render checkpoints, rereads Silo density data,
or competes for the production movie filename.

```sh
taskset -c 18 /work/dmarce1/marching_cubes/build/marching_cubes encode \
  --scratch /work/dmarce1/marching_cubes/scratch/q07_full \
  --output /work/dmarce1/marching_cubes/output/q07_progress.mp4 \
  --replace
```

Run that command again whenever you want a newer preview. `--replace` permits
refreshing this chosen preview; the existing MP4 remains intact until a new
encode succeeds. Omit it to require a new output filename. FPS and bitrate are
inherited from the rendering run; use e.g. `--fps 4` to slow down a short preview.
No camera, density or unit options need to be repeated. `--dry-run` checks the
preview plan without writing, and the usual disk warning and `[y/N]` prompt
apply. The encoder uses one CPU core; CPU18 in the example is separate from the
renderer on CPU19, and both count toward the aggregate War Thunder budget.

Only recorded, complete PNGs are included, stopping at the first incomplete
record. Uncommitted and `.tmp.png` files are ignored. Frames completed after the
command starts appear on the next invocation. The exporter verifies checksums,
copies the selected PNGs into a unique local temporary directory beside the
preview, checks the copied bytes again, then encodes that stable set. Those
copies are included in the space estimate and removed on success or ordinary
failure. A SIGKILL can leave `.marching_cubes_preview.*` temporary directories;
remove only abandoned ones after confirming their encoder is no longer running.

The sidecar `q07_progress.mp4.json` records the exact snapshot indices, settings,
frame provenance, and video checksum. MP4 publication is atomic; MP4 and sidecar
are separate files, so a kill between their two publications can leave a stale
sidecar (its video hash exposes that). Rerunning `encode --replace` repairs it.
Concurrent exports to the same preview path are rejected by a separate lock.
The source drive need not be mounted, although checks of its recorded path's
surviving filesystem ancestor can conservatively reject a destination on that
same filesystem. Use the same distinct local storage as the render outputs.

At the documented one-core, 321 x 321 x 97 settings, tested frames took 23.86,
34.67 and 64.73 seconds (snapshots 0/200/273). A rough full-run budget is **3–5
hours** for 274 frames; this is an extrapolation, not a measured full-run time,
and uncached disk reads or contention can take longer. The rendered film is
22.83 seconds at 12 fps. Sampling/rendering remain serial: increasing taskset's
CPU list does not parallelize a frame. The preview encoder can run concurrently
on another core.
