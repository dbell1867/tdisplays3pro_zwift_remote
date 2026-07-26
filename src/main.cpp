// ============================================================================
//  Zwift Remote — Stage 3: on-screen navigation pad
// ----------------------------------------------------------------------------
//  Stage 2 proved the key map on three physical buttons. But Zwift needs SIX
//  navigation actions (up, down, left, right, Enter, Esc) and this board only
//  has three readable buttons — so this stage puts the full pad on the screen.
//
//  WHY THIS STAGE EXISTS AS A TEST
//  -------------------------------
//  There is a real open question: a junction in Zwift gives you about two
//  seconds, and a dark screen has to wake, repaint (a full frame is ~40 ms over
//  this board's non-DMA SPI) and then have your finger find a target. The worry
//  is that a touch pad simply cannot be used at speed.
//
//  That worry is REASONING, not measurement. So this firmware measures it: every
//  wake times the whole button-down -> ready-to-touch path and prints the number
//  on screen ("wake NNN ms"). Try a real junction, read the number, and decide
//  with evidence instead of argument.
//
//  WHAT IS ON SCREEN
//      +---------------------+
//      |  Zwift Remote       |   status: connected / advertising
//      |  CONNECTED  wake 62 |   + the measured wake latency
//      +----+-----+----------+
//      |    |  ^  |          |
//      | <  | OK  |  >       |   the nav pad: 5 targets
//      |    |  v  |          |
//      +----+-----+----------+
//      |  ESC     |    T     |
//      +---------------------+
//
//  The physical rocker STAYS mapped to left/right and keeps working with the
//  screen dark — so you can compare the two directly at the same junction.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>                   // I2C bus (shared by touch and, later, the PMU)
#include <Arduino_GFX_Library.h>    // ST7796 display driver
#include <TouchDrvCSTXXX.hpp>       // SensorLib touch driver; auto-detects CST226SE
#include <HijelHID_BLEKeyboard.h>   // NimBLE-based BLE HID keyboard (core 3.x)

// ---------------------------------------------------------------------------
//  Pins. Carried over from the board bring-up project — none of these are
//  guesses, they are all measured/verified facts about this exact board.
//
//  `constexpr` rather than `#define`: a #define is a preprocessor text
//  substitution the compiler never sees — no type, no scope, and free to
//  collide with a library header. A constexpr is a real typed compile-time
//  variable: identical zero runtime cost, but type-checked and debuggable.
// ---------------------------------------------------------------------------
constexpr int TFT_SCLK = 18;
constexpr int TFT_MOSI = 17;
constexpr int TFT_MISO =  8;
constexpr int TFT_CS   = 39;
constexpr int TFT_DC   =  9;
constexpr int TFT_RST  = 47;
constexpr int TFT_BL   = 48;

constexpr int I2C_SDA   =  5;
constexpr int I2C_SCL   =  6;
constexpr int TOUCH_RST = 13;
constexpr int TOUCH_IRQ = 21;

// Three readable buttons. The controls are TWO ROCKERS = four switches, but
// rocker 1's other half is the RESET (EN) pin, which is hardware-only and not
// readable as a GPIO at all.
constexpr uint8_t BTN_LEFT  = 12;   // rocker half -> Left arrow
constexpr uint8_t BTN_RIGHT = 16;   // rocker half -> Right arrow
constexpr uint8_t BTN_UI    =  0;   // screen dark: wake. screen lit: tap=Enter, hold=Esc

constexpr int16_t SCREEN_W = 222;
constexpr int16_t SCREEN_H = 480;

constexpr uint32_t DEBOUNCE_MS = 25;
constexpr uint32_t HOLD_MS     = 400;   // BTN_UI: longer than this is a hold, not a tap

// How long the screen stays lit with no input. Generous on purpose: this is a
// test build and a 5-second blank makes it impossible to watch anything.
constexpr uint32_t SCREEN_OFF_MS = 20000;

// The touch IRQ *pulses* — it goes inactive between frames even while your
// finger is still down — so isPressed() flickers false constantly and cannot be
// used to detect release. Instead every accepted frame refreshes lastTouchMs,
// and a gap longer than this means the finger lifted.
//
// This value is a deliberate trade-off. A HELD arrow matters here (holding Down
// is a U-turn in Zwift), and a spurious release mid-hold would break it — far
// worse than releasing a fraction of a second late. So the timeout is generous,
// and fast response comes from the explicit touch-up frame instead (below).
constexpr uint32_t TOUCH_RELEASE_MS = 120;

// ---------------------------------------------------------------------------
//  Display + touch objects.
//
//  `new` returns a POINTER, which is why these use `->` instead of `.`. Think of
//  it as the difference between a Python object you hold a reference to and one
//  you hold by value — except C++ makes you say which.
//
//  The 49 is the panel COLUMN OFFSET. This 222-pixel-wide panel sits inside a
//  wider controller frame buffer, so pixel 0 of our image is really column 49 of
//  the ST7796's memory. Get it wrong and the whole image is shifted sideways.
// ---------------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);

Arduino_GFX *gfx = new Arduino_ST7796(
    bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
    SCREEN_W, SCREEN_H, 49 /*col offset*/, 0 /*row offset*/);

TouchDrvCSTXXX touch;
int16_t touchX[5];
int16_t touchY[5];

// Renamed from the library default in Stage 2, which is why the computer sees a
// new device. Capped at 29 characters by the BLE advertising payload.
HijelHID_BLEKeyboard keyboard("Zwift Remote", "dcbmah");

// ---------------------------------------------------------------------------
//  On-screen buttons.
//
//  Every pad button uses press()/release(), not tap() — the same decision as the
//  physical rocker in Stage 2, and for the same reason: Zwift cares about a key
//  being HELD (holding Down triggers a U-turn), which a tap cannot express. As a
//  bonus the computer generates auto-repeat for menu scrolling, exactly as it
//  does for a real keyboard.
//
//  Making Esc/OK/T hold-style too costs nothing and removes every special case:
//  resting your finger on Esc repeats Esc, which is what a real keyboard does.
// ---------------------------------------------------------------------------
enum class Glyph : uint8_t { Text, Up, Down, Left, Right };

//  Most pad buttons send a key. One does not: the AUTO button toggles the
//  repeating Ride On below, so it needs its own kind rather than a keycode.
enum class PadKind : uint8_t { Key, ToggleAuto };

struct PadButton {
  int16_t     x, y, w, h;
  const char *label;    // drawn when glyph == Glyph::Text
  Glyph       glyph;
  uint8_t     keycode;
  uint16_t    color;
  PadKind     kind;
};

// Grid geometry: three 70px columns with 6px gaps exactly fills 222px.
constexpr int16_t CW = 70, CH = 70;
constexpr int16_t COL0 = 0, COL1 = 76, COL2 = 152;
constexpr int16_t ROW0 = 96, ROW1 = 172, ROW2 = 248;
constexpr int16_t BOT_Y = 336, BOT_H = 76, BOT_W = 108;

PadButton pad[] = {
  // The nav cluster.
  { COL1, ROW0, CW, CH, "",    Glyph::Up,    KEY_UP,     RGB565_DODGERBLUE, PadKind::Key },
  { COL0, ROW1, CW, CH, "",    Glyph::Left,  KEY_LEFT,   RGB565_DODGERBLUE, PadKind::Key },
  { COL1, ROW1, CW, CH, "OK",  Glyph::Text,  KEY_RETURN, RGB565_DARKGREEN,  PadKind::Key },
  { COL2, ROW1, CW, CH, "",    Glyph::Right, KEY_RIGHT,  RGB565_DODGERBLUE, PadKind::Key },
  { COL1, ROW2, CW, CH, "",    Glyph::Down,  KEY_DOWN,   RGB565_DODGERBLUE, PadKind::Key },

  // The 3x3 grid's corners were empty, so the new buttons cost no layout churn
  // and no paging. F3 is a Ride On *bomb* — one press thanks every rider near
  // you, not just one.
  { COL0, ROW0, CW, CH, "RIDE", Glyph::Text, KEY_F3, RGB565_ORANGE,  PadKind::Key        },
  { COL2, ROW0, CW, CH, "AUTO", Glyph::Text, 0,      RGB565_DARKGREY, PadKind::ToggleAuto },

  { COL0,   BOT_Y, BOT_W, BOT_H, "ESC", Glyph::Text, KEY_ESCAPE, RGB565_RED,    PadKind::Key },
  { BOT_W+6, BOT_Y, BOT_W, BOT_H, "T",  Glyph::Text, KEY_T,      RGB565_PURPLE, PadKind::Key },
};
constexpr size_t PAD_COUNT = sizeof(pad) / sizeof(pad[0]);

// Which pad button is currently held, or nullptr. A pointer, so it can refer to
// whichever button is active — or to none.
PadButton *activePad = nullptr;
bool       activePadSent = false;   // did we actually deliver press() for it?

uint32_t lastTouchMs   = 0;
bool     touchActive   = false;

// ---------------------------------------------------------------------------
//  Screen power state.
//
//  "Off" means TWO things, and missing either one wastes current: the backlight
//  goes to PWM duty 0 (~20 mA) AND the display controller is put to sleep with
//  SLPIN via displayOff() (~10 mA). Blanking only the backlight leaves the
//  ST7796 happily scanning a panel nobody can see.
// ---------------------------------------------------------------------------
bool     screenOn      = false;
uint32_t lastActivityMs = 0;
uint32_t lastWakeMs     = 0;    // measured button-down -> ready-to-touch, in ms
bool     bondsCleared   = false;   // did the boot gesture erase our stored pairings?

// ---------------------------------------------------------------------------
//  Repeating Ride On.
//
//  F3 is a Ride On BOMB: one press thanks every rider near you. Zwift rate-
//  limits it — you must wait several seconds between presses, with ~8 s the
//  best cadence anyone has documented. The exact F3 cooldown is not published,
//  so this interval is deliberately conservative.
//
//  Sending faster than the cooldown is not harmful, it is simply IGNORED by
//  Zwift — so a shorter interval would burn battery and radio time to achieve
//  precisely nothing. If you want to tune it, raise it before you lower it.
// ---------------------------------------------------------------------------
constexpr uint32_t RIDE_ON_INTERVAL_MS = 10000;

bool     autoRideOn   = false;
uint32_t lastRideOnMs = 0;
uint32_t rideOnCount  = 0;

static bool wasConnected = false;

// ---------------------------------------------------------------------------
//  A debounced button that reports BOTH edges (unchanged from Stage 2).
// ---------------------------------------------------------------------------
enum class BtnEvent : uint8_t { None, Down, Up };

struct Button {
  uint8_t  pin;
  bool     stableHigh;
  bool     rawHigh;
  uint32_t lastEdgeMs;
};

static void buttonInit(Button &b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);   // external pull-up already present; harmless belt-and-braces
  b.stableHigh = (digitalRead(pin) == HIGH);
  b.rawHigh    = b.stableHigh;
  b.lastEdgeMs = millis();
}

// Buttons are active-LOW: going LOW is a press, going HIGH is a release.
static BtnEvent buttonUpdate(Button &b) {
  bool now = (digitalRead(b.pin) == HIGH);
  if (now != b.rawHigh) {
    b.rawHigh    = now;
    b.lastEdgeMs = millis();
  }
  if ((millis() - b.lastEdgeMs) >= DEBOUNCE_MS && now != b.stableHigh) {
    b.stableHigh = now;
    return now ? BtnEvent::Up : BtnEvent::Down;
  }
  return BtnEvent::None;
}

static bool buttonHeld(const Button &b) { return !b.stableHigh; }

// ---------------------------------------------------------------------------
//  The physical rocker — left/right, and deliberately NOT a screen wake.
//
//  You do not need the screen to turn at a junction, so the rocker never lights
//  it: that saves ~30 mA and avoids a screen flashing in your eyes at every
//  turn on a night ride.
// ---------------------------------------------------------------------------
struct ArrowButton {
  Button      btn;
  uint8_t     keycode;
  const char *name;
  bool        sent;     // guards the release: only release what we pressed
};

ArrowButton arrowLeft  { {}, KEY_LEFT,  "LEFT",  false };
ArrowButton arrowRight { {}, KEY_RIGHT, "RIGHT", false };

static void arrowUpdate(ArrowButton &a, bool connected) {
  switch (buttonUpdate(a.btn)) {
    case BtnEvent::Down:
      Serial.printf("[%-5s] down", a.name);
      if (connected) {
        keyboard.press(a.keycode);
        a.sent = true;
        Serial.println(" — key held");
      } else {
        Serial.println(" — not connected");
      }
      break;
    case BtnEvent::Up:
      Serial.printf("[%-5s] up", a.name);
      if (a.sent) {
        keyboard.release(a.keycode);
        a.sent = false;
        Serial.println(" — key released");
      } else {
        Serial.println();
      }
      break;
    case BtnEvent::None:
      break;
  }
}

// ---------------------------------------------------------------------------
//  Drawing.
// ---------------------------------------------------------------------------
static void drawPadButton(const PadButton &b, bool pressed) {
  // The AUTO toggle colours itself from its state rather than a fixed colour,
  // so the button IS the indicator. Three states, not two:
  //
  //    grey   — off
  //    GREEN  — armed AND connected, i.e. genuinely sending
  //    AMBER  — armed but DISCONNECTED, so nothing is going out
  //
  // The amber case is the one that matters. Sending is gated on `connected`,
  // so after a reflash or a dropped link the repeat silently stalls — and an
  // indicator that reads "running" while nothing happens is worse than no
  // indicator at all. Never let a status light show intent instead of reality.
  uint16_t base;
  if (b.kind == PadKind::ToggleAuto) {
    base = !autoRideOn               ? RGB565_DARKGREY
         : keyboard.isConnected()    ? RGB565_GREEN
                                     : RGB565_ORANGE;
  } else {
    base = b.color;
  }
  uint16_t bg = pressed ? RGB565_WHITE : base;
  uint16_t fg = pressed ? base         : RGB565_WHITE;
  gfx->fillRect(b.x, b.y, b.w, b.h, bg);

  int16_t cx = b.x + b.w / 2;
  int16_t cy = b.y + b.h / 2;

  if (b.glyph == Glyph::Text) {
    gfx->setTextColor(fg);
    gfx->setTextSize(3);
    int16_t tw = (int16_t)strlen(b.label) * 18;   // size-3 char is ~18 px wide
    gfx->setCursor(cx - tw / 2, cy - 12);         // ...and ~24 px tall
    gfx->print(b.label);
    return;
  }

  // An arrow: a filled triangle reads far better than an ASCII "^" or "v", and
  // the default GFX font has no arrow glyphs anyway.
  constexpr int16_t S = 18;
  switch (b.glyph) {
    case Glyph::Up:
      gfx->fillTriangle(cx, cy - S, cx - S, cy + S, cx + S, cy + S, fg); break;
    case Glyph::Down:
      gfx->fillTriangle(cx, cy + S, cx - S, cy - S, cx + S, cy - S, fg); break;
    case Glyph::Left:
      gfx->fillTriangle(cx - S, cy, cx + S, cy - S, cx + S, cy + S, fg); break;
    case Glyph::Right:
      gfx->fillTriangle(cx + S, cy, cx - S, cy - S, cx - S, cy + S, fg); break;
    default: break;
  }
}

// Repaint just the AUTO toggle. Needed because its colour depends on the BLE
// connection, which changes on its own schedule — without this, a dropped link
// leaves the button showing its last-drawn colour indefinitely. New state has
// to be PUSHED to the screen; a correct colour rule that nothing re-runs is
// still a stale pixel.
static void drawAutoButton() {
  for (size_t i = 0; i < PAD_COUNT; i++) {
    if (pad[i].kind == PadKind::ToggleAuto) {
      drawPadButton(pad[i], false);
      return;
    }
  }
}

// Redraw just the status band, so tapping a button doesn't flicker the whole pad.
static void drawStatus(bool connected) {
  gfx->fillRect(0, 0, SCREEN_W, 90, RGB565_BLACK);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(6, 10);
  gfx->print("Zwift Remote");

  gfx->setTextColor(connected ? RGB565_GREEN : RGB565_ORANGE);
  gfx->setCursor(6, 36);
  gfx->print(connected ? "CONNECTED" : "ADVERTISING");

  // The number this whole stage exists to produce.
  if (lastWakeMs > 0) {
    gfx->setTextColor(RGB565_CYAN);
    gfx->setCursor(6, 62);
    gfx->printf("wake %lu ms", (unsigned long)lastWakeMs);
  }

  // Ride On counter, colour-matched to the AUTO button: green while it is
  // really firing, amber while armed but stalled on a dropped link.
  if (autoRideOn) {
    gfx->setTextColor(connected ? RGB565_GREEN : RGB565_ORANGE);
    gfx->setCursor(140, 62);
    gfx->printf("RO:%lu", (unsigned long)rideOnCount);
  }
}

static void drawFooter() {
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(6, 424);
  gfx->print("rocker = left/right (works dark)");
  gfx->setCursor(6, 440);
  gfx->print("GPIO0  = wake / tap Enter / hold Esc");
  gfx->setCursor(6, 456);
  if (bondsCleared) {
    gfx->setTextColor(RGB565_YELLOW);
    gfx->print("BONDS CLEARED - forget device on PC too");
  } else {
    gfx->print("hold both rocker halves at boot = clear bonds");
  }
}

static void drawAll(bool connected) {
  gfx->fillScreen(RGB565_BLACK);
  drawStatus(connected);
  for (size_t i = 0; i < PAD_COUNT; i++) drawPadButton(pad[i], false);
  drawFooter();
}

// ---------------------------------------------------------------------------
//  Screen on / off.
//
//  ORDER MATTERS IN BOTH DIRECTIONS, and getting it wrong shows white noise:
//
//  Off: backlight to 0 FIRST, then SLPIN — so the controller's teardown happens
//       while nobody can see it.
//  On:  SLPOUT, then paint black, then draw, and only THEN raise the backlight.
//       SLPIN discards the controller's frame buffer, so on resume it contains
//       random data. Painting black while still dark is a GUARANTEE; trying to
//       finish the repaint before the backlight rises is a RACE (a full frame is
//       ~40 ms on this non-DMA SPI bus).
//
//  The rule this encodes: never illuminate a buffer you haven't drawn.
// ---------------------------------------------------------------------------
static void screenOff() {
  if (!screenOn) return;
  ledcWrite(TFT_BL, 0);
  gfx->displayOff();
  screenOn = false;
  Serial.println("[screen] off (backlight 0 + SLPIN)");
}

// `sinceMs` is when the waking button went down, so we can report the honest
// end-to-end latency rather than just our own drawing time.
static void screenWake(bool connected, uint32_t sinceMs) {
  if (screenOn) return;
  gfx->displayOn();
  drawAll(connected);
  ledcWrite(TFT_BL, 255);
  screenOn = true;

  // The touch controller asserts its IRQ and HOLDS IT LOW until somebody reads
  // the pending report. We skipped reading it entirely while dark, so drain any
  // stale frame now — otherwise the first thing we see is a phantom press.
  touch.getPoint(touchX, touchY, 5);
  touchActive = false;
  activePad   = nullptr;

  lastWakeMs = millis() - sinceMs;
  drawStatus(connected);      // repaint the band now that we know the number
  Serial.printf("[screen] on — wake took %lu ms\n", (unsigned long)lastWakeMs);
}

// ---------------------------------------------------------------------------
//  Touch handling — press/release on a pulsing IRQ.
// ---------------------------------------------------------------------------
static void padRelease() {
  if (!touchActive) return;
  touchActive = false;
  if (activePad) {
    if (activePadSent) {
      keyboard.release(activePad->keycode);
      activePadSent = false;
    }
    drawPadButton(*activePad, false);
    Serial.printf("[pad] release %s\n",
                  activePad->glyph == Glyph::Text ? activePad->label : "arrow");
    activePad = nullptr;
  }
}

static void touchUpdate(bool connected) {
  // Fallback release: no frame for a while means the finger lifted.
  if (touchActive && (millis() - lastTouchMs) > TOUCH_RELEASE_MS) padRelease();

  if (!touch.isPressed()) return;

  uint8_t count = touch.getPoint(touchX, touchY, 5);

  if (count == 0) {
    // The IRQ fired with no points: an explicit touch-up. Release immediately so
    // the next tap lands on a clean edge instead of waiting out the timeout.
    padRelease();
    return;
  }

  int16_t x = touchX[0];
  int16_t y = touchY[0];
  lastTouchMs    = millis();
  lastActivityMs = millis();

  if (touchActive) return;   // already holding something; nothing new to do

  touchActive = true;
  for (size_t i = 0; i < PAD_COUNT; i++) {
    PadButton &b = pad[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
      activePad = &b;

      // The AUTO button sends no key of its own — it flips the repeat on/off.
      if (b.kind == PadKind::ToggleAuto) {
        autoRideOn = !autoRideOn;
        // Backdate the timer on enable so the first bomb goes out immediately
        // instead of making you wait a full interval to see anything happen.
        // (Unsigned wraparound makes this correct even in the first 10 s
        // after boot: the subtraction wraps, and so does the later compare.)
        lastRideOnMs = autoRideOn ? (millis() - RIDE_ON_INTERVAL_MS) : millis();
        drawPadButton(b, true);
        drawStatus(connected);
        Serial.printf("[pad] AUTO ride-on %s\n", autoRideOn ? "ON" : "OFF");
        return;
      }

      drawPadButton(b, true);              // highlight while held
      if (connected) {
        keyboard.press(b.keycode);
        activePadSent = true;
      }
      Serial.printf("[pad] press %s%s\n",
                    b.glyph == Glyph::Text ? b.label : "arrow",
                    connected ? "" : " — not connected");
      return;
    }
  }
}

// ---------------------------------------------------------------------------
//  The UI button — wake when dark, Enter/Esc when lit.
//
//  When the screen is dark the press is SWALLOWED rather than sending Enter.
//  That is not tidiness: an accidental Enter can open Zwift's chat box, which
//  then captures every keystroke after it into a text field.
// ---------------------------------------------------------------------------
struct UiButton {
  Button   btn;
  uint32_t downMs;
  bool     holdFired;
  bool     wokeThisPress;   // this press was consumed waking the screen
};

UiButton uiButton { {}, 0, false, false };

static void sendTap(uint8_t keycode, const char *what, bool connected) {
  Serial.printf("[UI   ] %s", what);
  if (connected) {
    keyboard.tap(keycode);
    Serial.println(" — sent");
  } else {
    Serial.println(" — not connected");
  }
}

static void uiUpdate(UiButton &u, bool connected) {
  BtnEvent ev = buttonUpdate(u.btn);

  if (ev == BtnEvent::Down) {
    u.downMs        = millis();
    u.holdFired     = false;
    u.wokeThisPress = false;
    lastActivityMs  = millis();

    if (!screenOn) {
      u.wokeThisPress = true;       // consume this press: wake only, no keystroke
      screenWake(connected, u.downMs);
      return;
    }
  }

  if (u.wokeThisPress) {            // ignore the rest of the waking press
    if (ev == BtnEvent::Up) u.wokeThisPress = false;
    return;
  }

  // Fire the hold action the MOMENT the threshold is crossed, not on release —
  // the action lands while your thumb is still down, which is what makes a
  // long-press feel deliberate rather than laggy.
  if (buttonHeld(u.btn) && !u.holdFired && (millis() - u.downMs) >= HOLD_MS) {
    u.holdFired = true;
    sendTap(KEY_ESCAPE, "hold  -> Esc", connected);
  }

  // A tap can only be identified on RELEASE: until you let go it might still
  // have become a hold.
  if (ev == BtnEvent::Up && !u.holdFired) {
    sendTap(KEY_RETURN, "tap   -> Enter", connected);
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  // On native USB, Serial.print BLOCKS once the TX buffer fills if no host is
  // draining it — silently freezing the loop when running standalone on the
  // bars. This makes writes drop instead of wait. An `if (Serial)` guard is NOT
  // enough: Serial reads true whenever USB is merely connected.
  Serial.setTxTimeoutMs(0);

  Serial.println();
  Serial.println("=== Zwift Remote — Stage 3: on-screen nav pad ===");

  // Backlight on PWM (5 kHz is flicker-free), and held at ZERO through all of
  // bring-up. The display controller's frame buffer contains random data at
  // power-on, so raising the backlight before the first real frame is drawn is
  // how you get a screenful of white noise at every boot.
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 0);

  if (!gfx->begin()) Serial.println("ERROR: gfx->begin() failed!");
  gfx->fillScreen(RGB565_BLACK);

  touch.setPins(TOUCH_RST, TOUCH_IRQ);
  if (!touch.begin(Wire, CST226SE_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    Serial.println("ERROR: touch.begin() failed!");
  } else {
    Serial.println("Touch controller online.");
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);

  buttonInit(arrowLeft.btn,  BTN_LEFT);
  buttonInit(arrowRight.btn, BTN_RIGHT);
  buttonInit(uiButton.btn,   BTN_UI);

  // --- Bond recovery gesture ------------------------------------------------
  //  Hold BOTH rocker halves while powering on to erase the board's stored
  //  pairings.
  //
  //  Why this exists: BLE bonding is TWO-SIDED. The host keeps a long-term key
  //  (LTK) and so does the board. If only one side is erased, the other keeps
  //  presenting a key its peer can no longer honour — and the failure does not
  //  look like a key mismatch, it looks like "the device cannot be found",
  //  because the host silently gives up before ever showing you anything.
  //  Clearing BOTH sides is the standard fix; this is the board's half.
  bool clearGesture = (digitalRead(BTN_LEFT) == LOW) && (digitalRead(BTN_RIGHT) == LOW);

  // Let the library report connection, pairing and advertising events. When a
  // two-sided link misbehaves, instrument the side you actually control.
  keyboard.setLogLevel(HIDLogLevel::Normal);

  keyboard.begin();
  Serial.println("Advertising as \"Zwift Remote\".");

  if (clearGesture) {
    keyboard.clearBonds();
    bondsCleared = true;
    Serial.println("*** BONDS CLEARED (both rocker halves held at boot).");
    Serial.println("*** Now remove/forget this device on the computer too,");
    Serial.println("*** then pair again. Both sides must be cleared.");
  }
  Serial.printf("Bond state: bonded=%s paired=%s\n",
                keyboard.isBonded() ? "yes" : "no",
                keyboard.isPaired() ? "yes" : "no");

  // First real frame, then light it up.
  drawAll(false);
  ledcWrite(TFT_BL, 255);
  screenOn       = true;
  lastActivityMs = millis();

  Serial.printf("Screen blanks after %lu s idle; GPIO %d wakes it.\n",
                (unsigned long)(SCREEN_OFF_MS / 1000), BTN_UI);
}

void loop() {
  bool connected = keyboard.isConnected();

  if (connected != wasConnected) {
    Serial.println(connected ? ">>> Computer connected."
                             : ">>> Computer disconnected — re-advertising.");
    if (!connected) {
      // The connection carried our held-key state; it is gone. Forget the
      // presses so a control held across a dropout doesn't release into the
      // next connection.
      arrowLeft.sent  = false;
      arrowRight.sent = false;
      activePadSent   = false;
    }
    if (screenOn) {
      drawStatus(connected);
      drawAutoButton();   // its colour depends on `connected` — repaint it
    }
    wasConnected = connected;
  }

  // The rocker works whatever the screen is doing — that is the whole point.
  arrowUpdate(arrowLeft,  connected);
  arrowUpdate(arrowRight, connected);
  uiUpdate(uiButton, connected);

  // Repeating Ride On. Deliberately runs whatever the screen is doing — enable
  // it, let the screen blank, and it keeps thanking riders while drawing almost
  // nothing. Note it does NOT touch lastActivityMs: an auto-send is the board's
  // own activity, not yours, so it must not keep the screen awake.
  if (autoRideOn && connected &&
      (millis() - lastRideOnMs) >= RIDE_ON_INTERVAL_MS) {
    keyboard.tap(KEY_F3);
    lastRideOnMs = millis();
    rideOnCount++;
    Serial.printf("[auto] Ride On bomb #%lu sent\n", (unsigned long)rideOnCount);
    if (screenOn) drawStatus(connected);
  }

  if (screenOn) {
    touchUpdate(connected);
    if ((millis() - lastActivityMs) > SCREEN_OFF_MS) {
      padRelease();       // never leave a key held into the dark
      screenOff();
    }
  }

  // Touch needs a fast poll to catch the pulsing IRQ (delay(10) measured ~16 Hz
  // on this controller, delay(2) ~40 Hz). With the screen dark there is no touch
  // to sample, so we back off — three buttons against a 25 ms debounce are
  // comfortable at 10 ms.
  delay(screenOn ? 2 : 10);
}
