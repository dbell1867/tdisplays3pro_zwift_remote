# Zwift Remote — Plan

A handlebar remote for **Zwift**, built on the LilyGO T-Display S3 Pro. It pairs
with the computer running Zwift as a **Bluetooth (BLE) HID keyboard** and sends
Zwift's own keyboard shortcuts when you press a button.

Built on the board we characterised across Stages 1–14 in the `tdisplay`
project — this reuses that board knowledge (display, touch, buttons, power)
but is a **separate firmware application**.

---

## Why BLE HID keyboard (the transport decision)

Zwift on PC/Mac has real keyboard shortcuts, and the actions we want map to
**single keys** — no menu-diving:

| Action                        | Zwift key |
|-------------------------------|-----------|
| Menu / paused screen          | `Esc`     |
| Garage & Drop Shop (change bike) | `T`    |
| Workout picker (spare)        | `E`       |
| Device pairing (spare)        | `A`       |
| Skip workout block (spare)    | `Tab`     |

A BLE HID keyboard is wireless (handlebar-friendly), reuses the BLE work from
Stage 13, and needs no cooperation from Zwift — the OS delivers the keystroke to
whatever window is focused, and Zwift is fullscreen.

**Rejected alternatives:** USB HID (wired, tethered); emulating a real Zwift
Play/Click controller (encrypted, partially-reversed protocol — fragile);
Zwift Companion network protocol (reverse-engineered, aimed at steering).

**Library:** `hijelhub/HijelHID_BLEKeyboard` — a NimBLE-based HID keyboard built
for ESP32 Arduino core 3.3.7+ / NimBLE 2.3.8+. Our core resolves to 3.3.9. The
classic `T-vK/ESP32-BLE-Keyboard` targets the old 2.x core and does not fit.

---

## Stages

### Stage 1 — Minimal BLE HID keyboard  ✅ DONE
De-risk the whole idea with the least code. No display. Advertise as a BLE
keyboard; press GPIO 12 → type `T`, press GPIO 16 → type `Esc`. Serial logs
connect/disconnect and every keystroke.
**Verified:** advertised as "HijelHID KB"; paired to the computer; a rocker press
lands `t` in a text editor; **in Zwift, `T` opens the garage and `Esc` opens the
menu.** Concept proven end-to-end. (Next pass renames the device to
"Zwift Remote" via the constructor's first arg — forces one re-pair.)

### Stage 2 — On-screen button UI
Bring back the display + CST226SE touch (reuse the bring-up init). Draw touch
buttons — **Menu**, **Garage**, and spares — each wired to its keystroke.
Physical rockers stay mapped too.

### Stage 3 — Connection status + polish
On-screen "advertising / connected" indicator, a visible key-map, auto-reconnect
behaviour, and a tap confirmation so you know a press was sent.

### Stage 4 — Battery + deep sleep
Battery % (reuse the SY6970 PMU work from Stage 5) and deep-sleep-on-idle with
wake-on-button, so it lasts on the bars. Measure the draw (reuse the Stage 5e /
Stage 14 power-bench method).

### Optional / later
Haptic tap feedback, more actions (ride-on, camera views), a config screen to
remap buttons, multi-key macros.

---

## Board facts carried over (from the tdisplay project)

- Rockers are **active-LOW with external pull-ups**; free halves are GPIO **12**
  and **16** (GPIO 0 is BOOT, keep for flashing).
- Native-USB serial re-enumerates on reset — wait for `Serial` with a timeout.
- Display: ST7796 via `GFX Library for Arduino`; touch: CST226SE via SensorLib;
  PMU: SY6970 via XPowersLib — all on shared I2C (added back in Stage 2+).
