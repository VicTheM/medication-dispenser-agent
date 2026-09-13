/*
  ESP32-CAM (AI Thinker) + MAX4466 mic + MAX98357A amp + 4-ohm speaker
  ----------------------------------------------------------------------
  Sequence at boot:
    1. Play a pre-saved audio clip from LittleFS ("/startup.pcm")
    2. Record 5 seconds of audio from the MAX4466 mic
    3. Short delay
    4. Play back the recorded audio through the MAX98357A speaker

  HARDWARE NOTES
  ----------------------------------------------------------------------
  This sketch never initializes the onboard camera, so all camera-reserved
  GPIOs are free. Pins were chosen to avoid boot-strapping pins (0, 2, 12)
  and UART pins (1, 3).

    MAX4466 (mic)        OUT  -> GPIO33 (ADC1_CH5)
                          VCC  -> 3.3V
                          GND  -> GND

    MAX98357A (amp)       BCLK -> GPIO14
                          LRC  -> GPIO15
                          DIN  -> GPIO13
                          SD   -> 3.3V (or leave floating to enable)
                          VIN  -> 5V
                          GND  -> GND
                          Speaker terminals -> 4-ohm speaker

  REQUIREMENTS
  ----------------------------------------------------------------------
  - Board package: arduino-esp32 core 2.0.x (legacy driver/i2s.h API).
    Core 3.x replaced the I2S driver and this sketch will NOT compile there.
  - PSRAM enabled (Tools > PSRAM: Enabled). The AI-Thinker module has PSRAM
    onboard; this is required to hold the 5-second recording buffer.
  - A file named "startup.pcm" uploaded to LittleFS (see notes at bottom
    of this file for how to create/upload it).

  AUDIO FORMAT USED THROUGHOUT
  ----------------------------------------------------------------------
  16 kHz, mono, signed 16-bit PCM, no header (raw). Both the startup clip
  and the recorded clip use this format so the same playback code can
  handle either.
*/

#include <Arduino.h>
#include "FS.h"
#include "LittleFS.h"
#include "driver/i2s.h"

// ---------------- Pin configuration ----------------
#define I2S_SPK_PORT      I2S_NUM_1
#define I2S_SPK_BCLK      14   // BCLK / SCK
#define I2S_SPK_LRC       15   // WS / LRCLK
#define I2S_SPK_DOUT      13   // DIN on MAX98357A

#define MIC_PIN           12   // ADC1_CH5, safe / unused by camera

// ---------------- Audio configuration ----------------
#define SAMPLE_RATE       16000
#define RECORD_SECONDS    5
#define RECORD_SAMPLES    (SAMPLE_RATE * RECORD_SECONDS)
#define STARTUP_FILE      "/startup.pcm"
#define PLAYBACK_CHUNK     512   // mono samples per I2S write chunk

int16_t *recordBuffer = nullptr;

// =====================================================================
//  I2S SPEAKER OUTPUT (MAX98357A)
// =====================================================================
void setupI2SSpeaker() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[SPK] ERROR: i2s_driver_install failed (%d)\n", err);
    return;
  }

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_set_pin(I2S_SPK_PORT, &pins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

// Writes a block of mono int16 samples to the amp, duplicating each
// sample to left+right (MAX98357A expects a stereo I2S stream).
void i2sWriteMonoBlock(const int16_t *mono, size_t count) {
  static int16_t stereoBuf[PLAYBACK_CHUNK * 2];
  size_t pos = 0;
  while (pos < count) {
    size_t n = min((size_t)PLAYBACK_CHUNK, count - pos);
    for (size_t i = 0; i < n; i++) {
      stereoBuf[2 * i]     = mono[pos + i];
      stereoBuf[2 * i + 1] = mono[pos + i];
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_SPK_PORT, stereoBuf, n * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    pos += n;
  }
}

// =====================================================================
//  STEP 1: PLAY PRE-SAVED STARTUP CLIP FROM LITTLEFS
// =====================================================================
bool playPCMFile(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[PCM] ERROR: could not open '%s' - skipping startup audio\n", path);
    return false;
  }

  size_t fileBytes = f.size();
  Serial.printf("[PCM] Playing '%s' (%u bytes, ~%.2f s)\n",
                path, (unsigned)fileBytes, fileBytes / 2.0f / SAMPLE_RATE);

  int16_t monoBuf[PLAYBACK_CHUNK];
  size_t totalSamples = 0;

  while (f.available()) {
    size_t bytesRead = f.read((uint8_t *)monoBuf, sizeof(monoBuf));
    size_t samples = bytesRead / sizeof(int16_t);
    if (samples == 0) break;
    i2sWriteMonoBlock(monoBuf, samples);
    totalSamples += samples;
  }
  f.close();

  Serial.printf("[PCM] Startup playback complete (%u samples)\n", (unsigned)totalSamples);
  return true;
}

// =====================================================================
//  STEP 2: RECORD 5 SECONDS FROM THE MAX4466 MIC
// =====================================================================
bool recordFromMic(int16_t *buffer, size_t numSamples, uint32_t sampleRate) {
  Serial.printf("[MIC] Recording %u samples @ %lu Hz (%.1f s)...\n",
                (unsigned)numSamples, (unsigned long)sampleRate,
                numSamples / (float)sampleRate);

  const uint32_t periodUs = 1000000UL / sampleRate;
  uint32_t nextSampleTime = micros();

  for (size_t i = 0; i < numSamples; i++) {
    // Wait until the next sample slot (precise pacing at 16 kHz = 62.5us)
    while ((int32_t)(micros() - nextSampleTime) < 0) {
      // tight spin - short enough not to need a yield here
    }
    nextSampleTime += periodUs;

    int raw = analogRead(MIC_PIN);              // 0-4095 (12-bit)
    buffer[i] = (int16_t)((raw - 2048) * 16);    // center + scale to int16 range

    // Feed the watchdog periodically without disturbing timing much
    if ((i & 0x3FF) == 0) {
      yield();
    }
  }

  Serial.println("[MIC] Recording complete.");
  return true;
}

// =====================================================================
//  STEP 4: PLAY BACK THE RECORDED BUFFER
// =====================================================================
bool playRecordedAudio(const int16_t *buffer, size_t numSamples) {
  Serial.printf("[SPK] Playing back %u samples (%.2f s)...\n",
                (unsigned)numSamples, numSamples / (float)SAMPLE_RATE);
  i2sWriteMonoBlock(buffer, numSamples);
  Serial.println("[SPK] Playback complete.");
  return true;
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("==================================================");
  Serial.println("[BOOT] ESP32-CAM mic/speaker demo starting");
  Serial.println("==================================================");

  if (!psramFound()) {
    Serial.println("[BOOT] WARNING: PSRAM not detected - recording buffer allocation may fail.");
  } else {
    Serial.printf("[BOOT] PSRAM OK - %u bytes free\n", (unsigned)ESP.getFreePsram());
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[BOOT] ERROR: LittleFS mount failed.");
  } else {
    Serial.println("[BOOT] LittleFS mounted.");
  }

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  Serial.printf("[BOOT] Mic input ready on GPIO%d\n", MIC_PIN);

  setupI2SSpeaker();
  Serial.printf("[BOOT] I2S speaker ready (BCLK=%d LRC=%d DIN=%d)\n",
                I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DOUT);

  // ---- STEP 1: startup clip ----
  Serial.println("--------------------------------------------------");
  Serial.println("[STEP 1] Playing startup audio clip");
  playPCMFile(STARTUP_FILE);

  // Allocate the recording buffer in PSRAM (5s @ 16kHz, 16-bit = ~160KB)
  recordBuffer = (int16_t *)ps_malloc(RECORD_SAMPLES * sizeof(int16_t));
  if (!recordBuffer) {
    Serial.println("[BOOT] ERROR: failed to allocate recording buffer in PSRAM. Halting.");
    while (true) delay(1000);
  }

  // ---- STEP 2: record ----
  Serial.println("--------------------------------------------------");
  Serial.println("[STEP 2] Recording from microphone");
  recordFromMic(recordBuffer, RECORD_SAMPLES, SAMPLE_RATE);

  // ---- STEP 3: delay ----
  Serial.println("--------------------------------------------------");
  Serial.println("[STEP 3] Pausing before playback...");
  delay(800);

  // ---- STEP 4: play back recording ----
  Serial.println("--------------------------------------------------");
  Serial.println("[STEP 4] Playing back recorded audio");
  playRecordedAudio(recordBuffer, RECORD_SAMPLES);

  Serial.println("==================================================");
  Serial.println("[DONE] Sequence complete. Send 'r' over Serial to repeat.");
  Serial.println("==================================================");
}

// =====================================================================
//  LOOP - idle, but lets you re-trigger record/playback over Serial
// =====================================================================
void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 'R') {
      Serial.println("[CMD] Repeating record/playback cycle...");
      recordFromMic(recordBuffer, RECORD_SAMPLES, SAMPLE_RATE);
      delay(800);
      playRecordedAudio(recordBuffer, RECORD_SAMPLES);
    }
  }
  delay(1000);
  Serial.println("--------------------------------------------------");
  Serial.println("[STEP 1] Playing startup audio clip");
  playPCMFile(STARTUP_FILE);
}

/*
  PREPARING THE STARTUP CLIP ("/startup.pcm")
  ----------------------------------------------------------------------
  The clip must be raw 16kHz mono signed 16-bit PCM with no file header.

  1. Convert any audio file with ffmpeg:
       ffmpeg -i input.mp3 -ar 16000 -ac 1 -f s16le startup.pcm

  2. Put startup.pcm inside a "data" folder next to this .ino file.

  3. Upload it to the ESP32's LittleFS partition:
       - Arduino IDE: install the "Arduino ESP32 LittleFS Data Upload"
         plugin/tool, then use Tools > "ESP32 LittleFS Data Upload".
       - PlatformIO: run `pio run --target uploadfs`.

  4. Make sure your partition scheme leaves room for a filesystem
     (e.g. "Default 4MB with spiffs" in the Arduino IDE board menu).

  If the file is missing or fails to open, playPCMFile() logs an error
  and the rest of the sequence (record/playback) still runs normally.
*/
