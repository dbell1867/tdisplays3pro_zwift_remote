# Zwift Remote

A wireless handlebar remote for [Zwift](https://www.zwift.com/), built on the
**LilyGO T-Display S3 Pro** (ESP32-S3). It pairs with the computer running Zwift
as a **Bluetooth (BLE) HID keyboard** and sends Zwift's keyboard shortcuts:

| Button        | Sends | Zwift action                     |
|---------------|-------|----------------------------------|
| Garage        | `T`   | Garage & Drop Shop (change bike) |
| Menu          | `Esc` | Menu / paused screen             |

No app, no Zwift-side setup: the keystrokes go to the focused window, and Zwift
runs fullscreen.

See [`docs/PLAN.md`](docs/PLAN.md) for the staged build plan and the reasoning
behind the BLE-keyboard approach.

## Build & flash

```bash
pio run                 # compile
pio run -t upload       # flash over USB-C
pio device monitor      # watch the serial log (115200)
```

## Status

- **Stage 1 — minimal BLE HID keyboard** (no display): press GPIO 12 → `T`,
  GPIO 16 → `Esc`. Pair from your computer's Bluetooth settings, then test.

This is a sibling project to the `tdisplay` board bring-up (Stages 1–14); it
reuses that board knowledge but is separate firmware.
