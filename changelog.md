# v1.1.0

- The <cy>Map</c> button now opens a proper window instead of a text box: the
  level drawn as a bar at the top, then a table of the shakiest sections with a
  colour chip, the percentage, what makes each one shaky and how many runs are
  behind the number.

# v1.0.1

- Fixed the mod reporting "not enough attempts yet" after plenty of attempts. A
  section you play cleanly scores exactly 0.0, and 0.0 was being thrown away
  along with the "no data" value, so consistent play looked like no data at all.
  Calm sections are now scored, listed and reported as calm.
- The strip now paints every section you have played - grey until it is scored,
  then green, then yellow to red - so it fills in from the first attempt instead
  of staying blank. Its backing bar is visible on its own too.
- Fixed the gear button taking over GD's own pause settings. The callback was
  named `onSettings`, which is also a real `PauseLayer` method, so Geode hooked
  the game's function instead of adding a new one.

# v1.0.0

- First release.
- Records where every player 1 jump is pressed and scores each one-block section
  of the level by the spread of those presses, converted into 240 TPS ticks, and
  by how often the section is pressed in at all.
- Draws a strip across the top of the screen painting only the shaky sections.
- Pausing shows one summary line plus a Map button for the full list and a gear
  button for the settings.
- Data is kept per level as a compact string using Welford's method, so it
  survives across sessions without growing with attempt count.
- Practice runs are ignored by default.
