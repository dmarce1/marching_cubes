# Implementation handoff

Owner: this Codex chat, 2026-11-16. Repo `/work/dmarce1/marching_cubes`,
branch `main`. User requested relocation to /work and GitHub publication on 2026-11-16.
User/editor-generated .project/.cproject/.clangd/.settings files are unrelated
and were left untouched. Source data on Marcello2 must remain read-only.

The full workflow is native C++20: direct Silo reader, global-grid reconstruction,
custom cube extraction, smooth normals, CPU transparent lit renderer, OpenCV
labels/PNG I/O, direct FFmpeg-library H.264/MP4 encoding, storage preflight,
default-N confirmation, atomic checkpoints and restart. There are no Python
source/tests/scripts left in this project and no VisIt dependency.

Latest explicit War Thunder report: ON. Shared budget <=6 CPU cores, <=25% GPU.
This chat used CPU19 for release builds/real data and CPU18 for coordinated
sanitizer/helper work, with no GPU use. Recoordinate before starting other work.
No QB login was authorized or attempted. Source data was never modified.

Current real-data output: output/annotated_preview/movie.mp4, sparse snapshots
0/200/273, physical labels and ruler. Scratch: scratch/annotated_preview.
A stop after the first frame followed by --resume verified checkpoint reuse.
Original early_preview and late_preview artifacts remain intact. See validation
for tests and actual timings. The entire 274-frame production movie has not been
rendered; README gives its tested dry-run command and restart instructions.

The documented wide camera/grid encloses the tested initial, later binary and
final remnant snapshots. Runtime guards stop on untested intermediate clipping.
It is a moderate sampling grid; finer geometry requires larger sample counts.
The method is not MC33 and uses constant fallback at unequal-width AMR stencils.
Physical unit and initial-period assumptions are documented in README/inventory.

Data inventory: outer /run/media/dmarce1/Marcello2/q0.7 has 274 masters and
2,185 referenced external leaves. Nested q0.7 copy is incomplete. Source times
span 0 (explicit override for bad first timestamp) to 1246.432726 seconds.
Initial period 114.1409915 seconds is inferred from retained initial-run data.

Prior historical context came from "Find Octotiger movie pipeline", chat
01a111fe-a690-7c42-b92c-501659a1fb75. No exact historical lighting scene was found;
current defaults are documented artistic choices informed by the group's figures.

Final verification: Release CTest 7/7 passed; ASan/UBSan CTest 7/7 passed
with leak detection disabled. Both pipeline SIGKILL checks ran, not skipped.
Build/test/preview processes have finished; CPU18/19 reservations are released.

Additional completed work: native `encode --scratch ... --output FILE.mp4
--replace` exports the completed consecutive checkpoint prefix while the main
renderer continues. It uses one manifest snapshot, bounded private PNG copies,
separate preview locking, disk checks/default-N confirmation, and atomic MP4
replacement. FPS/bitrate default to the render checkpoint; source Silo data need
not be reread. See README for the full command and sidecar publication caveat.
Actual data export is output/annotated_progress.mp4; render checkpoint/PNG hashes
were unchanged. Rendering a frame remains serial, as explained to the user;
no multi-core sampling or concurrent snapshot worker implementation was added.
This extension used CPU18 only and no GPU. Tests/builds finish before release
of its reservation. Latest explicit WT status remains ON (reconfirmed via the
user's cross-chat status update).

Extension final checks: Release 7/7 and ASan/UBSan 7/7 passed. All local work
from this chat has finished; CPU18 is released. No commit or push was made.

Relocation on 2026-11-16: the complete repository, IDE state, previews, scratch,
and Git metadata moved to /work/dmarce1/marching_cubes. The previous
/home/dmarce1/workspace/marching_cubes path is a compatibility symlink.
Operational checkpoint and preview metadata now use the canonical new root;
original JSON copies are preserved under scratch/relocation_2026_11_16.
Source Silo filenames and identity stamps, frames, and movies are unchanged.
Old generated build trees are preserved as build_before_relocation and
build_sanitize_before_relocation; active builds are regenerated at the new root.
User requested GitHub publication; generated data and local editor configuration
remain ignored. No project license has been selected.

Relocation checks completed: fresh Release 7/7 (9.60 s) and ASan/UBSan 7/7
(22.04 s, leak detection disabled). Resume and encode dry runs passed with all
three saved frames recognized. Source/test/IDE files and 23 PNG/MP4 artifacts
match the pre-move SHA-256 checksums. CPU18 work is finished and released.
GitHub repository: https://github.com/dmarce1/marching_cubes, public visibility,
origin git@github.com:dmarce1/marching_cubes.git; the user authorized the initial
commit and push on main. No full 274-frame render was started by this relocation.
