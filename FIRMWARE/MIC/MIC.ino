const int micPin = 36; // Analog GPIO pin connected to MAX4466 OUT

void setup() {
  Serial.begin(115200);
  // Configure ADC attenuation if needed for full range
  analogSetAttenuation(ADC_11db); 
}

void loop() {
  int micValue = analogRead(micPin);
  Serial.println(micValue);
  delay(10);
}
