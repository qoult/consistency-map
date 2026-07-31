# Consistency Map

Shows **where your timing is shaky before you start dying there**.

Death trackers tell you where you already failed. Consistency Map watches the
attempts you survive: it records where in the level each jump was pressed, and
reports the sections where those presses scatter.

The level is cut into sections one block wide. Each one is scored by the spread
of your click positions - reported in 240 TPS physics ticks - and by how often
you press there at all, since pressing on half your runs and not the other half
is its own kind of inconsistency. Sections are ignored until you have played
through them enough times for the number to mean anything.

<cy>A strip across the top of the screen</c> paints only the sections worth
worrying about. <cy>Pausing</c> lists the shakiest ones with the numbers behind
them.
