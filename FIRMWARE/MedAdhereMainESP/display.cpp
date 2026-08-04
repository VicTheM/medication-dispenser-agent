#include "display.h"
#include "config.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

static LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

// Idle-screen state (updated by the state machine, rendered continuously)
static String s_idleLine1 = "MedAdhere";
static String s_idleLine2 = "Starting...";
static int s_scrollOffset = 0;
static unsigned long s_lastScrollMs = 0;
static bool s_inIdleMode = false;

void displayInit() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
}

void displayClear() {
  lcd.clear();
}

void displayMessage(const String &line1, const String &line2) {
  s_inIdleMode = false;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, LCD_COLS));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, LCD_COLS));
}

void displayShowIdle(const String &nextTimeLabel, const String &medNames) {
  s_idleLine1 = nextTimeLabel;
  s_idleLine2 = medNames;
  s_scrollOffset = 0;
  s_lastScrollMs = millis();
  s_inIdleMode = true;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(s_idleLine1.substring(0, LCD_COLS));
  lcd.setCursor(0, 1);
  lcd.print(s_idleLine2.substring(0, LCD_COLS));
}

// Call frequently from loop(); only actually touches the LCD every ~400ms
// and only when line2 is too long to fit, so it's cheap to call often.
void displayIdleTick() {
  if (!s_inIdleMode) return;
  if (s_idleLine2.length() <= LCD_COLS) return;

  unsigned long now = millis();
  if (now - s_lastScrollMs < 400) return;
  s_lastScrollMs = now;

  String padded = s_idleLine2 + "   ";
  int len = padded.length();
  String window = "";
  for (int i = 0; i < LCD_COLS; i++) {
    window += padded[(s_scrollOffset + i) % len];
  }
  lcd.setCursor(0, 1);
  lcd.print(window);
  s_scrollOffset = (s_scrollOffset + 1) % len;
}
