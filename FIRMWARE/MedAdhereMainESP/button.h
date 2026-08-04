#pragma once
#include <Arduino.h>
#include "config.h"

enum class ButtonEvent { NONE, SHORT_PRESS, LONG_PRESS, FACTORY_RESET };

static bool s_btnDown = false;
static unsigned long s_btnDownAt = 0;
static bool s_longFired = false;
static bool s_resetFired = false;

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
    s_resetFired = false;
  } else if (pressed && s_btnDown) {
    // checked before LONG_PRESS_MS so a continued hold escalates past it
    if (!s_resetFired && millis() - s_btnDownAt >= FACTORY_RESET_HOLD_MS) {
      s_resetFired = true;
      return ButtonEvent::FACTORY_RESET;
    }
    if (!s_longFired && millis() - s_btnDownAt >= LONG_PRESS_MS) {
      s_longFired = true;
      return ButtonEvent::LONG_PRESS;
    }
  } else if (!pressed && s_btnDown) {
    s_btnDown = false;
    if (!s_longFired && !s_resetFired) {
      return ButtonEvent::SHORT_PRESS;
    }
  }
  return ButtonEvent::NONE;
}
