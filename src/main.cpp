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
//      +--------------------------+
//      | / ZWIFT REMOTE  wake 224 |  wordmark + measured wake latency
//      | (o CONNECTED)      RO 12 |  state pill + Ride On counter
//      +------+--------+----------+
//      | (db) |   ^    |   AUTO   |  Ride On  /  up  /  repeat toggle
//      |  <   |   OK   |    >     |  the nav cross, in Zwift zone-2 blue
//      |      |   v    |          |
//      +------+--------+----------+
//      |    MENU      |   (bike)  |  Esc  /  T
//      +--------------------------+
//
//  The look is deliberate: Zwift's own power-zone colours, a condensed bold
//  face generated from Noto Sans, and real icons rather than letters. The five
//  navigation targets are solid slabs; everything else is a dark outline. See
//  PadStyle below for why that split is a usability decision, not a taste one.
//
//  The physical rocker STAYS mapped to left/right and keeps working with the
//  screen dark — so you can compare the two directly at the same junction.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>                   // I2C bus (shared by touch and, later, the PMU)
#include <Arduino_GFX_Library.h>    // ST7796 display driver
#include <TouchDrvCSTXXX.hpp>       // SensorLib touch driver; auto-detects CST226SE
#include <HijelHID_BLEKeyboard.h>   // NimBLE-based BLE HID keyboard (core 3.x)

// Proportional fonts, generated from Noto Sans Condensed by tools/fontconvert.c.
// The built-in GFX font is a 5x7 bitmap scaled by whole integers — at setTextSize(3)
// every stroke is 3 px thick and every curve is a staircase, which is exactly why
// the first cut looked like a debug screen rather than a product. A real font is
// the single biggest visual upgrade available here, and it costs ~4.5 KB of flash.
#include "fonts/ZwiftBold26.h"      // button labels
#include "fonts/ZwiftBold18.h"      // wordmark
#include "fonts/ZwiftBold14.h"      // status
#include "fonts/ZwiftSmall11.h"     // captions + footer

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

// ---------------------------------------------------------------------------
//  Palette.
//
//  RGB565 packs a colour into 16 bits: 5 red, 6 green, 5 blue (green gets the
//  spare bit because the eye resolves green detail best). Writing those as hex
//  literals is unreadable and unreviewable, so this converts from the ordinary
//  8-8-8 values you can look up — at COMPILE time, so it costs nothing.
//
//  The accents are not invented. They are Zwift's own power-zone colours, the
//  ones already burned into your eye from the HUD, plus the brand orange:
//
//      Z2 blue   #0996D8      Z3 green  #00A94F
//      Z4 yellow #FFCB0E      Z6 red    #E4002B      brand #FC6719
//
//  Using them means the remote reads as part of the same product rather than a
//  generic dev board sitting next to one.
// ---------------------------------------------------------------------------
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t C_BG      = rgb(0x0A, 0x0E, 0x14);  // near-black, faintly blue
constexpr uint16_t C_SURFACE = rgb(0x17, 0x1C, 0x24);  // unpressed outline buttons
constexpr uint16_t C_LINE    = rgb(0x2B, 0x33, 0x40);  // hairlines and recesses
constexpr uint16_t C_MUTED   = rgb(0x7C, 0x88, 0x99);  // captions, footer
constexpr uint16_t C_WHITE   = rgb(0xFF, 0xFF, 0xFF);

constexpr uint16_t C_ORANGE  = rgb(0xFC, 0x67, 0x19);  // Zwift brand
constexpr uint16_t C_BLUE    = rgb(0x09, 0x96, 0xD8);  // zone 2 — navigation
constexpr uint16_t C_GREEN   = rgb(0x00, 0xA9, 0x4F);  // zone 3 — go / connected
constexpr uint16_t C_AMBER   = rgb(0xFF, 0xCB, 0x0E);  // zone 4 — armed but stalled
constexpr uint16_t C_RED     = rgb(0xE4, 0x00, 0x2B);  // zone 6 — menu / escape
constexpr uint16_t C_STEEL   = rgb(0x6E, 0x7D, 0x94);  // inert / off

// Blend two RGB565 colours, t=0 gives a, t=255 gives b. Used for the pressed
// tint and the subtle rim light on solid buttons.
static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + ((br - ar) * t) / 255;
  int g = ag + ((bg - ag) * t) / 255;
  int l = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | l);
}

// Pick legible ink for a filled background.
//
// This is not fussiness. Hard-coding white ink puts white on Zwift's zone-4
// amber, which is the colour the AUTO button turns when the link has dropped —
// the one message on this screen you must not be able to miss. Deciding from
// perceived brightness (green counts for more than red, red for more than blue)
// keeps every fill readable no matter which state colour lands underneath.
static uint16_t inkOn(uint16_t bg) {
  int r = ((bg >> 11) & 0x1F) * 255 / 31;
  int g = ((bg >> 5)  & 0x3F) * 255 / 63;
  int b = (bg & 0x1F) * 255 / 31;
  int lum = (77 * r + 151 * g + 28 * b) >> 8;
  return (lum > 150) ? C_BG : C_WHITE;
}

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
enum class Glyph : uint8_t { None, Up, Down, Left, Right, Check, ThumbUp, Bike };

//  Most pad buttons send a key. One does not: the AUTO button toggles the
//  repeating Ride On below, so it needs its own kind rather than a keycode.
enum class PadKind : uint8_t { Key, ToggleAuto };

//  VISUAL WEIGHT FOLLOWS URGENCY.
//
//  Solid buttons are filled slabs of colour: maximum contrast, findable with a
//  glance you cannot afford to take. Outline buttons are dark with a coloured
//  rim: legible, but they recede.
//
//  The split is not decoration. At a junction you have about two seconds and
//  you are looking at the road, so the four arrows and the confirm are Solid.
//  Ride On, AUTO, MENU and GARAGE are things you do while soft-pedalling, so
//  they are Outline and stay out of the way. Making everything loud is the same
//  as making nothing loud.
enum class PadStyle : uint8_t { Solid, Outline };

struct PadButton {
  int16_t     x, y, w, h;
  const char *label;    // large text, drawn when non-empty
  const char *caption;  // small text along the bottom edge, may be ""
  Glyph       glyph;
  uint8_t     keycode;
  uint16_t    color;
  PadKind     kind;
  PadStyle    style;
};

// Grid geometry. Three 68px columns, 5px gutters, 4px margins: 3*68 + 2*5 + 2*4
// is exactly 222. The margins are new — the old layout ran the outer buttons
// off the edge of the glass, which is the single clearest tell of a screen laid
// out by arithmetic rather than by eye.
constexpr int16_t CW = 68, CH = 68;
constexpr int16_t COL0 = 4, COL1 = 77, COL2 = 150;
constexpr int16_t ROW0 = 94, ROW1 = 167, ROW2 = 240;
constexpr int16_t BOT_Y = 316, BOT_H = 78;
constexpr int16_t BOT_W = 104;                 // (222 - 2*4 - 5) / 2, rounded down

constexpr int16_t HEADER_H = 88;               // status band above the pad
constexpr int16_t RADIUS   = 10;               // corner rounding, all buttons

PadButton pad[] = {
  // The nav cluster — the time-critical five.
  { COL1, ROW0, CW, CH, "", "", Glyph::Up,    KEY_UP,     C_BLUE,  PadKind::Key, PadStyle::Solid },
  { COL0, ROW1, CW, CH, "", "", Glyph::Left,  KEY_LEFT,   C_BLUE,  PadKind::Key, PadStyle::Solid },
  { COL1, ROW1, CW, CH, "", "", Glyph::Check, KEY_RETURN, C_GREEN, PadKind::Key, PadStyle::Solid },
  { COL2, ROW1, CW, CH, "", "", Glyph::Right, KEY_RIGHT,  C_BLUE,  PadKind::Key, PadStyle::Solid },
  { COL1, ROW2, CW, CH, "", "", Glyph::Down,  KEY_DOWN,   C_BLUE,  PadKind::Key, PadStyle::Solid },

  // The 3x3 grid's corners were empty, so these cost no layout churn and no
  // paging. F3 is a Ride On *bomb* — one press thanks every rider near you.
  { COL0, ROW0, CW, CH, "",     "RIDE ON", Glyph::ThumbUp, KEY_F3, C_ORANGE, PadKind::Key,        PadStyle::Outline },
  { COL2, ROW0, CW, CH, "AUTO", "",        Glyph::None,    0,      C_STEEL,  PadKind::ToggleAuto, PadStyle::Outline },

  // Labelled by what they DO in Zwift, with the keystroke as the small print.
  // "ESC" and "T" describe the wire; "MENU" and "GARAGE" describe the ride.
  { COL0,        BOT_Y, BOT_W, BOT_H, "MENU", "ESC", Glyph::None, KEY_ESCAPE, C_RED,   PadKind::Key, PadStyle::Outline },
  { COL0+BOT_W+5,BOT_Y, BOT_W, BOT_H, "",  "GARAGE", Glyph::Bike, KEY_T,      C_STEEL, PadKind::Key, PadStyle::Outline },
};
constexpr size_t PAD_COUNT = sizeof(pad) / sizeof(pad[0]);

// For the serial log: a button's most useful human name.
static const char *padName(const PadButton &b) {
  if (b.caption[0]) return b.caption;
  if (b.label[0])   return b.label;
  switch (b.glyph) {
    case Glyph::Up:    return "UP";
    case Glyph::Down:  return "DOWN";
    case Glyph::Left:  return "LEFT";
    case Glyph::Right: return "RIGHT";
    case Glyph::Check: return "ENTER";
    default:           return "?";
  }
}

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
// --- Text ------------------------------------------------------------------
//
//  With a proportional font the cursor sits on the BASELINE, not the top-left
//  corner, and every glyph is a different width — so you can no longer guess a
//  string's size from strlen(). getTextBounds() measures the real ink box, and
//  centring against that measurement is what stops labels drifting a few pixels
//  off in every button, which is precisely the sort of thing the eye reads as
//  "unfinished" without being able to say why.

static int16_t textWidth(const char *s, const GFXfont *f) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setFont(f);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int16_t)w;
}

static void textAt(const char *s, const GFXfont *f, int16_t x, int16_t baseline, uint16_t color) {
  gfx->setFont(f);
  gfx->setTextColor(color);
  gfx->setCursor(x, baseline);
  gfx->print(s);
}

static void textRight(const char *s, const GFXfont *f, int16_t rightX, int16_t baseline, uint16_t color) {
  textAt(s, f, rightX - textWidth(s, f), baseline, color);
}

// Centre the ink box on (cx, cy) — not the advance box, so a string of capitals
// sits optically centred rather than floating high on its descender space.
static void textCentred(const char *s, const GFXfont *f, int16_t cx, int16_t cy, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setFont(f);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(cx - w / 2 - x1, cy - h / 2 - y1);
  gfx->print(s);
}

// Drop to the smaller face rather than let a label overrun its button. Cheaper
// and more honest than picking one size and hoping every label fits forever.
static const GFXfont *fitLabel(const char *s, int16_t maxW) {
  return (textWidth(s, &ZwiftBold26) <= maxW) ? &ZwiftBold26 : &ZwiftBold18;
}

// --- Icons -----------------------------------------------------------------

// A head PLUS a shaft. The old bare triangle was really a "play" symbol; adding
// the shaft is what makes it unambiguously an arrow at a glance.
static void drawArrow(Glyph g, int16_t cx, int16_t cy, int16_t s, uint16_t c) {
  constexpr int16_t T = 6;   // half the shaft thickness
  switch (g) {
    case Glyph::Up:
      gfx->fillTriangle(cx, cy - s, cx - s, cy, cx + s, cy, c);
      gfx->fillRect(cx - T, cy, 2 * T, s, c);                     break;
    case Glyph::Down:
      gfx->fillTriangle(cx, cy + s, cx - s, cy, cx + s, cy, c);
      gfx->fillRect(cx - T, cy - s, 2 * T, s, c);                 break;
    case Glyph::Left:
      gfx->fillTriangle(cx - s, cy, cx, cy - s, cx, cy + s, c);
      gfx->fillRect(cx, cy - T, s, 2 * T, c);                     break;
    case Glyph::Right:
      gfx->fillTriangle(cx + s, cy, cx, cy - s, cx, cy + s, c);
      gfx->fillRect(cx - s, cy - T, s, 2 * T, c);                 break;
    default: break;
  }
}

// A thick segment, as two triangles forming a sheared quad. The shear is
// invisible on short diagonal strokes and saves a rotation.
static void thickSeg(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t t, uint16_t c) {
  gfx->fillTriangle(x0, y0, x1, y1, x1, y1 + t, c);
  gfx->fillTriangle(x0, y0, x1, y1 + t, x0, y0 + t, c);
}

static void drawCheck(int16_t cx, int16_t cy, uint16_t c) {
  thickSeg(cx - 16, cy - 2, cx - 5, cy + 9, 8, c);
  thickSeg(cx - 5,  cy + 9, cx + 16, cy - 13, 8, c);
}

// Zwift's Ride On is a thumbs-up, so this is a thumbs-up.
//
// The first attempt drew a wrist, a fist and VERTICAL knuckle gaps, and it
// read as three raised fingers — a rude gesture, not a compliment. The fix is
// anatomical: seen from the side, curled fingers stack HORIZONTALLY, and the
// thumb rises from the near edge of the fist rather than its centre. Two
// creases are enough; a third starts to look like corduroy.
static void drawThumbUp(int16_t cx, int16_t cy, uint16_t c, uint16_t fill) {
  gfx->fillRoundRect(cx - 17, cy - 2,  34, 20, 6, c);   // fist
  gfx->fillRoundRect(cx - 14, cy - 19, 12, 19, 5, c);   // thumb, up from the left
  gfx->fillRect(cx - 1, cy + 4,  14, 1, fill);          // finger creases
  gfx->fillRect(cx - 1, cy + 11, 14, 1, fill);
}

// A 2px stroke at any angle: the line plus both single-pixel offsets.
static void stroke2(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) {
  gfx->drawLine(x0, y0, x1, y1, c);
  gfx->drawLine(x0 + 1, y0, x1 + 1, y1, c);
  gfx->drawLine(x0, y0 + 1, x1, y1 + 1, c);
}

// The garage is where the bikes live, so the button is a bike. Diamond frame,
// two wheels, saddle and bars — enough for the eye to name it instantly.
static void drawBike(int16_t cx, int16_t cy, uint16_t c) {
  const int16_t rh_x = cx - 22, fh_x = cx + 22, hub_y = cy + 7;
  const int16_t bb_x = cx - 1,  st_x = cx - 9,  st_y   = cy - 9;
  const int16_t ht_x = cx + 13, ht_y = cy - 11;

  for (int16_t r = 12; r >= 11; r--) {            // 2px rims
    gfx->drawCircle(rh_x, hub_y, r, c);
    gfx->drawCircle(fh_x, hub_y, r, c);
  }
  stroke2(rh_x, hub_y, st_x, st_y, c);            // seat stay
  stroke2(st_x, st_y, bb_x, hub_y, c);            // seat tube
  stroke2(bb_x, hub_y, rh_x, hub_y, c);           // chain stay
  stroke2(st_x, st_y, ht_x, ht_y, c);             // top tube
  stroke2(ht_x, ht_y, bb_x, hub_y, c);            // down tube
  stroke2(ht_x, ht_y, fh_x, hub_y, c);            // fork
  stroke2(cx - 16, st_y - 3, cx - 4, st_y - 3, c);// saddle
  stroke2(ht_x - 2, ht_y - 3, ht_x + 8, ht_y - 3, c); // bars
}

// --- Buttons ---------------------------------------------------------------

static void drawPadButton(const PadButton &b, bool pressed) {
  // The AUTO toggle colours itself from its state rather than a fixed colour,
  // so the button IS the indicator. Three states, not two:
  //
  //    STEEL  — off
  //    GREEN  — armed AND connected, i.e. genuinely sending
  //    AMBER  — armed but DISCONNECTED, so nothing is going out
  //
  // The amber case is the one that matters. Sending is gated on `connected`,
  // so after a reflash or a dropped link the repeat silently stalls — and an
  // indicator that reads "running" while nothing happens is worse than no
  // indicator at all. Never let a status light show intent instead of reality.
  uint16_t accent;
  if (b.kind == PadKind::ToggleAuto) {
    accent = !autoRideOn            ? C_STEEL
           : keyboard.isConnected() ? C_GREEN
                                    : C_AMBER;
  } else {
    accent = b.color;
  }

  // Pressed INVERTS the button rather than merely tinting it. A tint that keeps
  // white ink on a paler fill destroys the very contrast that made the glyph
  // findable — and the pressed state is the one you look at while holding a
  // turn. Whatever the state, ink and fill stay far apart.
  uint16_t fill, ink, sub, rim;
  if (b.style == PadStyle::Solid) {
    fill = pressed ? mix565(accent, C_WHITE, 175) : accent;
    ink  = pressed ? accent : C_WHITE;   // dark-on-light while held
    sub  = ink;
    // A rim one shade off the fill reads as a lit edge and lifts the slab off
    // the background — the cheapest depth cue there is, one drawRoundRect.
    rim  = pressed ? C_WHITE : mix565(accent, C_WHITE, 70);
  } else {
    fill = pressed ? accent : C_SURFACE;
    ink  = pressed ? inkOn(accent) : accent;
    sub  = pressed ? inkOn(accent) : C_MUTED;
    rim  = pressed ? C_WHITE : accent;
  }

  gfx->fillRoundRect(b.x, b.y, b.w, b.h, RADIUS, fill);
  gfx->drawRoundRect(b.x, b.y, b.w, b.h, RADIUS, rim);
  gfx->drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, RADIUS - 1, rim);

  // AUTO advertises its own cadence, derived from the constant so the label can
  // never drift away from what the code actually does.
  char autoCap[8];
  const char *caption = b.caption;
  if (b.kind == PadKind::ToggleAuto) {
    snprintf(autoCap, sizeof(autoCap), "%luS", (unsigned long)(RIDE_ON_INTERVAL_MS / 1000));
    caption = autoCap;
  }

  const bool  hasCap = caption[0] != '\0';
  const int16_t cx   = b.x + b.w / 2;
  // Captioned buttons lift their glyph to keep the pair optically centred.
  const int16_t cy   = b.y + (hasCap ? (b.h - 14) / 2 : b.h / 2);

  switch (b.glyph) {
    case Glyph::Up: case Glyph::Down: case Glyph::Left: case Glyph::Right:
      drawArrow(b.glyph, cx, cy, 17, ink);          break;
    case Glyph::Check:   drawCheck(cx, cy, ink);    break;
    case Glyph::ThumbUp: drawThumbUp(cx, cy, ink, fill); break;
    case Glyph::Bike:    drawBike(cx, cy, ink);     break;
    case Glyph::None:    break;
  }

  if (b.label[0]) textCentred(b.label, fitLabel(b.label, b.w - 14), cx, cy, ink);
  if (hasCap)     textCentred(caption, &ZwiftSmall11, cx, b.y + b.h - 13, sub);
}

// The two unused corners of the 3x3 grid, drawn as shallow recesses. Left
// black they looked like a rendering bug; given an edge they read as a
// deliberate cross-shaped D-pad.
static void drawRecess(int16_t x, int16_t y) {
  gfx->fillRoundRect(x, y, CW, CH, RADIUS, mix565(C_BG, C_SURFACE, 110));
  gfx->drawRoundRect(x, y, CW, CH, RADIUS, C_LINE);
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
  gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_BG);

  // Brand mark: a slanted orange bar, in the spirit of Zwift's own chevron
  // without copying the logo. Two triangles make the parallelogram.
  gfx->fillTriangle(4, 32, 11, 11, 17, 11, C_ORANGE);
  gfx->fillTriangle(4, 32, 17, 11, 10, 32, C_ORANGE);

  // "ZWIFT" in brand orange, "REMOTE" in white: says whose ecosystem this
  // belongs to and that it is not the game itself, in one line.
  int16_t x = 23;
  textAt("ZWIFT", &ZwiftBold18, x, 31, C_ORANGE);
  x += textWidth("ZWIFT", &ZwiftBold18) + 7;
  textAt("REMOTE", &ZwiftBold18, x, 31, C_WHITE);

  // The number this whole stage exists to produce, kept but demoted to small
  // print — it is instrumentation, not something you read while riding.
  if (lastWakeMs > 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "WAKE %luMS", (unsigned long)lastWakeMs);
    textRight(buf, &ZwiftSmall11, SCREEN_W - 6, 30, C_MUTED);
  }

  // Status pill: dot plus word, ringed in the state colour. A filled dot is
  // readable at arm's length when the word is not.
  const char *state = connected ? "CONNECTED" : "PAIRING";
  uint16_t    col   = connected ? C_GREEN : C_AMBER;
  int16_t     pw    = textWidth(state, &ZwiftBold14) + 36;
  gfx->fillRoundRect(4, 46, pw, 26, 13, C_SURFACE);
  gfx->drawRoundRect(4, 46, pw, 26, 13, col);
  gfx->fillCircle(19, 59, 5, col);
  textAt(state, &ZwiftBold14, 30, 64, col);

  // Ride On counter, colour-matched to the AUTO button: green while it is
  // really firing, amber while armed but stalled on a dropped link.
  if (autoRideOn) {
    char buf[16];
    snprintf(buf, sizeof(buf), "RO %lu", (unsigned long)rideOnCount);
    textRight(buf, &ZwiftBold14, SCREEN_W - 6, 64, connected ? C_GREEN : C_AMBER);
  }

  gfx->drawFastHLine(0, HEADER_H - 4, SCREEN_W, C_LINE);
}

static void drawFooter() {
  gfx->drawFastHLine(0, 404, SCREEN_W, C_LINE);
  textAt("ROCKER  L/R ARROWS, WORKS SCREEN OFF", &ZwiftSmall11, 6, 424, C_MUTED);
  textAt("GPIO0   WAKE, TAP=ENTER, HOLD=ESC",    &ZwiftSmall11, 6, 440, C_MUTED);
  if (bondsCleared) {
    textAt("BONDS CLEARED - FORGET DEVICE ON PC", &ZwiftSmall11, 6, 456, C_AMBER);
  } else {
    textAt("BOTH ROCKERS AT BOOT = CLEAR BONDS",  &ZwiftSmall11, 6, 456,
           mix565(C_LINE, C_MUTED, 120));
  }
}

static void drawAll(bool connected) {
  gfx->fillScreen(C_BG);
  drawStatus(connected);
  drawRecess(COL0, ROW2);
  drawRecess(COL2, ROW2);
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
    Serial.printf("[pad] release %s\n", padName(*activePad));
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
      Serial.printf("[pad] press %s%s\n", padName(b),
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
  gfx->fillScreen(C_BG);
  // Every string on this screen uses a proportional font, and the integer
  // scaler exists only for the built-in 5x7 bitmap. Set it once to 1 and never
  // touch it again: scaling a real font just multiplies its pixels.
  gfx->setTextSize(1);

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
