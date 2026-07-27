# Screen preview

Renders the Zwift Remote screen layout to a PNG on the host, so a UI change can
be judged before it costs a flash cycle.

```bash
cp ../../include/fonts/*.h .
cc -O2 -I. -o preview preview.c -lm
./preview      > a.ppm && python3 ppm2png.py a.ppm connected.png
./preview alt  > b.ppm && python3 ppm2png.py b.ppm alt.png     # disconnected, AUTO armed, buttons held
python3 zoom.py a.ppm icons.png 3 0,88,222,320                 # 3x crop of the nav pad
```

`ppm2png.py` needs nothing but the Python standard library — PNG is just zlib
plus four length-prefixed chunks.

## What it proves, and what it does not

It links the **real** font headers from `include/fonts/` and its `drawChar` is a
faithful port of `Arduino_GFX::drawChar`'s custom-font path. So it genuinely
verifies the parts that are new and easy to get wrong: that `fontconvert`'s bit
packing is right, that labels fit inside their buttons, that nothing collides,
and that ink stays legible against every state colour.

It **re-implements** the drawing primitives (`fillRoundRect`, `fillTriangle`,
`drawCircle`, …) rather than calling Arduino_GFX, so it does not prove those.
They were already working on the board; there is nothing to learn by mocking
them a second time.

⚠ The layout here is a **hand-kept copy** of `src/main.cpp`. It will drift.
Treat a disagreement between them as a bug in this file, never in the firmware,
and re-sync before trusting a render.

## It has already earned its keep

Two defects it caught before they reached the glass:

- The thumbs-up drew its knuckle gaps **vertically**, which read as three raised
  fingers — a rude gesture rather than a Ride On. Curled fingers seen from the
  side stack horizontally.
- The pressed state tinted the fill toward white while keeping white ink, so a
  held arrow nearly vanished — destroying the contrast at exactly the moment you
  are looking for confirmation that your turn registered.
