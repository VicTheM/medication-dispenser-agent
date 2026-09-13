#pragma once
#include <Arduino.h>

// Starts a WPA2-protected SoftAP + local HTTP config form (WiFi creds,
// API base URL, device_uid/device_secret). See DEVICE_BRIEF.md "Config
// portal security model" for why this is WPA2-on-the-AP rather than
// TLS-on-the-form-server.
void webportalStart();
void webportalLoop();  // call every loop() iteration while active; handles
                        // requests and the idle auto-timeout
void webportalStop();
bool webportalIsActive();
bool webportalSaveCompleted();
