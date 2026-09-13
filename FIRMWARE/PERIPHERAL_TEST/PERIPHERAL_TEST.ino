/*
  ============================================================
  ESP32 MULTI-COMPONENT TEST SKETCH
  ============================================================
  Tests, one function per component:
    1. HC-SR04 Ultrasonic Sensor      (Trig/Echo)
    3. 28BYJ-48 Stepper + ULN2003     (4-wire driver)
    4. 16x2 I2C LCD                   (I2C)
    5. Buzzer                         (Digital/Tone)
    6. Push Button                    (Interrupt, INPUT_PULLUP)
    7. IR Sender/Receiver module      (Digital obstacle/distance)

  REQUIRED LIBRARIES (install via Library Manager):
    - LiquidCrystal I2C   by Frank de Brabander (or Marco Schwartz)
    - Stepper             (built-in, comes with Arduino IDE)

  Wiring summary (change pins below if yours differ):
    HC-SR04   : TRIG->GPIO5   ECHO->GPIO18 (use a voltage divider
                on ECHO, since it's 5V logic and ESP32 is 3.3V)
    28BYJ-48  : IN1->GPIO14 IN2->GPIO27 IN3->GPIO26 IN4->GPIO25
                (ULN2003 powered from external 5V, common GND)
    LCD I2C   : SDA->GPIO21  SCL->GPIO22  (addr usually 0x27 or 0x3F)
    Buzzer    : +  ->GPIO4
    Button    : one leg->GPIO15, other leg->GND (internal pull-up used)
    IR module : OUT->GPIO34 (input-only pin, fine for digital read)
  ============================================================
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>
// ------------------------------------------------------------
// INDICATORS
// ------------------------------------------------------------
#define GREEN 13
#define RED 12
#define BLUE 2

// ------------------------------------------------------------
// 1. ULTRASONIC SENSOR CONFIG
// ------------------------------------------------------------
#define TRIG_PIN        5
#define ECHO_PIN        18
#define SOUND_SPEED_CM  0.0343  // cm per microsecond


// ------------------------------------------------------------
// 3. STEPPER MOTOR CONFIG (28BYJ-48 + ULN2003)
// ------------------------------------------------------------
#define STEPPER_IN1     19
#define STEPPER_IN2     14
#define STEPPER_IN3     23
#define STEPPER_IN4     15
#define STEPS_PER_REV   2048   // 28BYJ-48 with internal gearbox
// NOTE: Stepper library expects wiring order IN1-IN3-IN2-IN4
Stepper stepperMotor(STEPS_PER_REV, STEPPER_IN1, STEPPER_IN3, STEPPER_IN2, STEPPER_IN4);

// ------------------------------------------------------------
// 4. I2C LCD CONFIG
// ------------------------------------------------------------
#define LCD_ADDRESS     0x27   // change to 0x3F if 0x27 doesn't work
#define LCD_COLS        16
#define LCD_ROWS        2
#define I2C_SDA         21
#define I2C_SCL         22
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

// ------------------------------------------------------------
// 5. BUZZER CONFIG
// ------------------------------------------------------------
#define BUZZER_PIN      4

// ------------------------------------------------------------
// 6. BUTTON CONFIG (interrupt, default INPUT_PULLUP -> active LOW)
// ------------------------------------------------------------
#define BUTTON_PIN      35
volatile bool buttonFlag = false;
volatile unsigned long lastInterruptTime = 0;
#define DEBOUNCE_MS     200

void IRAM_ATTR handleButtonPress() {
  unsigned long now = millis();
  if (now - lastInterruptTime > DEBOUNCE_MS) {
    buttonFlag = true;
    lastInterruptTime = now;
  }
}

// ------------------------------------------------------------
// 7. IR SENDER/RECEIVER MODULE CONFIG (obstacle/distance module)
// ------------------------------------------------------------
// Most single-module IR pairs used for "distance" are analog-comparator
// obstacle sensors: digital OUT goes LOW when an object is detected.
// If your module also exposes an analog "AO" pin, wire it to an ADC
// pin (e.g. GPIO32) and set IR_HAS_ANALOG to true.
#define IR_PIN          32
#define IR_HAS_ANALOG   false

// ------------------------------------------------------------
// GENERAL
// ------------------------------------------------------------
#define TEST_DELAY_MS   3000


// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== ESP32 Multi-Component Test Boot ===");

  // Ultrasonic
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // indicators
  pinMode(GREEN, OUTPUT);
  pinMode(BLUE, OUTPUT);
  pinMode(RED, OUTPUT);


  // Stepper
  stepperMotor.setSpeed(10); // RPM

  // LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Booting..");

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  // Button + interrupt (default pull-up, active LOW)
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);

  // IR module
  pinMode(IR_PIN, INPUT);
  delay(1000);
  lcd.clear();
  Serial.println("Setup complete. Starting test loop.\n");

  digitalWrite(BLUE, HIGH);

  digitalWrite(RED, HIGH);

  digitalWrite(GREEN, HIGH);

}


// ============================================================
// LOOP - cycles through each component test, button works anytime
// ============================================================
void loop() {
  checkButton();       // non-blocking, always checked

  testUltrasonic();
  checkButton();
  delay(TEST_DELAY_MS);

  testStepper();
  checkButton();
  delay(TEST_DELAY_MS);

  testBuzzer();
  checkButton();
  delay(TEST_DELAY_MS);

  testIR();
  checkButton();
  delay(TEST_DELAY_MS);


  digitalWrite(BLUE, LOW);

  digitalWrite(RED, LOW);

  digitalWrite(GREEN, LOW);
}


// ============================================================
// COMPONENT FUNCTIONS
// ============================================================

// ---------- 1. ULTRASONIC ----------
float readUltrasonicDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout ~5m range
  if (duration == 0) return -1; // no echo / out of range

  float distance = (duration * SOUND_SPEED_CM) / 2.0;
  return distance;
}

void testUltrasonic() {
  float distance = readUltrasonicDistanceCM();
  Serial.print("[Ultrasonic] Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ultrasonic:");
  lcd.setCursor(0, 1);
  lcd.print(distance);
  lcd.print(" cm");
}

// ---------- 3. STEPPER ----------
void testStepper() {
  Serial.println("[Stepper] Rotating forward 1/4 turn");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Stepper Test");
  lcd.setCursor(0, 1);
  lcd.print("Forward...");

  stepperMotor.step(STEPS_PER_REV / 4);

  lcd.setCursor(0, 1);
  lcd.print("Backward...");
  Serial.println("[Stepper] Rotating backward 1/4 turn");

  stepperMotor.step(-STEPS_PER_REV / 4);
}

// ---------- 4. LCD ----------
// Generic helper other functions reuse; also runs its own demo below.
void printToLCD(const String &line1, const String &line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// ---------- 5. BUZZER ----------
void testBuzzer() {
  Serial.println("[Buzzer] Beeping 3 times");
  printToLCD("Buzzer Test", "Beeping...");

  for (int i = 0; i < 5; i++) {
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
  }
}

// ---------- 6. BUTTON (interrupt-driven) ----------
void checkButton() {
  if (buttonFlag) {
    buttonFlag = false; // clear flag
    Serial.println("[Button] Pressed! (interrupt triggered)");
    printToLCD("Button Pressed!", "Interrupt OK");
    digitalWrite(BUZZER_PIN, LOW);
    delay(1000);
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

// ---------- 7. IR SENDER/RECEIVER (obstacle / distance) ----------
void testIR() {
  bool objectDetected = readIRDigital();

  Serial.print("[IR] Object detected: ");
  Serial.println(objectDetected ? "YES" : "NO");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("IR Sensor Test");
  lcd.setCursor(0, 1);
  lcd.print(objectDetected ? "Object: YES" : "Object: NO");
}

// Digital IR read: most obstacle-avoidance IR modules pull the
// output LOW when an object reflects the IR beam back.
bool readIRDigital() {
  int val = digitalRead(IR_PIN);
  return (val == LOW); // change to (val == HIGH) if your module is active-high
}