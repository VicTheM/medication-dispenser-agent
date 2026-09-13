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
  digitalWrite(PIN_BUZZER, HIGH);
}

void indicatorsSetPattern(IndicatorPattern pattern) {
  if (pattern == s_pattern) return;
  s_pattern = pattern;
  s_phaseStart = millis();
  s_step = 0;
  digitalWrite(PIN_BUZZER, HIGH);
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
      digitalWrite(PIN_BUZZER, HIGH);
      break;

    case IndicatorPattern::IDLE_OK:
      // slow, dim-feeling green heartbeat: brief pulse every 3s
      digitalWrite(PIN_LED_GREEN, inWindow(elapsed, 3000, 0, 120) ? HIGH : LOW);
      digitalWrite(PIN_LED_RED, LOW);
      digitalWrite(PIN_LED_BLUE, LOW);
      digitalWrite(PIN_BUZZER, HIGH);
      break;

    case IndicatorPattern::OFFLINE:
      digitalWrite(PIN_LED_RED, inWindow(elapsed, 1500, 0, 750) ? HIGH : LOW);
      digitalWrite(PIN_LED_GREEN, LOW);
      digitalWrite(PIN_LED_BLUE, LOW);
      digitalWrite(PIN_BUZZER, HIGH);
      break;

    case IndicatorPattern::WAITING_APPROACH:
      digitalWrite(PIN_LED_BLUE, inWindow(elapsed, 1000, 0, 500) ? HIGH : LOW);
      digitalWrite(PIN_BUZZER, inWindow(elapsed, 1000, 0, 180) ? LOW : HIGH);
      break;

    case IndicatorPattern::PERSON_APPROACHED:
      digitalWrite(PIN_LED_GREEN, inWindow(elapsed, 400, 0, 200) ? HIGH : LOW);
      digitalWrite(PIN_BUZZER, inWindow(elapsed, 400, 0, 100) ? LOW : HIGH);
      break;

    case IndicatorPattern::DISPENSING:
      digitalWrite(PIN_LED_BLUE, HIGH);
      digitalWrite(PIN_BUZZER, HIGH);
      break;

    case IndicatorPattern::PICKUP_OK:
      digitalWrite(PIN_BUZZER, HIGH);
      if (elapsed < 120) { digitalWrite(PIN_LED_GREEN, HIGH); }
      else if (elapsed < 220) { digitalWrite(PIN_LED_GREEN, LOW); }
      else if (elapsed < 340) { digitalWrite(PIN_LED_GREEN, HIGH); }
      else if (elapsed < 1200) { digitalWrite(PIN_LED_GREEN, LOW); }
      else { indicatorsSetPattern(IndicatorPattern::IDLE_OK); }
      break;

    case IndicatorPattern::PICKUP_MISSED:
      digitalWrite(PIN_LED_RED, inWindow(elapsed, 2000, 0, 1000) ? HIGH : LOW);
      if (inWindow(elapsed, 2000, 0, 150) || inWindow(elapsed, 2000, 250, 400) || inWindow(elapsed, 2000, 500, 650)) {
        digitalWrite(PIN_BUZZER, LOW);
      } else {
        digitalWrite(PIN_BUZZER, HIGH);
      }
      break;

    case IndicatorPattern::ERROR_PATTERN:
      digitalWrite(PIN_LED_RED, inWindow(elapsed, 900, 0, 450) ? HIGH : LOW);
      if (inWindow(elapsed, 900, 0, 100) || inWindow(elapsed, 900, 150, 250) || inWindow(elapsed, 900, 300, 400)) {
        digitalWrite(PIN_BUZZER, LOW);
      } else {
        digitalWrite(PIN_BUZZER, HIGH);
      }
      break;

    case IndicatorPattern::CONFIG_MODE:
      digitalWrite(PIN_LED_BLUE, inWindow(elapsed, 1200, 0, 600) ? HIGH : LOW);
      digitalWrite(PIN_LED_GREEN, inWindow(elapsed, 1200, 600, 1200) ? HIGH : LOW);
      digitalWrite(PIN_BUZZER, LOW);
      break;
  }
}
