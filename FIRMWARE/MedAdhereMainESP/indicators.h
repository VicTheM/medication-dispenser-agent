#pragma once
#include <Arduino.h>

enum class IndicatorPattern {
  OFF,
  IDLE_OK,          // solid dim green heartbeat - normal operation, WS connected
  OFFLINE,          // slow red blink - no connectivity
  WAITING_APPROACH, // slow low beep + blue blink - "time to take medication", waiting for approach
  PERSON_APPROACHED,// faster higher beep + green blink - person is within range
  DISPENSING,       // solid blue, no beep
  PICKUP_OK,        // short double chime + green flash - pickup confirmed
  PICKUP_MISSED,    // slow triple beep + red flash - pickup not confirmed in time
  ERROR_PATTERN,    // fast triple beep + red blink - hardware/backend error
  CONFIG_MODE,      // slow blue/green alternate, no beep - config portal active
};

void indicatorsInit();
void indicatorsSetPattern(IndicatorPattern pattern);
void indicatorsTick(); // call every loop() iteration - non-blocking
