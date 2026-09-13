#include "sensors.h"
#include "config.h"
#include "storage.h"

// =======================================================================
// Ultrasonic
// =======================================================================
void ultrasonicInit() {
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
}

float ultrasonicReadCM() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duration = pulseIn(PIN_ECHO, HIGH, 30000UL); // 30ms timeout, ~5m max
  if (duration == 0) return -1;
  return (duration * SOUND_SPEED_CM_US) / 2.0f;
}

// =======================================================================
// IR beam-break ("laser") - the tray door is the normal obstacle
// =======================================================================
void beamInit() {
  pinMode(PIN_IR_BEAM, INPUT);
}

bool beamObstacleDetected() {
  return digitalRead(PIN_IR_BEAM) == LOW; // module is active-low; invert here if yours differs
}

// =======================================================================
// Carousel stepper - non-blocking, targets an absolute step position per
// compartment so rounding error never accumulates across many rotations.
// See DEVICE_BRIEF.md "Carousel homing" for the full rationale.
// =======================================================================
static const uint8_t STEP_SEQUENCE[4][4] = {
  {1, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 1},
  {1, 0, 0, 1},
};
static const uint32_t STEP_INTERVAL_US = 2000; // ~2ms/step - safe speed for 28BYJ-48

static long s_currentStep = 0;     // 0..STEPS_PER_REV-1
static long s_targetStep = 0;
static long s_remainingSteps = 0;
static int8_t s_direction = 1;
static uint8_t s_phase = 0;
static bool s_moving = false;
static unsigned long s_lastStepMicros = 0;
static int8_t s_currentIndex = -1; // -1 = uncalibrated

static float stepsPerCompartment() {
  return STEPS_PER_REV / (float)NUM_COMPARTMENTS;
}

static void writeStepPins(uint8_t phase) {
  digitalWrite(PIN_STEP_IN1, STEP_SEQUENCE[phase][0]);
  digitalWrite(PIN_STEP_IN2, STEP_SEQUENCE[phase][1]);
  digitalWrite(PIN_STEP_IN3, STEP_SEQUENCE[phase][2]);
  digitalWrite(PIN_STEP_IN4, STEP_SEQUENCE[phase][3]);
}

static void deenergizeCoils() {
  digitalWrite(PIN_STEP_IN1, LOW);
  digitalWrite(PIN_STEP_IN2, LOW);
  digitalWrite(PIN_STEP_IN3, LOW);
  digitalWrite(PIN_STEP_IN4, LOW);
}

void carouselInit() {
  pinMode(PIN_STEP_IN1, OUTPUT);
  pinMode(PIN_STEP_IN2, OUTPUT);
  pinMode(PIN_STEP_IN3, OUTPUT);
  pinMode(PIN_STEP_IN4, OUTPUT);
  deenergizeCoils();

  int8_t savedIndex = storageLoadCarouselPos();
  if (savedIndex >= 0 && savedIndex < NUM_COMPARTMENTS) {
    s_currentIndex = savedIndex;
    s_currentStep = lround(savedIndex * stepsPerCompartment()) % STEPS_PER_REV;
  } else {
    s_currentIndex = -1; // needs calibration - main firmware should check this
    s_currentStep = 0;
  }
}

bool carouselIsMoving() {
  return s_moving;
}

uint8_t carouselCurrentIndex() {
  return s_currentIndex < 0 ? 0 : s_currentIndex;
}

void carouselGoTo(uint8_t targetIndex) {
  if (targetIndex >= NUM_COMPARTMENTS) return;

  long targetStep = lround(targetIndex * stepsPerCompartment()) % STEPS_PER_REV;
  long delta = targetStep - s_currentStep;

  // normalize to the shortest direction around the carousel
  while (delta > STEPS_PER_REV / 2) delta -= STEPS_PER_REV;
  while (delta <= -STEPS_PER_REV / 2) delta += STEPS_PER_REV;

  s_targetStep = targetStep;
  s_remainingSteps = labs(delta);
  s_direction = (delta >= 0) ? 1 : -1;
  s_currentIndex = targetIndex;
  s_moving = s_remainingSteps > 0;
  s_lastStepMicros = micros();
}

void carouselTick() {
  if (!s_moving) return;

  if (micros() - s_lastStepMicros < STEP_INTERVAL_US) return;
  s_lastStepMicros = micros();

  s_phase = (uint8_t)((s_phase + s_direction + 4) % 4);
  writeStepPins(s_phase);
  s_currentStep = (s_currentStep + s_direction + STEPS_PER_REV) % STEPS_PER_REV;
  s_remainingSteps--;

  if (s_remainingSteps <= 0) {
    s_moving = false;
    deenergizeCoils(); // don't hold coils energized - saves power, avoids heat
    storageSaveCarouselPos(s_currentIndex);
  }
}

void carouselCalibrateHere() {
  s_currentStep = 0;
  s_currentIndex = 0;
  s_moving = false;
  deenergizeCoils();
  storageSaveCarouselPos(0);
}
