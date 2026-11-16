# q = 0.7 Silo dataset inventory

The inspected dataset is on the mounted `Marcello2` drive at
`/run/media/dmarce1/Marcello2/q0.7`. The drive was inspected read-only. The
path and mount name are specific to the machine on which it was examined.

## Saved outputs

The outer directory contains 274 consecutively numbered master files,
`X.0.silo` through `X.273.silo`, with no missing numbers. An automated scan
of the master files found the expected external leaf filenames in every
master. `X.0.silo` references one leaf, `X.0.silo.data/0.silo`; each master
from `X.1.silo` through `X.273.silo` references eight numbered leaf files,
`0.silo` through `7.silo`, in its corresponding `.silo.data` directory. All
referenced files exist, are nonempty, and begin with an HDF5 signature. The
masters and numbered leaves total 309,168,654,200 bytes, approximately
287.94 GiB.

This check establishes filename continuity and file-level reference presence;
it does not establish that every HDF5 object and value in every file is
readable or scientifically valid. Extra hidden temporary files appear in a
few leaf directories and are not among the referenced numbered leaves.

The nested directory `q0.7/q0.7` has the same master numbering but is not a
complete copy: 128 of its numbered leaf directories are empty, and one has
six rather than eight leaf files. Use the outer directory.

The output sequence is complete **for the saved interval**. The retained
`job_script.sh.o834372` log reports that the job was killed after exceeding
its 72-hour wall-time limit, and its recorded `stop_time` was the default
approximately `1.8e308`. The files alone therefore do not establish that a
planned physical endpoint was reached. The retained `step.dat` extends to
step 33863 and a reported time of 71.31235 in its own time convention; it
must not be assumed to use the same convention as a Silo timestamp.

The outer directory also has a saved 274-frame `movie.mp4` and PNG sequence.
That movie is a pseudocolor density visualization, not the requested lit
isosurface movie. Its last seven PNGs, 267 through 273, are identical blank
white images. Their failure does not by itself establish a problem with the
corresponding Silo data.

## Mesh, variables, and time

The masters contain a `quadmesh` multimesh and density component multivars
including `rho_1` through `rho_5`; in the historical master, the derived
expression `rho` is the sum of those five components. Direct probes of the
first referenced block in both `X.0.silo` and `X.273.silo` found
three-dimensional rectilinear meshes with 9 nodes per axis and scalar,
zone-centered 8-by-8-by-8 density arrays. The probed arrays are double
precision with Silo strides 1, 8, 64: x is the fastest-varying index. Other
blocks have not all been individually probed by this inventory.

The first leaf mesh in `X.273.silo` reports cycle 273 and time
10.920113568741792. The master multimesh `dtime` reports
1246.4327263607804 for the same output; at cycle 266 the corresponding
values are 10.640785706156027 in the first leaf and 1214.5499636843997 in
the master. The latter matches the timestamp shown in the saved VisIt image.
The current OctoTiger writer uses rotation time in DWD leaf meshes and
physical time in the master, but the precise convention and units of the
2020 executable have not been independently verified against that historical
source. Do not silently interchange these numbers.

For `X.0.silo`, the first leaf reports time 0 and cycle 0. Direct reading of
the master `dtime` produced approximately `4.49e-97`, while its master
cycle component was not found. Treat the first master timestamp as suspect
and do not infer a physical time convention from that tiny value.

## Reader reconstruction

The standalone reader sums the selected zone-centered component fields,
checks rectilinear x-fast geometry, and requires nonoverlapping leaf blocks.
It constructs one global sample lattice for surface extraction. Its
piecewise-constant mode uses the containing leaf cell. Its trilinear mode
uses neighboring cell centers across same-resolution block boundaries; at a
coarse-fine interface, when the full same-resolution stencil is unavailable,
it falls back to the containing cell's value. Outer-domain samples are
clamped to the nearest available cell-center value. This fallback preserves a
constant field but is not a conservative, high-order AMR reconstruction and
may introduce visible changes at refinement boundaries. Surface geometry is
also dependent on the user-selected global lattice resolution and bounds.
