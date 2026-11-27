# GeoSolverBegin and GeoSolverEnd

Tolerance: none recorded.

A geometry solver runs the nodes between one or more GeoSolverBegin nodes and a GeoSolverEnd
below them once per frame, each time starting from the result of the frame before. Any chain of
geometry nodes between them becomes a simulation: a body that moves points by their velocity
makes them fly, a body that adds a point makes a trail.

## Pairing

The solver belongs to the GeoSolverEnd. Walking up from its Source input, every GeoSolverBegin
reached opens the solver. A GeoSolverEnd met on the way closes a solver nested inside this one, so
the GeoSolverBegins above it belong to that solver. A GeoSolverBegin that no GeoSolverEnd closes,
or that several do, outputs its input and shows a warning.

## GeoSolverBegin

- **Source** is the geometry this node works on.
- **Method** chooses what it outputs to the body:
  - **Prev Frame**: the result of the frame before, or of the substep before when there are
    substeps. Its Source is what the simulation starts from: the first solved frame starts from
    Source at Start Frame.
  - **Input**: Source at the time being solved, to bring live geometry into the simulation, such
    as a collider that moves or an emitter whose shape is animated.

Several GeoSolverBegins may open one solver, each with its own Method. For example, one fetches
the frame before and another the current shape of a collision object, which a GeoWrangle reads
on its second input. When several are set to Prev Frame, they all output the same result, and
the simulation starts from the Source of the first by script name. A solver with no
GeoSolverBegin set to Prev Frame is an error.

Outside a cook of the solver, for example when a GeoSolverBegin is viewed on its own, it shows
its Source.

## GeoSolverEnd

- **Source** is the end of the body.
- **Start Frame** is where the simulation starts. Up to and including Start Frame, the output is
  the Source of the Prev Frame GeoSolverBegin, unchanged, at the frame being shown. The frame
  after Start Frame is the first one solved.
- **Substeps** is how many times the body cooks per frame. Each substep starts from the substep
  before and covers 1 / Substeps of a frame. More substeps make fast motion and stiff forces
  steadier.
- **Reset Simulation** forgets every solved frame, so the next frame shown solves again from
  Start Frame.
- **Cache Simulation** keeps every solved frame, so playing back or scrubbing shows it without
  solving it again. Off, only the last solved frame is kept: going forward one frame at a time
  stays quick, and going back solves again from Start Frame.
- **Cache Memory** is the RAM in MB the solved frames of this node may hold. Past it, the least
  recently used frames are written to the disk cache directory and read back when they are
  shown again. 0 keeps only the frame just solved in RAM.
- **Spill To Disk** writes the frames that do not fit in Cache Memory to disk. Off, they are
  dropped and solved again when they are shown.
- **Loop Name** names the `loops.<name>.*` variables this solver sets. Empty uses the node's
  script name.

## Solving frames

A frame is solved from the frame before it, so showing frame 100 of a simulation nobody has
looked at solves frames 1 to 100 in order. A frame already solved is shown from the cache. A
frame that is not starts from the closest earlier frame that is, and solves forward from there.
Jumping far ahead can take a while; moving to another frame, or any edit, cancels it, and the
frames solved before the cancel are kept, so coming back resumes where it stopped. A frame
cancelled half-way is never kept.

A time between two frames shows the frame before it: the simulation only has whole frames.

Any edit above the GeoSolverEnd, in the body or in the inputs of the GeoSolverBegins, and any
change to Start Frame or Substeps, starts the simulation over from Start Frame. The output of the
solver changes from frame to frame even when nothing above it is animated.

When two viewers ask for different frames of one simulation at once, one solves and the other
waits for the frames it needs. A geometry cache writing frames below a solver writes them one at
a time and in order, so each frame solves from the one before.

The kept frames belong to one simulation each: a solver inside a GeoForEach loop keeps the
frames of each iteration apart, so each iteration simulates on its own.

## Variables

Each cook of the body sets these, also as `loops.<name>.*`:

- `loop.substep`: the substep being cooked, from 0.
- `loop.numsubsteps`: Substeps.
- `loop.dt`: the frames one substep covers, 1 / Substeps.

The body cooks at the time the substep ends: at frame f with 4 substeps, at f - 0.75, f - 0.5,
f - 0.25 and f. Animated parameters in the body read their value at that time.

In a GeoWrangle, `@TimeInc` is the time one substep covers in seconds, `loop.dt` divided by the
project frame rate, so `@P += @v * @TimeInc` moves points by a velocity in units per second
whatever the number of substeps.

## Example

To make a cube rise at 2 units per second:

1. A GeoPrimitive makes a cube.
2. A GeoWrangle over Points gives it a velocity: `v@v = {0, 2, 0};`
3. A GeoSolverBegin set to Prev Frame takes the GeoWrangle.
4. A GeoWrangle over Points moves the points: `@P += @v * @TimeInc;`
5. A GeoSolverEnd closes the solver.

At frame 25 at 24 frames per second, the cube has risen 2 units.
