# Zwift Remote

A wireless handlebar remote for [Zwift](https://www.zwift.com/), built on the
**LilyGO T-Display S3 Pro** (ESP32-S3). It pairs with the computer running Zwift
as a **Bluetooth (BLE) HID keyboard** and sends Zwift's keyboard shortcuts:

| Control              | Sends    | Zwift action                          |
|----------------------|----------|---------------------------------------|
| ← ↑ → ↓ (touch)      | arrows   | Turn at a junction; hold ↓ for a U-turn |
| ✓ (touch)            | `Enter`  | Confirm                               |
| MENU (touch)         | `Esc`    | Menu / paused screen                  |
| GARAGE (touch)       | `T`      | Garage & Drop Shop (change bike)      |
| RIDE ON (touch)      | `F3`     | Ride On bomb — thanks every rider near you |
| AUTO (touch)         | `F3` ×N  | Repeats the Ride On bomb every 10 s   |
| Rocker (physical)    | ← / →    | Works with the screen **off**         |
| GPIO 0 (physical)    | —        | Wake; tap = `Enter`, hold = `Esc`     |

Arrows use press/release rather than a tap, so the host generates auto-repeat
and a *held* ↓ registers as a U-turn — something a tap cannot express.

No app, no Zwift-side setup: the keystrokes go to the focused window, and Zwift
runs fullscreen.

There is **no pause shortcut** — Zwift does not have one. It is only in the
Companion app. `Esc` (menu) is the nearest equivalent.

See [`docs/PLAN.md`](docs/PLAN.md) for the staged build plan and the reasoning
behind the BLE-keyboard approach.

## Screen

Laid out in Zwift's own power-zone colours with a condensed bold face generated
from Noto Sans. The five navigation targets are solid slabs of colour; the
lower-urgency buttons are dark with a coloured rim, so the controls you need in
a two-second junction window are the ones that carry visual weight.

`tools/` holds the two host-side programs behind that:

| Tool | What it does |
|---|---|
| `fontconvert.c` | TTF → `GFXfont` header, sized in pixels. Regenerates `include/fonts/*.h` |
| `preview/` | Renders the screen layout to a PNG so UI changes can be checked without a flash cycle |

```bash
cc tools/fontconvert.c -o /tmp/fontconvert $(pkg-config --cflags --libs freetype2)
/tmp/fontconvert /usr/share/fonts/noto/NotoSans-CondensedBold.ttf 14 ZwiftBold14 \
    > include/fonts/ZwiftBold14.h
```

## Build & flash

```bash
pio run                 # compile
pio run -t upload       # flash over USB-C
pio device monitor      # watch the serial log (115200)
```

## Status

- **Stage 1–2** — BLE HID keyboard, full key map on physical buttons. ✅
- **Stage 3** — on-screen touch nav pad. ✅ Working in Zwift on Windows.
- **Stage 5a** — visual design. ✅ Measured screen wake **262 ms**
  (was 224 ms before the polish), ~13% of a junction's ~2 s budget.
- **Next** — an OFF state (deep sleep) so it survives on the bars between rides.

**Pairing trouble?** BLE bonding is two-sided and the board's half lives in NVS,
which reflashing does *not* erase. Hold **both rocker halves at boot** to clear
the board's bonds, then remove the device on the computer too. Both sides.

This is a sibling project to the `tdisplay` board bring-up (Stages 1–14); it
reuses that board knowledge but is separate firmware.
