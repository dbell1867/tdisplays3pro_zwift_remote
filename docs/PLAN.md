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

| Action                           | Zwift key   |
|----------------------------------|-------------|
| Turn left / right at a junction  | `←` / `→`   |
| Navigate a menu list             | `↑` / `↓`   |
| Select / confirm                 | `Enter`     |
| Menu / paused screen, and "back" | `Esc`       |
| Garage & Drop Shop (change bike) | `T`         |
| Workout picker (spare)           | `E`         |
| Device pairing (spare)           | `A`         |
| Skip workout block (spare)       | `Tab`       |

All of these exist in the library's `BLEHIDKeys.h`: `KEY_LEFT` (0x50),
`KEY_RIGHT` (0x4F), `KEY_UP` (0x52), `KEY_DOWN` (0x51), `KEY_RETURN` (0x28),
`KEY_ESCAPE` (0x29).

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

### Stage 2 — Full navigation key map, no display  ✅ DONE
Zwift navigation needs **↑ ↓ ← → Esc Enter**. The board has only **three**
readable buttons, so the map is split by *how time-critical each action is*
(see "The two input paths" below). This stage is firmware-only and de-risks the
time-critical path **before** any display work.

```
GPIO 12  ->  KEY_LEFT     press/release, always fires, screen state irrelevant
GPIO 16  ->  KEY_RIGHT    press/release, always fires, screen state irrelevant
GPIO 0   ->  tap = KEY_RETURN, hold = KEY_ESCAPE
```

Use **`press()` / `release()`**, not `tap()`, so the **host** generates key
auto-repeat when an arrow is held — correct for scrolling menus, and less radio
traffic than repeating in firmware.
**Verify:** a real route fork in Zwift, taken with the rocker.

### Stage 3 — Touch nav-pad, dark by default  ✅ WORKING (2026-07-26)
**Verified in Zwift:** paired to a Windows PC and the on-screen nav pad drives
the game. Some control assignments still need adjusting (TBD).
**MEASURED: wake = 224 ms** (GPIO 0 button-down -> ready to touch).

That is **~11% of the ~2 s junction budget**, leaving ~1.8 s for reach, aim and
tap. **The display wake is not the blocker** — the Stage 3 objection ("wake +
repaint will make you miss the turn") was wrong on the measurable axis. The
figure is also consistent with the hardware: well above the ~40 ms bare-frame
estimate because `drawAll()` issues `fillScreen` + nine buttons + status +
footer as separate non-DMA SPI transactions, plus `displayOn()` settling.

**Scope of the number — this is the important part.** 224 ms is the cost with
the **CPU awake and BLE already connected**. It is a floor for *this*
architecture, not a universal one. Any power saving that suspends the CPU or
drops the BLE link adds its latency **on top**, so the trade is direct:

> **battery life vs junction latency.**

The remaining risk to a usable remote is therefore no longer the screen — it is
**ergonomic** (can you hit a 70 px target while pedalling hard?), and only a
real junction can answer that.

---

## Pairing / bonding troubleshooting (learned the hard way, 2026-07-26)

Symptom: board logs "Advertising", **PC cannot find the device at all**.

**Do not debug this from the host.** Instrument the side you control, and use an
independent scanner to split "is it advertising?" from "will that host list it?".
A `bleak` scan from a Linux box found it instantly at **rssi −41**, and a GATT
connect enumerated a textbook HID-over-GATT profile (0x1812 with Report Map,
Protocol Mode, HID Info, Control Point + 0x180F battery). **The board was never
the problem.**

Two host-side causes, both Windows-specific and both needed fixing:

1. **BLE bonding is TWO-SIDED.** The host stores a long-term key and so does the
   board — and the board's bond lives in **NVS, which reflashing does NOT
   erase** (`Bond state: bonded=yes` survived two flashes). Clearing one side
   only leaves the other offering a key its peer cannot honour, and the failure
   presents as *"device not found"*, never as a key error.
2. **Windows caches BLE names by address.** The board's address never changes, so
   after the Stage 2 rename Windows still listed it as **"HijelHID KB"** — and
   Windows *hides already-known devices* from Add-device. So "Zwift Remote"
   appeared nowhere while the device sat in plain sight under the old name.

**Recovery procedure** (host first, so a reconnect can't re-bond the board):
1. Windows → Settings → Bluetooth & devices → View more devices → remove **both**
   `HijelHID KB` and `Zwift Remote`.
2. Device Manager → **Show hidden devices** → uninstall greyed-out entries under
   *Bluetooth* and *Human Interface Devices*. Windows leaves stale HID nodes that
   block a clean re-pair; step 1 alone is often not enough.
3. Board: **hold both rocker halves + tap RESET** → `clearBonds()`.
4. Pair fresh.

Two false leads worth remembering, both instances of *a diagnostic is worth what
its derivation is worth*:
- A `busctl` scan reported nothing — because **BlueZ ties discovery to the D-Bus
  client**, so it ended the moment `busctl call` exited (`Discovering: false`).
  An instrument that could only ever print "not found".
- The scan showed `service_uuids: []` and `AdvertisingFlags: 0x00`, which looked
  like a library bug suppressing the HID UUID and the General-Discoverable flag.
  It was neither: those are **derived** BlueZ properties, empty because
  `ServicesResolved: False`. A GATT connect showed every service present.
  *Suspect the instrument before the subject.*

Also confirmed from the boot log: security is **Just Works**, TX power is level 8
(**21 dBm**, maximum) — so range and pairing complexity are both ruled out.

### Stage 3 — Touch nav-pad, dark by default
Bring back the display + CST226SE touch (reuse the bring-up init). No LTR-553 —
proximity was considered as the screen wake and **rejected in favour of a
button** (see decisions below), so this stage needs only display + touch.

```
  +-------------+
  |      ^      |
  |  <   |      |     <- full pad for menu use
  |      v      |
  |  [Esc] [T]  |
  +-------------+
```

Screen is **off by default**: backlight duty 0 **and** `panel->displayOff()`
(`SLPIN`) — blanking only the backlight leaves the ST7796 scanning at ~10 mA.
**GPIO 0 is the UI button:**

- screen dark -> **wake only, keystroke swallowed**. Firing `Enter` here is not
  harmless: it can open Zwift's chat input, which then captures every
  subsequent keystroke into a text field.
- screen lit -> tap = `Enter`, hold = `Esc` as in Stage 2.

The **rocker never wakes the screen** — you don't need the screen to turn at a
junction, so don't pay ~30 mA for it, and a screen flashing on at every turn is
unpleasant at night.

On wake, order matters: `displayOn()` -> `fillScreen(RGB565_BLACK)` -> repaint ->
*then* raise the backlight. Never illuminate a buffer you haven't drawn.

### Stage 4a — An OFF state (deep sleep)
**The dominant consumer is idle-between-rides, not riding.** A ride is ~1 h and
runtime awake is ~8–10 h, so riding is never the constraint — but a remote living
on the bars, connected and awake, is flat in a day and a half. So the device
needs a genuine **off**:

```
OFF   deep sleep, ~tens of µA
      -> months on the shelf; the BLE reconnect on power-on is acceptable
         because you do it deliberately, before clipping in

ON    awake, BLE connected, screen dark    ~8–10 h, covers any ride
```

**⚠ OPEN — how power-off is triggered.** GPIO 0 would otherwise need *three*
press durations (tap = Enter, hold = Esc, long-hold = off), which is fiddly with
gloves on. Power-**on** must be physical (chip asleep, screen dark) → long-hold
GPIO 0. Power-**off** is better as a **button on the touch pad**: no duration to
learn, and impossible to trigger accidentally mid-ride. Asymmetric, but each half
matches its situation. Decide before Stage 3 lays out the pad.

Deep sleep was initially ruled out over reconnect latency — that only
disqualifies it **mid-ride**. As a deliberate power-on it is exactly right.
Deep sleep = **reboot**, not resume, so `setup()` re-runs and re-pairs; keep a
boot counter in `RTC_DATA_ATTR` to prove RTC memory survived. Wake via
**EXT1** (the S3 dropped EXT0): `esp_sleep_enable_ext1_wakeup(1ULL<<0,
ESP_EXT1_WAKEUP_ANY_LOW)`. Drive the backlight enable to its safe level and
`gpio_hold_en()` it — a non-RTC output floats in deep sleep. And wait for the
button to **release** before sleeping, or it wakes again instantly.

### Stage 4b — Light sleep  ⚠ LIKELY UNNECESSARY (superseded 2026-07-26)

**The 224 ms measurement above probably deletes this stage.** The reasoning:

- A ride is ~1 h; awake runtime is ~8–10 h. **Battery is a non-issue during a
  ride.** The problem was only ever idle-between-rides.
- Stage 4a's explicit OFF state already fixes that (months on the shelf), and
  costs nothing at a junction because you power on deliberately beforehand.
- Light sleep during a ride would buy battery you do not need, and pay for it in
  wake/reconnect latency **exactly at the moment you can least afford it**.

So the riskiest, highest-effort item in this plan — *"does the BLE HID link
survive light sleep?"* — can most likely be dropped without being answered.
**Do Stage 4a; skip 4b unless ride-time battery ever becomes a real problem.**

*Kept below for reference in case that assumption changes.*

#### (reference) Sleep, and MEASURE the latency
The open question this whole design rests on: **does the BLE HID link survive
light sleep, and how long is press -> keystroke-delivered?**

`afterWake()` is documented as *waiting for the host to reconnect and complete
LTK re-encryption*, with a 15 s default budget against a 3000 ms supervision
timeout (`HID_CONN_TIMEOUT`) — which reads like light sleep **drops** the link.
Junction turning needs the link live at all times, so this decides the ceiling:

- **If the link survives** — large battery win; sleep on idle.
- **If it does not** — the answer is "screen off, CPU awake, BLE riding the
  library's automatic idle slave-latency", and we accept ~32.5 mA of CPU.

Instrument the actual milliseconds; do not design on top of a guess. Then run
the Stage 5e / Stage 14 power-bench method **on battery with charging
disabled** — the first honest battery-side numbers in either project.

Button wake is cheap here: GPIO 0/12/16 all have **external** pull-ups, so the
whole `rtc_gpio_init` / `pullup_en` / `hold_en` +
`esp_sleep_pd_config(RTC_PERIPH, ON)` dance can be dropped.

### Stage 5 — Battery gauge + polish
SY6970 battery % (reuse the Stage 5 PMU work), fed to **`setBatteryLevel()`** so
the host shows the remote's own battery natively via the BLE Battery Service.
On-screen "advertising / connected" indicator, a visible key-map, a tap
confirmation, and `setTxPower()` tuning.

### Optional / later
**Wire external buttons** — a 5-way tactile nav switch or 6 tacts on free GPIOs
would give every action its own physical control (findable by feel, ~0 mA, all
RTC-wake-capable) and retire the touch pad from the input path. Also: haptic tap
feedback, more actions (ride-on, camera views), a remap screen, multi-key macros.

---

## The two input paths (the core design decision)

Two use cases with wildly different latency budgets:

| Use case | Budget | Path |
|---|---|---|
| **In-game turning at a junction** | ~2 s | Physical buttons. Instant, ~0 mA, works with the screen dark. |
| **Menus** (ride/workout picker, settings) | seconds are fine | **Touch nav-pad.** Screen dark until the UI button wakes it. |

The concern is that a touch pad cannot be the time-critical path: wake ->
`displayOn()` -> a full repaint (~40 ms over non-DMA SPI) -> your hand finding a
target -> possibly a BLE reconnect.

### ⚠ CORRECTION (2026-07-26) — all FOUR arrows are time-critical

This section originally assigned only `←`/`→` to the rocker and put `↑`/`↓` on
the touch pad. **That was wrong.** Verified against actual Zwift behaviour:

- A junction can be taken with **any** directional key; which one is correct
  **depends on the junction**.
- **Held `↓` triggers a U-turn** on the course.

So all four arrows matter at speed, and three readable buttons cannot cover four
arrows + Enter + Esc.

This **retroactively justifies `press`/`release` over `tap`** far more strongly
than the original menu-scrolling argument: a held `↓` producing a U-turn is only
expressible as a sustained key-down, which `tap()` could never do.

**Decision: build the full on-screen nav pad first and TEST it** (Stage 3)
before committing to hardware. The ~2 s latency objection above is *reasoning,
not measurement* — Stage 3 should report the real wake->ready time and let a
junction attempt settle it. Prefer the measurement that comes out differently
under each hypothesis.

### ⚠ Free pins for external buttons: BLOCKED (audited 2026-07-26)

**A camera shield is fitted to this board**, which consumes GPIO
**1, 2, 4, 7, 10, 11, 15** as its DVP data/clock lines — exactly the pins that
would otherwise have been free *and* RTC-capable.

Everything else in 0–21 is taken: 5/6 I²C, 8/9/17/18 display, 13/21 touch,
14 SD, 0/12/16 buttons, **19/20 native USB** (needed for serial + flashing).
Above the RTC range only 38/43/44 remain, and **GPIO 0–21 is the only range that
can do EXT1 deep-sleep wake**.

So with the shield attached there are **no free RTC-capable pins**. External
buttons require removing the shield — or a different board. Also note GPIO **3**
is a **strapping** pin (skip it) and GPIO **33–37 are consumed by the octal
PSRAM** on this R8 module, despite appearing free on generic pin lists.

**Open: is this the right board?** Deferred until Stage 3 shows whether on-screen
nav is good enough. If it is, the pin shortage stops mattering.

## Power budget (measured on THIS board — tdisplay Stages 5e / 5f / 14)

Inline USB meter, charging disabled. Trust the **differences**; the absolute
floor is inflated by the USB host link (~26–35 mA), which is absent on battery.

| Load | Measured |
|---|---|
| Busy poll CPU (`delay(5)` loop) | **32.5 mA** |
| Backlight 100% | **~20 mA** (± quite a lot) |
| ST7796 controller awake | **10.2 mA** |
| BLE — advertising *or* connected | **+9 mA** |
| Touch + ALS | 0.8 mA |
| Deep vs light sleep | 2.5 mA (at the resolution limit) |

### Turning that into runtime — battery is a **470 mAh** LiPo

⚠ **Conversion caveat.** Every figure above was measured on the **5 V side,
upstream of the PMU**, so it includes the USB host link (~26–35 mA) that is
absent on battery, and 5 V-side mA cannot be applied directly to a 3.7 V cell's
mAh. Converting through the buck (≈ ×1.36) gives rough battery-side numbers —
enough to rank options, not to quote.

| State | 5 V-side | ≈ battery-side | 470 mAh lasts |
|---|---|---|---|
| Awake, BLE connected, screen dark | ~42 mA | ~57 mA | **~8 h** |
| …at 80 MHz (`setCpuFrequencyMhz`) | — | ~45 mA | ~10 h |
| Light sleep, link alive *(if it works)* | unmeasured | few mA? | 50 h+ |
| Deep sleep | unmeasured | tens–hundreds µA | months |

The sleep rows are **unmeasured, not optimistic** — the measured "floor" of
25–28 mA was almost entirely the USB host link, so the battery-side sleep draw
is genuinely unknown. Stage 4b measures it.

**The touch pad is effectively free.** Screen lit at ~41 mA battery-side for
~60 s per ride is **0.7 mAh of 470 — 0.14%**. The ~30 mA screen cost that
argued against a touch nav-pad never mattered at this duty cycle. The expensive
thing is not the screen; it is the CPU being awake at all.

Consequences that shaped the stages above:

- **BLE is nearly free, and costs the same connected as advertising.** There is
  no power argument for dropping the link — only for `deinit`.
- **The screen is the expensive way to add buttons** (~30 mA) to replace physical
  buttons that cost ~0 mA. Hence: dark by default, button wake, rocker bypasses
  it entirely.
- **Busy CPU is the single largest firmware-controllable load** — which is why
  Stage 4's light-sleep question matters more than anything else on this list.
- Differences **≥10 mA are solid; ~2 mA is run-to-run drift.** Don't chase it.

## Decisions log

- **Proximity wake: rejected.** The LTR-553 could light the screen on hand
  approach, but a button is preferred — and it aligns with bring-up gotcha 22
  (a touch/sensor line makes a poor sleep wake source; the CST226SE holds IRQ
  low until the report is read, so a level wake fires immediately). Also drops
  the ALS from Stage 3 and simplifies Stage 4's pin setup.
- **Wake press is swallowed, not fired** — an accidental `Enter` can open Zwift's
  chat input and capture subsequent keystrokes.
- **`press`/`release` over `tap`** — host-side auto-repeat for held arrows.
- **Deep sleep is an OFF state, not an idle state.** Its BLE reconnect is
  unacceptable mid-ride and fine as a deliberate power-on. Idle-between-rides,
  not riding, is what drains a 470 mAh cell.
- **The touch pad's power cost was a non-issue** — 0.14% of the battery per
  ride. Raised as a concern, retracted once the duty cycle was quantified.

---

---

## Board facts carried over (from the tdisplay project)

- The controls are **2 rockers = 4 switches**: Rocker 1 = BOOT (**GPIO 0**) +
  RESET (the EN pin, hardware-only, not readable); Rocker 2 = **GPIO 12** +
  **GPIO 16**. So there are **three readable buttons**, all **active-LOW with
  external pull-ups**, all RTC-capable (GPIO 0–21 on the S3).
  *A rocker is not a button — an early probe that pressed only one side of each
  concluded GPIO 12 was dead. Exercise every control both ways.*
- **GPIO 0 is usable as the UI button** despite being BOOT — its bootloader role
  only applies while held *during reset*. Keep a short **"flash window"** at the
  top of `setup()` before any sleeping is allowed (a sleeping CPU isn't
  servicing native-USB CDC, and upload fails with `[Errno 71] Protocol error`;
  recover with RESET, or BOOT+RESET for the ROM bootloader).
- Native-USB serial re-enumerates on reset — wait for `Serial` with a timeout.
  Also call **`Serial.setTxTimeoutMs(0)`**: printing from `loop()` with no
  monitor attached otherwise blocks once the TX buffer fills.
- Display: ST7796 via `GFX Library for Arduino`; touch: CST226SE via SensorLib
  (@0x5A); PMU: SY6970 via XPowersLib (@0x6A) — all on shared I2C (SDA=5,
  SCL=6), added back in Stage 3+. The LTR-553 ALS (@0x23) is **not needed**.
- Arduino_GFX colour constants are `RGB565_BLACK` etc., not `BLACK`.
