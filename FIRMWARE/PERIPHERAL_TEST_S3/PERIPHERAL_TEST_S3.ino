/*
  ============================================================
  ESP32-S3 MULTI-COMPONENT TEST SKETCH  (ported from DoIT ESP32)
  ============================================================
  Tests, one function per component:
    1. HC-SR04 Ultrasonic Sensor      (Trig/Echo)
    2. 28BYJ-48 Stepper + ULN2003     (4-wire driver)
    3. 16x2 I2C LCD                   (I2C)
    4. Buzzer                         (Digital/Tone)
    5. Push Button                    (Interrupt, INPUT_PULLUP)
    6. IR Sender/Receiver module      (Digital obstacle/distance)
    7. HX711 + 1kg Load Cell          (NEW - weight sensor)

  REQUIRED LIBRARIES (install via Library Manager):
    - LiquidCrystal I2C   by Frank de Brabander (or Marco Schwartz)
    - Stepper             (built-in, comes with Arduino IDE)
    - HX711               by bogde

  BOARD: ESP32-S3 WROOM (no camera, no SD card used in this build)

  ------------------------------------------------------------
  WHY THE PINS CHANGED FROM THE ORIGINAL ESP32 DOIT SKETCH:
  ------------------------------------------------------------
  The S3 has different restricted pins than the classic ESP32:
    - GPIO0, 3, 45, 46  -> strapping pins, must be free/floating at boot
    - GPIO19, 20        -> native USB D-/D+ on most S3-WROOM dev boards
    - GPIO26-37         -> wired to on-module SPI flash / octal PSRAM
                            on most WROOM-1(N16R8 etc) boards - DO NOT USE
    - GPIO43, 44        -> default Serial0 TX/RX (USB-serial debug) - left free
    - GPIO17, 18        -> intentionally RESERVED/UNUSED here for your
                            future Serial2.begin(baud, SERIAL_8N1, 17, 18)
                            (S3 UARTs are fully pin-remappable, so these
                            are just a placeholder - pick any free pins
                            when you actually wire Serial2 hardware)

  Wiring summary (change pins below if yours differ):
    HC-SR04   : TRIG->GPIO7   ECHO->GPIO15 (use a voltage divider
                on ECHO, since it's 5V logic and S3 is 3.3V)
    28BYJ-48  : IN1->GPIO8  IN2->GPIO9  IN3->GPIO10  IN4->GPIO11
                (ULN2003 powered from external 5V, common GND)
    LCD I2C   : SDA->GPIO12  SCL->GPIO13  (addr usuallywwedf  0x27 or 0x3F)
    Buzzer    : +  ->GPIO14
    Button    : one leg->GPIO21, other leg->GND (internal pull-up used)
    IR module : OUT->GPIO38
    HX711     : DOUT->GPIO39   SCK->GPIO40
                (load cell E+/E-/A+/A- -> HX711 bridge inputs,
                 HX711 VCC->3.3V, GND->GND)
    Indicators: GREEN->GPIO4  RED->GPIO5  BLUE->GPIO6

    RESERVED (do not use elsewhere): GPIO17, GPIO18 -> future Serial2
  ============================================================
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>
#include <HX711.h>

// ------------------------------------------------------------
// INDICATORS
// ------------------------------------------------------------
#define GREEN 4
#define RED   5
#define BLUE  6

// ------------------------------------------------------------
// 1. ULTRASONIC SENSOR CONFIG
// ------------------------------------------------------------
#define TRIG_PIN        7
#define ECHO_PIN        15
#define SOUND_SPEED_CM  0.0343  // cm per microsecond

// ------------------------------------------------------------
// 2. STEPPER MOTOR CONFIG (28BYJ-48 + ULN2003)
// ------------------------------------------------------------
#define STEPPER_IN1     8
#define STEPPER_IN2     9
#define STEPPER_IN3     10
#define STEPPER_IN4     11
#define STEPS_PER_REV   2048   // 28BYJ-48 with internal gearbox
// NOTE: Stepper library expects wiring order IN1-IN3-IN2-IN4
Stepper stepperMotor(STEPS_PER_REV, STEPPER_IN1, STEPPER_IN3, STEPPER_IN2, STEPPER_IN4);

// ------------------------------------------------------------
// 3. I2C LCD CONFIG
// ------------------------------------------------------------
#define LCD_ADDRESS     0x27   // change to 0x3F if 0x27 doesn't work
#define LCD_COLS        16
#define LCD_ROWS        2
#define I2C_SDA         12
#define I2C_SCL         13
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

// ------------------------------------------------------------
// 4. BUZZER CONFIG
// ------------------------------------------------------------
#define BUZZER_PIN      14

// ------------------------------------------------------------
// 5. BUTTON CONFIG (interrupt, default INPUT_PULLUP -> active LOW)
// ------------------------------------------------------------
#define BUTTON_PIN      21
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
// 6. IR SENDER/RECEIVER MODULE CONFIG (obstacle/distance module)
// ------------------------------------------------------------
#define IR_PIN          38
#define IR_HAS_ANALOG   false

// ------------------------------------------------------------
// 7. HX711 + 1KG LOAD CELL CONFIG  (NEW)
// ------------------------------------------------------------
#define HX711_DOUT      39
#define HX711_SCK       40
// Placeholder calibration factor - you MUST calibrate this for your
// specific 1kg load cell (see calibrateLoadCell() notes below).
#define HX711_CAL_FACTOR  -7050.0
HX711 scale;

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
  Serial.println("=== ESP32-S3 Multi-Component Test Boot ===");

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
  digitalWrite(BUZZER_PIN, LOW);

  // Button + interrupt (default pull-up, active LOW)
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);

  // IR module
  pinMode(IR_PIN, INPUT);

  // HX711 load cell
  scale.begin(HX711_DOUT, HX711_SCK);
  if (scale.is_ready()) {
    scale.set_scale(HX711_CAL_FACTOR);
    scale.tare();  // zero the scale with no load - make sure it's empty!
    Serial.println("[HX711] Ready and tared.");
  } else {
    Serial.println("[HX711] WARNING: not detected - check wiring.");
  }

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

  testLoadCell();
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

// ---------- 2. STEPPER ----------
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

// ---------- 3. LCD ----------
// Generic helper other functions reuse
void printToLCD(const String &line1, const String &line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// ---------- 4. BUZZER ----------
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

// ---------- 5. BUTTON (interrupt-driven) ----------
void checkButton() {
  if (buttonFlag) {
    buttonFlag = false; // clear flag
    Serial.println("[Button] Pressed! (interrupt triggered)");
    printToLCD("Button Pressed!", "Interrupt OK");
    tone(BUZZER_PIN, 1500, 100); // quick confirmation beep
    delay(1000);
  }
}

// ---------- 6. IR SENDER/RECEIVER (obstacle / distance) ----------
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

bool readIRDigital() {
  int val = digitalRead(IR_PIN);
  return (val == LOW); // change to (val == HIGH) if your module is active-high
}

// ---------- 7. HX711 + LOAD CELL ----------
void testLoadCell() {
  Serial.print("[HX711] Weight: ");

  if (!scale.is_ready()) {
    Serial.println("not ready / not detected");
    printToLCD("Load Cell Test", "Not detected");
    return;
  }

  float grams = scale.get_units(10); // average of 10 readings
  Serial.print(grams, 1);
  Serial.println(" g");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Weight Test");
  lcd.setCursor(0, 1);
  lcd.print(grams, 1);
  lcd.print(" g");
}

/*
  ------------------------------------------------------------
  HOW TO CALIBRATE THE HX711_CAL_FACTOR ABOVE:
  ------------------------------------------------------------
  1. Upload this sketch once with HX711_CAL_FACTOR set to 1.0 and,
     in setup(), comment out set_scale() (or set it to 1.0).
  2. With the load cell empty, note the raw reading from
     scale.get_units(10) after tare() - it should read near 0.
  3. Place a known weight (e.g. 100g) on the load cell and read
     scale.get_units(10) again (call it "raw_reading").
  4. Calculate: calibration_factor = raw_reading / known_weight_in_grams
  5. Put that value into HX711_CAL_FACTOR above, re-tare with the
     cell empty, and verify known weights read correctly.
  ------------------------------------------------------------
*/
