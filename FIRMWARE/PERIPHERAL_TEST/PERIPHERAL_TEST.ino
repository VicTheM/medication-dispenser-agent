/*
  ============================================================
  ESP32 MULTI-COMPONENT TEST SKETCH
  ============================================================
  Tests, one function per component:
    1. HC-SR04 Ultrasonic Sensor      (Trig/Echo)
    2. MG996R Servo Motor             (PWM)
    3. 28BYJ-48 Stepper + ULN2003     (4-wire driver)
    4. 16x2 I2C LCD                   (I2C)
    5. Buzzer                         (Digital/Tone)
    6. Push Button                    (Interrupt, INPUT_PULLUP)
    7. IR Sender/Receiver module      (Digital obstacle/distance)

  REQUIRED LIBRARIES (install via Library Manager):
    - ESP32Servo          by Kevin Harrington / John K. Bennett
    - LiquidCrystal I2C   by Frank de Brabander (or Marco Schwartz)
    - Stepper             (built-in, comes with Arduino IDE)

  Wiring summary (change pins below if yours differ):
    HC-SR04   : TRIG->GPIO5   ECHO->GPIO18 (use a voltage divider
                on ECHO, since it's 5V logic and ESP32 is 3.3V)
    MG996R    : Signal->GPIO13 (external 5-6V supply for the servo,
                common GND with ESP32)
    28BYJ-48  : IN1->GPIO14 IN2->GPIO27 IN3->GPIO26 IN4->GPIO25
                (ULN2003 powered from external 5V, common GND)
    LCD I2C   : SDA->GPIO21  SCL->GPIO22  (addr usually 0x27 or 0x3F)
    Buzzer    : +  ->GPIO4
    Button    : one leg->GPIO15, other leg->GND (internal pull-up used)
    IR module : OUT->GPIO34 (input-only pin, fine for digital read)
  ============================================================
*/

#include <Wire.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>

// ------------------------------------------------------------
// 1. ULTRASONIC SENSOR CONFIG
// ------------------------------------------------------------
#define TRIG_PIN        5
#define ECHO_PIN        18
#define SOUND_SPEED_CM  0.0343  // cm per microsecond

// ------------------------------------------------------------
// 2. SERVO MOTOR CONFIG
// ------------------------------------------------------------
#define SERVO_PIN       13
Servo mg996rServo;

// ------------------------------------------------------------
// 3. STEPPER MOTOR CONFIG (28BYJ-48 + ULN2003)
// ------------------------------------------------------------
#define STEPPER_IN1     14
#define STEPPER_IN2     27
#define STEPPER_IN3     26
#define STEPPER_IN4     25
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
#define BUTTON_PIN      15
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
#define IR_PIN          34
#define IR_ANALOG_PIN   32
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

  // Servo
  ESP32PWM::allocateTimer(0);
  mg996rServo.setPeriodHertz(50);       // standard 50Hz servo
  mg996rServo.attach(SERVO_PIN, 500, 2400);

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
  digitalWrite(BUZZER_PIN, LOW);

  // Button + interrupt (default pull-up, active LOW)
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);

  // IR module
  pinMode(IR_PIN, INPUT);
  if (IR_HAS_ANALOG) {
    pinMode(IR_ANALOG_PIN, INPUT);
  }

  delay(1000);
  lcd.clear();
  Serial.println("Setup complete. Starting test loop.\n");
}


// ============================================================
// LOOP - cycles through each component test, button works anytime
// ============================================================
void loop() {
  checkButton();       // non-blocking, always checked

  testUltrasonic();
  checkButton();
  delay(TEST_DELAY_MS);

  testServo();
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
  if (distance < 0) {
    Serial.println("Out of range");
  } else {
    Serial.print(distance);
    Serial.println(" cm");
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ultrasonic:");
  lcd.setCursor(0, 1);
  if (distance < 0) {
    lcd.print("Out of range");
  } else {
    lcd.print(distance);
    lcd.print(" cm");
  }
}

// ---------- 2. SERVO ----------
void testServo() {
  Serial.println("[Servo] Sweeping 0 -> 90 -> 180 -> 90");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Servo Test");

  moveServoTo(0);
  delay(500);
  moveServoTo(90);
  delay(500);
  moveServoTo(180);
  delay(500);
  moveServoTo(90);
}

void moveServoTo(int angle) {
  angle = constrain(angle, 0, 180);
  mg996rServo.write(angle);
  Serial.print("[Servo] Angle set to: ");
  Serial.println(angle);

  lcd.setCursor(0, 1);
  lcd.print("Angle: ");
  lcd.print(angle);
  lcd.print("   "); // clear trailing chars
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
    tone(BUZZER_PIN, 2000);   // 2kHz beep
    delay(150);
    noTone(BUZZER_PIN);
    delay(150);
  }
}

// ---------- 6. BUTTON (interrupt-driven) ----------
void checkButton() {
  if (buttonFlag) {
    buttonFlag = false; // clear flag
    Serial.println("[Button] Pressed! (interrupt triggered)");
    printToLCD("Button Pressed!", "Interrupt OK");
    tone(BUZZER_PIN, 1500, 100); // quick confirmation beep
    delay(1000);
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

  if (IR_HAS_ANALOG) {
    int raw = readIRAnalogRaw();
    Serial.print("[IR] Analog raw: ");
    Serial.println(raw);
  }
}

// Digital IR read: most obstacle-avoidance IR modules pull the
// output LOW when an object reflects the IR beam back.
bool readIRDigital() {
  int val = digitalRead(IR_PIN);
  return (val == LOW); // change to (val == HIGH) if your module is active-high
}

// Optional: raw analog reading if module exposes an AO pin
int readIRAnalogRaw() {
  return analogRead(IR_ANALOG_PIN); // 0-4095 on ESP32 (12-bit ADC)
}
