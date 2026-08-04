#pragma once
#include <Arduino.h>

void displayInit();
void displayMessage(const String &line1, const String &line2);
// Sets the persistent "idle" screen content; loop() should call
// displayIdleTick() regularly to keep the countdown and any scrolling
// medication-name text updating without blocking anything else.
void displayShowIdle(const String &nextTimeLabel, const String &medNames);
void displayIdleTick();
void displayClear();
