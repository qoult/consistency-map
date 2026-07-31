# Consistency Map

A Geode mod for Geometry Dash 2.2081 that shows **where your timing is shaky
before you start dying there**.

Death trackers tell you where you already failed. Consistency Map watches the
attempts you survive: it records where in the level each jump was pressed, and
reports the sections where those presses scatter.

## What it measures

The level is cut into sections one block wide. For every section the mod keeps:

- the spread of your click positions, converted from level units into 240 TPS
  physics ticks using how fast you actually cross that section;
- how often you press there at all - pressing on half your runs and not on the
  other half is its own kind of inconsistency.

A section is scored by whichever of the two is worse, and is ignored entirely
until you have played through it enough times (five by default) for the number
to mean anything.

The running mean and spread are kept with Welford's method, so nothing grows
without bound: a level's whole history is one short string, and it survives
across sessions.

## What you see

- **A strip across the top of the screen** during play, painting only the
  sections worth worrying about, yellow through red.
- **A panel when you pause**, listing the shakiest sections with the numbers
  behind them: `62%  spread 3.2 ticks  (24 runs)`.

Practice runs start from checkpoints and have different timing, so they are
ignored unless you turn that on.

## Building

There is no local toolchain in this project. Push to GitHub and the
**Build Geode Mod** workflow builds Windows, macOS and both Android targets,
then combines them into a single `.geode`.
