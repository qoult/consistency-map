# v1.0.0

- First release.
- Records where every player 1 jump is pressed and scores each one-block section
  of the level by the spread of those presses, converted into 240 TPS ticks, and
  by how often the section is pressed in at all.
- Draws a strip across the top of the screen painting only the shaky sections,
  and lists the worst ones on the pause screen with their numbers.
- Data is kept per level as a compact string using Welford's method, so it
  survives across sessions without growing with attempt count.
- Practice runs are ignored by default.
