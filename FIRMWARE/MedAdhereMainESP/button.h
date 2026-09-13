#pragma once
#include <Arduino.h>
#include "config.h"

enum class ButtonEvent { NONE, SHORT_PRESS, LONG_PRESS };

static bool s_btnDown = false;
static unsigned long s_btnDownAt = 0;
static bool s_longFired = false;
static unsigned long s_btnLastAcceptedAt = 0;

extern volatile bool s_btnPressPending;
void IRAM_ATTR buttonISR();

inline void buttonInit() {
  pinMode(PIN_BUTTON, INPUT_PULLUP); // button to GND
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), buttonISR, FALLING);
}

// Call every loop() iteration.
inline ButtonEvent buttonTick() {
  bool pressEdge = false;
  noInterrupts();
  if (s_btnPressPending) {
    s_btnPressPending = false;
    pressEdge = true;
  }
  interrupts();

  unsigned long now = millis();
  bool pressed = digitalRead(PIN_BUTTON) == LOW;

  if (pressEdge && !s_btnDown && now - s_btnLastAcceptedAt >= 50 && pressed) {
    s_btnDown = true;
    s_btnDownAt = now;
    s_longFired = false;
    s_btnLastAcceptedAt = now;
  } else if (pressed && s_btnDown) {
    if (!s_longFired && now - s_btnDownAt >= LONG_PRESS_MS) {
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
