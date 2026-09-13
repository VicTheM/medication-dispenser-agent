#pragma once
#include <Arduino.h>

enum class IndicatorPattern {
  OFF,
  IDLE_OK,          // solid dim green heartbeat - normal operation, WS connected
  OFFLINE,          // slow red blink - no connectivity
  WAITING_APPROACH, // digital buzzer pulse + blue blink - "time to take medication", waiting for approach
  PERSON_APPROACHED,// faster digital buzzer pulse + green blink - person is within range
  DISPENSING,       // solid blue, no beep
  PICKUP_OK,        // short double chime + green flash - pickup confirmed
  PICKUP_MISSED,    // slow triple buzzer pulse + red flash - pickup not confirmed in time
  ERROR_PATTERN,    // fast triple buzzer pulse + red blink - hardware/backend error
  CONFIG_MODE,      // blue/green alternate with buzzer on - config portal active
};

void indicatorsInit();
void indicatorsSetPattern(IndicatorPattern pattern);
void indicatorsTick(); // call every loop() iteration - non-blocking
