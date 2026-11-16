# Validation

## Native C++ movie workflow, 2026-11-16

The current program has seven C++ CTest suites: extraction, Silo reader,
renderer, media, storage, frame API, and CLI pipeline. All seven passed in Release and
AddressSanitizer/UndefinedBehaviorSanitizer builds in the actual
repository, including the cross-filesystem pipeline test. Leak detection is
disabled for the sanitizer run because of the sandbox's earlier ptrace
limitation; no leak-check success is claimed. Dependencies used: GCC 15.2,
Silo 4.11, libpng 1.6.57, OpenCV 4.10, nlohmann-json 3.11.3 and FFmpeg 8.0.

New coverage includes physical time/density/ruler conversions, deterministic
annotations, preserved scene pixels, PNG roundtrip, real H.264 encoding,
confirmation default-N/EOF with no writes, dry-run with no writes, source-device
rejection, storage overflow and remaining-work accounting, changed source/config
rejection, corrupt-PNG recovery, completed-movie reuse, and atomic checkpoint
writes that preserve an unrelated hardlinked legacy temporary name. The pipeline
really kills a running process with SIGKILL and resumes its saved frame; this
subcheck ran successfully and was not skipped. It also tests safe recovery from
startup directories containing only lock files. Invalid ruler geometry and an
unrepresentable frame rate are rejected before prompting or creating output.
Synthetic source files are checked byte-for-byte and by size/mtime after tests.

The real-data test loaded all blocks from snapshots **0, 200 and 273** into one
fixed view. It used the exact 321 x 321 x 97 grid/camera documented for the full
run in README. After saving snapshot 0, `--stop-after 1` ended the process;
`--resume` reused its PNG with unchanged size, hash and modification time.
All twelve surfaces have zero boundary edges, nonmanifold edges, inconsistent
winding, above-level boundary samples and out-of-viewport vertices.

The output is `output/annotated_preview/movie.mp4`: H.264, 1368 x 1184,
three frames at 1 fps, 3 seconds. Both first and final annotated PNGs were
visually inspected. This is a sparse validation preview, **not the full
274-frame movie**. The last frame displays 10.9201 initial periods and
20.7739 minutes. The ruler represents 1e10 cm = 0.1437401 solar radii; its
301-pixel rasterized length differs from the exact 301.17647 pixels by 0.059%.

| Snapshot | Read (s) | Sample (s) | Extract/check (s) | Render (s) |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 2.75 | 18.89 | 2.13 | 0.08 |
| 200 | 12.70 | 19.72 | 2.15 | 0.10 |
| 273 | 32.12 | 29.12 | 2.96 | 0.52 |

These one-core timings depend on filesystem caching, AMR size and other activity;
they are not a full-run wall-time measurement. Source files were opened read-only;
all new results and scratch reside on the local /home filesystem.

A full `--dry-run` inventoried all **274 masters and their referenced leaves**.
For 1024-square scenes at 12 fps, the estimated remaining scratch is **1.542 GiB**,
MP4 budget **1.269 GiB**, and combined payload including result metadata
**2.827 GiB**, plus **1 GiB reserve**. About **33.56 GiB** was available when checked.
The estimate intentionally budgets far more than the compressed preview uses.
No production scratch/output directory was created by this dry run.

The full sequence is 22.8333 seconds at 12 fps. Only the selected snapshots above
have been rendered with this common view; the runtime guards will stop if an
untested intermediate frame is clipped or fails topology validation. No sampling
convergence or complete HDF5-content validation of every input has been claimed.

## Earlier extraction and renderer validation

The following measurements preceded the all-C++ movie integration. They document
the unchanged extraction/reader methods and the earlier close-up preview. The
legacy Python driver and Python test have since been removed from the project.

Validation date: 2026-11-16. GCC 15.2, CMake, Silo 4.11/HDF5, libpng, and
FFmpeg 8.0 on the local workstation. Builds and actual-data processing use one
CPU worker each under coordinated aggregate limits; rendering uses no GPU.

## Automated checks

- Extraction: a closed sphere, plane edge interpolation, exact-node plane,
  complemented-field normals/winding, deterministic extraction, expected
  ambiguous-face pairings for positive/negative/zero determinants, and an
  irregular-field regression requiring opposite directed uses of shared edges.
  Invalid dimensions, storage, and nonfinite data are rejected.
- Reader: real synthetic Silo files with noncubic dimensions, x-fast indexing,
  mixed float/double field sums, metadata, missing-field errors, affine continuity
  across same-level blocks, mixed-level constant preservation and fallback counts,
  overlap/gap/outside rejection.
- Renderer: opaque occlusion, transparent compositing and submission-order
  independence, shared-edge coverage, normal-dependent shading, black background,
  supersampling, PNG output/error handling, camera validation, and invariance
  under physical-coordinate scaling through 1e-300 and 1e300.
- Pipeline: synthetic Silo-to-PNG and two-frame MP4, provenance, explicit time-zero
  override, disappearing individual shell vs all-empty rejection, camera clipping
  and movie rejection, explicit snapshot ordering and empty-index validation.

The three C++ suites passed after the winding and interpolation-statistics
changes. The Python pipeline suite also passed with a real FFmpeg encode.

## First real snapshot

Source: outer `q0.7/X.0.silo`, all 4,908 leaf blocks loaded. Density is the sum of
`rho_1` through `rho_5`. Peak: 2,136,885.5338749476. Fixed reference:
2,136,885.53387. Grid: 201 × 126 × 126 nodes in x ±4e9, y/z ±2.5e9.
Camera: focus (4.8e8,0,0), half-height 3.8e9, overhead. PNG: 768 square,
2 × 2 supersampling. Raw master time is preserved; displayed time overridden to 0.

| Density fraction | Vertices | Triangles | Boundary edges | Nonmanifold edges |
| --- | ---: | ---: | ---: | ---: |
| 0.5 | 2,930 | 5,856 | 0 | 0 |
| 0.1 | 18,360 | 36,712 | 0 | 0 |
| 1e-3 | 44,508 | 89,008 | 0 | 0 |
| 1e-5 | 47,988 | 95,968 | 0 | 0 |

No samples on the box boundary were above a requested level. Initial single-core
run: read 5.51 s, sample 7.20 s, render 0.30 s. This is an example timing, not a
performance guarantee. Disk cache state and data size affect read time.

An early movie test rendered all consecutive snapshots 0–7 and encoded eight
frames at 4 fps, with fixed reference, camera, bounds, and sampling. This is a
two-second early preview, not the complete merger. The initial preview preceded
the strengthened winding, viewport, and provenance checks; final validation and
artifacts are recorded below as they complete.

## Interpretation limits

Zero boundary/nonmanifold counts do not prove correct trilinear interior topology,
absence of self-intersections, or geometric convergence. Face ambiguity handling
and shared vertices prevent the tested face cracks; this is not MC33. The reader
uses a constant fallback at unequal-width interpolation stencils. Both source AMR
resolution and the global sample lattice affect morphology. No quantitative
convergence study on this simulation has yet been completed.

## Final verification and late merger frame

The requested repository was built in place with Release settings; all four
CTest suites passed. A separate AddressSanitizer + UndefinedBehaviorSanitizer
Debug build also passed all four suites. LeakSanitizer could not run under the
execution sandbox (it reported its ptrace limitation), so that run used
`ASAN_OPTIONS=detect_leaks=0`; no leak-check success is claimed.

`X.273.silo` was loaded and rendered successfully from all 52,655 leaf blocks.
The peak density was 2,216,076.3789002853 and master time was
1,246.4327263607804. The wide view used x ±1.5e10, y from -2.2e10 to 9e9,
z ±4e9, and 321 × 321 × 97 sample nodes. The camera focus was (0,-6.5e9,0),
half-height 1.7e10; image 1024 square with 2 × 2 supersampling.

| Density fraction | Vertices | Triangles |
| --- | ---: | ---: |
| 0.5 | 582 | 1,160 |
| 0.1 | 2,024 | 4,044 |
| 1e-3 | 28,775 | 57,514 |
| 1e-5 | 304,255 | 608,398 |

All four shells have zero boundary edges, nonmanifold edges, inconsistent winding
edges, above-level samples on the box boundary, and vertices outside the camera
viewport. The image includes the extended tidal tail. Timing: read 113.84 s,
sample 29.24 s, extraction/diagnostics 2.92 s, render 0.59 s.

The late grid spacing is approximately (9.375e7, 9.6875e7, 8.3333e7), versus a
minimum source-cell width of approximately 3.9856e7. Thus the smallest AMR features
are not resolved by this preview grid. 600,123 of 9,994,977 samples (about 6.00%)
used the unequal-cell-width reconstruction fallback. These are lattice-wide
counts, not counts restricted to the isosurfaces.

The late snapshot rendering establishes that its density fields are readable;
it does not validate every field or every intervening output in the run.

The final repository binary regenerated snapshots 0–7 into
`output/early_preview/`. All 32 surfaces passed the boundary, nonmanifold,
winding, and viewport checks. The MP4 contains eight 768 × 768 frames at 4 fps
(two seconds). The stored times run from 0 to 32.06326896938886. Output intervals
range from 4.447436109697918 to 4.673562452004015; the driver reports this cadence
variation rather than assuming uniform physical time. The annotation uses the
inferred P0 described in README.

In the regenerated initial frame, 197,040 of 3,191,076 lattice samples used the
unequal-cell-width fallback (about 6.17%). All first-frame triangle counts above
remain unchanged after the winding correction.

## Concurrent preview export

The native `encode` subcommand was added and tested after the initial movie
workflow. It reads a fixed completed prefix without acquiring the renderer's
locks, checks PNG provenance and bounded copies, and atomically publishes a
separate preview MP4. The pipeline tests hold both production locks, advance the
checkpoint while confirmation is pending, omit original Silo sources, inject a
post-confirmation PNG failure, and verify successful and failed replacement.
They also verify ignored uncommitted PNGs, inherited/overridden FPS, no-write
confirmation/dry-run behavior, production-path protection, and unchanged render
checkpoints. The storage suite checks offline recorded-source destination guards.

The real-data export used scratch/annotated_preview and wrote
output/annotated_progress.mp4: three 1368 x 1184 H.264 frames, one FPS, three
seconds, snapshot indices 0/200/273. Independent ffprobe verified the stream.
SHA-256 checks before and after verified that run.json and all three source PNGs
were unchanged. The original Silo files were not opened for density reads.
MP4 publication is atomic; its optional JSON sidecar is published separately and
contains the video hash so an interrupted sidecar update can be detected.

Final extension verification: Release CTest 7/7 passed (19.39 s total);
ASan/UBSan CTest 7/7 passed (25.91 s), with leak detection disabled as above.

## Relocation verification, 2026-11-16

The complete project now lives at `/work/dmarce1/marching_cubes` on local NVMe
storage. The previous home-directory path is a compatibility symlink. Fresh
Release and ASan/UBSan builds at the new root each passed all seven CTest suites
(9.60 s and 22.04 s respectively), including the cross-filesystem pipeline test;
sanitizer leak detection remained disabled as described above.

SHA-256 comparisons preserved all source/test/IDE files and all 23 PNG/MP4
artifacts. Only the scratch/output paths in the operational checkpoint and
preview metadata changed; original JSON copies remain in ignored scratch.
A `movie --resume --dry-run` accepted the migrated configuration and reported
three reusable frames. The separate `encode --replace --dry-run` accepted all
three saved frames. Neither validation dry run wrote data or rerendered frames.
About 738 GiB was free on the destination during validation. Historical timing
and capacity measurements above describe the original location.
