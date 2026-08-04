#include "indicators.h"
#include "config.h"

static IndicatorPattern s_pattern = IndicatorPattern::OFF;
static unsigned long s_phaseStart = 0;
static int s_step = 0; // generic step counter, meaning depends on pattern

static void allLedsOff() {
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
}

void indicatorsInit() {
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  allLedsOff();
  noTone(PIN_BUZZER);
}

void indicatorsSetPattern(IndicatorPattern pattern) {
  if (pattern == s_pattern) return;
  s_pattern = pattern;
  s_phaseStart = millis();
  s_step = 0;
  noTone(PIN_BUZZER);
  allLedsOff();
}

// Small helper: is `elapsed` currently within [a,b) of a repeating cycle of length `period`?
static bool inWindow(unsigned long elapsed, unsigned long period, unsigned long a, unsigned long b) {
  unsigned long phase = elapsed % period;
  return phase >= a && phase < b;
}

void indicatorsTick() {
  unsigned long elapsed = millis() - s_phaseStart;

  switch (s_pattern) {
    case IndicatorPattern::OFF:
      break;

    case IndicatorPattern::IDLE_OK:
      // slow, dim-feeling green heartbeat: brief pulse every 3s
      digitalWrite(PIN_LED_GREEN, inWindow(elapsed, 3000, 0, 120) ? HIGH : LOW);
      digitalWrite(PIN_LED_RED, LOW);
      digitalWrite(PIN_LED_BLUE, LOW);
      break;

    case IndicatorPattern::OFFLINE:
      digitalWrite(PIN_LED_RED, inWindow(elapsed, 1500, 0, 750) ? HIGH : LOW);
      digitalWrite(PIN_LED_GREEN, LOW);
      digitalWrite(PIN_LED_BLUE, LOW);
      break;

    case IndicatorPattern::WAITING_APPROACH:
      // "rings the buzzer while waiting for the person to approach"
      digitalWrite(PIN_LED_BLUE, inWindow(elapsed, 1000, 0, 500) ? HIGH : LOW);
      if (inWindow(elapsed, 1000, 0, 180)) {
        tone(PIN_BUZZER, 1200);
      } else {
        noTone(PIN_BUZZER);
      }
      break;

    case IndicatorPattern::PERSON_APPROACHED:
      // "changes the sound" once within range - faster, higher pitch
      digitalWrite(PIN_LED_GREEN, inWindow(elapsed, 400, 0, 200) ? HIGH : LOW);
      if (inWindow(elapsed, 400, 0, 100)) {
        tone(PIN_BUZZER, 2200);
      } else {
        noTone(PIN_BUZZER);
      }
      break;

    case IndicatorPattern::DISPENSING:
      digitalWrite(PIN_LED_BLUE, HIGH);
      noTone(PIN_BUZZER);
      break;

    case IndicatorPattern::PICKUP_OK:
      // two quick chimes then settle - drawn from a short fixed sequence
      if (elapsed < 120) { tone(PIN_BUZZER, 2600); digitalWrite(PIN_LED_GREEN, HIGH); }
      else if (elapsed < 220) { noTone(PIN_BUZZER); digitalWrite(PIN_LED_GREEN, LOW); }
      else if (elapsed < 340) { tone(PIN_BUZZER, 2600); digitalWrite(PIN_LED_GREEN, HIGH); }
      else if (elapsed < 1200) { noTone(PIN_BUZZER); digitalWrite(PIN_LED_GREEN, LOW); }
      else { indicatorsSetPattern(IndicatorPattern::IDLE_OK); }
      break;

    case IndicatorPattern::PICKUP_MISSED:
      digitalWrite(PIN_LED_RED, inWindow(elapsed, 2000, 0, 1000) ? HIGH : LOW);
      if (inWindow(elapsed, 2000, 0, 150) || inWindow(elapsed, 2000, 250, 400) || inWindow(elapsed, 2000, 500, 650)) {
        tone(PIN_BUZZER, 900);
      } else {
        noTone(PIN_BUZZER);
      }
      break;

    case IndicatorPattern::ERROR_PATTERN:
      digitalWrite(PIN_LED_RED, inWindow(elapsed, 900, 0, 450) ? HIGH : LOW);
      if (inWindow(elapsed, 900, 0, 100) || inWindow(elapsed, 900, 150, 250) || inWindow(elapsed, 900, 300, 400)) {
        tone(PIN_BUZZER, 700);
      } else {
        noTone(PIN_BUZZER);
      }
      break;

    case IndicatorPattern::CONFIG_MODE:
      digitalWrite(PIN_LED_BLUE, inWindow(elapsed, 1200, 0, 600) ? HIGH : LOW);
      digitalWrite(PIN_LED_GREEN, inWindow(elapsed, 1200, 600, 1200) ? HIGH : LOW);
      noTone(PIN_BUZZER);
      break;
  }
}
