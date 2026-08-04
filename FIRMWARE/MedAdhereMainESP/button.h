#pragma once
#include <Arduino.h>
#include "config.h"

enum class ButtonEvent { NONE, SHORT_PRESS, LONG_PRESS };

static bool s_btnDown = false;
static unsigned long s_btnDownAt = 0;
static bool s_longFired = false;

inline void buttonInit() {
  pinMode(PIN_BUTTON, INPUT_PULLUP); // button to GND
}

// Call every loop() iteration.
inline ButtonEvent buttonTick() {
  bool pressed = digitalRead(PIN_BUTTON) == LOW;

  if (pressed && !s_btnDown) {
    s_btnDown = true;
    s_btnDownAt = millis();
    s_longFired = false;
  } else if (pressed && s_btnDown) {
    if (!s_longFired && millis() - s_btnDownAt >= LONG_PRESS_MS) {
      s_longFired = true;
      return ButtonEvent::LONG_PRESS;
    }
  } else if (!pressed && s_btnDown) {
    s_btnDown = false;
    if (!s_longFired) {
      return ButtonEvent::SHORT_PRESS;
    }
  }
  return ButtonEvent::NONE;
}
