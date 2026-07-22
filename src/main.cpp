// ============================================================================
//  Zwift Remote — Stage 1: minimal BLE HID keyboard
// ----------------------------------------------------------------------------
//  Goal of this stage: prove the WHOLE command path with the least code.
//  The board advertises itself as a Bluetooth keyboard; you pair it with your
//  computer once; then pressing a rocker types a Zwift keyboard shortcut.
//
//  Deliberately NO display this stage. If a rocker press lands a keystroke in a
//  text editor — and then opens the garage in Zwift — the risky part is done
//  and everything after it (on-screen buttons, battery, etc.) is decoration.
//
//  Keys we send (verified against Zwift's PC/Mac shortcuts):
//    T   -> Garage & Drop Shop (change bike / wheels / kit)
//    Esc -> Menu / paused screen  (also "back" inside menus)
// ============================================================================

#include <Arduino.h>
#include <HijelHID_BLEKeyboard.h>   // NimBLE-based BLE HID keyboard (core 3.x)

// ---------------------------------------------------------------------------
//  Which physical control sends which key.
//
//  The two rockers on this board are ACTIVE-LOW with external pull-ups (we
//  established this during board bring-up): the pin idles HIGH and reads LOW
//  only while the button is held. GPIO 0 is the shared BOOT button and is best
//  left for flashing, so we use the two free rocker halves, 12 and 16.
// ---------------------------------------------------------------------------
#define BTN_GARAGE  12   // -> Zwift 'T'  (Garage / Drop Shop)
#define BTN_MENU    16   // -> Zwift Esc  (Menu / pause)

// KEY_T (0x17) and KEY_ESCAPE (0x29) come from the library's BLEHIDKeys.h.
// For plain letters we could also just write('t'); we use the keycode form so
// every button — letters and non-printing keys like Esc — works the same way.

// The keyboard object. Think of `keyboard` like a global peripheral handle:
// keyboard.begin() powers up the BLE stack and starts advertising; after a
// computer connects, keyboard.tap(...) sends a key.
HijelHID_BLEKeyboard keyboard;

// ---------------------------------------------------------------------------
//  A tiny debounced edge-detector for one active-low button.
//
//  Mechanical buttons "bounce" — one physical press makes the electrical level
//  flicker for a few milliseconds. We only want ONE keystroke per press, so we
//  wait for the level to hold steady for DEBOUNCE_MS before believing it, then
//  report a press exactly once on the HIGH->LOW (released->pressed) edge.
//
//  In Python you might reach for a library; in C++ we keep the small amount of
//  per-button state in a struct and pass it by reference (the `&`).
// ---------------------------------------------------------------------------
static const uint32_t DEBOUNCE_MS = 25;

struct Button {
  uint8_t  pin;
  bool     stableHigh;    // last DEBOUNCED level (true = released/HIGH)
  bool     rawHigh;       // last RAW reading, used to time the debounce
  uint32_t lastEdgeMs;    // when the raw reading last changed
};

Button btnGarage;
Button btnMenu;

static void buttonInit(Button &b, uint8_t pin) {
  b.pin        = pin;
  // External pull-up already holds the line HIGH; enabling the internal
  // pull-up too is harmless and makes the code robust if the board changes.
  pinMode(pin, INPUT_PULLUP);
  b.stableHigh = (digitalRead(pin) == HIGH);
  b.rawHigh    = b.stableHigh;
  b.lastEdgeMs = millis();
}

// Returns true exactly once, on the moment the (debounced) button is pressed.
static bool buttonPressed(Button &b) {
  bool now = (digitalRead(b.pin) == HIGH);

  if (now != b.rawHigh) {          // raw level changed -> (re)start debounce timer
    b.rawHigh    = now;
    b.lastEdgeMs = millis();
  }

  // Level has been steady long enough to trust it?
  if ((millis() - b.lastEdgeMs) >= DEBOUNCE_MS && now != b.stableHigh) {
    b.stableHigh = now;
    if (!now) return true;         // just went HIGH->LOW == a fresh press
  }
  return false;
}

// ---------------------------------------------------------------------------
//  Connection-state logging.
//
//  Native-USB serial re-enumerates on reset, so the classic trick is to wait
//  briefly for the host to (re)open the port — but with a timeout so a headless
//  boot never hangs. (Same pattern the bring-up project settled on.)
// ---------------------------------------------------------------------------
static bool wasConnected = false;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  Serial.println();
  Serial.println("=== Zwift Remote — Stage 1: BLE HID keyboard ===");
  Serial.printf("GPIO %d -> 'T'  (garage)\n", BTN_GARAGE);
  Serial.printf("GPIO %d -> Esc  (menu)\n",   BTN_MENU);

  buttonInit(btnGarage, BTN_GARAGE);
  buttonInit(btnMenu,   BTN_MENU);

  // Start advertising as a Bluetooth keyboard. After this the device shows up
  // in your computer's Bluetooth list as a keyboard you can pair with.
  keyboard.begin();
  Serial.println("Advertising as a BLE keyboard — pair from your computer now.");
}

void loop() {
  // Report connect / disconnect transitions so you can SEE, on serial, the
  // moment the pairing takes (invisible state made visible — the running theme
  // of this whole board's project).
  bool connected = keyboard.isConnected();
  if (connected != wasConnected) {
    Serial.println(connected ? ">>> Computer connected."
                             : ">>> Computer disconnected — re-advertising.");
    wasConnected = connected;
  }

  // Poll the buttons every loop. A press always logs; the keystroke only goes
  // out if a computer is actually connected (tapping into the void is useless
  // and confusing, so we say so).
  if (buttonPressed(btnGarage)) {
    Serial.print("[GARAGE] ");
    if (connected) { keyboard.tap(KEY_T);      Serial.println("sent 'T'");   }
    else           {                           Serial.println("not connected"); }
  }

  if (buttonPressed(btnMenu)) {
    Serial.print("[MENU]   ");
    if (connected) { keyboard.tap(KEY_ESCAPE); Serial.println("sent Esc");  }
    else           {                           Serial.println("not connected"); }
  }

  delay(5);   // gentle poll; plenty fast for button presses
}
