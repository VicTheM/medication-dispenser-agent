#pragma once
#include <Arduino.h>

// ---- Ultrasonic (approach detection) ----
void ultrasonicInit();
float ultrasonicReadCM(); // -1 if no echo / out of range

// ---- IR beam-break ("laser") at the picking-tray door ----
void beamInit();
bool beamObstacleDetected(); // true = beam blocked (door closed / obstacle present)

// ---- Carousel stepper (28BYJ-48 + ULN2003) ----
void carouselInit();
void carouselTick();                 // call every loop() iteration - non-blocking
bool carouselIsMoving();
uint8_t carouselCurrentIndex();      // 0-6, corresponds to compartment A-G
void carouselGoTo(uint8_t targetIndex); // starts a non-blocking move
void carouselCalibrateHere();        // marks current physical position as compartment A (index 0)
