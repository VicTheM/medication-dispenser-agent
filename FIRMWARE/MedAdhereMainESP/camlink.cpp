#include "camlink.h"
#include "config.h"

void camlinkInit() {
  Serial2.begin(CAM_SERIAL_BAUD, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
  Serial2.setTimeout(3000);
}

static uint8_t xorAll(const uint8_t *data, size_t len, uint8_t seed = 0) {
  uint8_t c = seed;
  for (size_t i = 0; i < len; i++) c ^= data[i];
  return c;
}

void camSendControl(char type, uint32_t param) {
  uint8_t header[7];
  header[0] = 0xAA;
  header[1] = 0x55;
  header[2] = (uint8_t)type;
  header[3] = (uint8_t)(param & 0xFF);
  header[4] = (uint8_t)((param >> 8) & 0xFF);
  header[5] = (uint8_t)((param >> 16) & 0xFF);
  header[6] = (uint8_t)((param >> 24) & 0xFF);

  uint8_t checksum = xorAll(&header[2], 5); // TYPE + 4 length/param bytes
  Serial2.write(header, sizeof(header));
  Serial2.write(checksum);
  Serial2.flush();
}

void camSendPlaybackAudio(const uint8_t *data, uint32_t len) {
  uint8_t header[7];
  header[0] = 0xAA;
  header[1] = 0x55;
  header[2] = (uint8_t)'P';
  header[3] = (uint8_t)(len & 0xFF);
  header[4] = (uint8_t)((len >> 8) & 0xFF);
  header[5] = (uint8_t)((len >> 16) & 0xFF);
  header[6] = (uint8_t)((len >> 24) & 0xFF);

  uint8_t checksum = xorAll(&header[2], 5);
  checksum = xorAll(data, len, checksum);

  Serial2.write(header, sizeof(header));
  Serial2.write(data, len);
  Serial2.write(checksum);
  Serial2.flush();
}

bool camWaitForFrameHeader(char &type, char &subType, uint32_t &payloadLen, unsigned long timeoutMs) {
  unsigned long start = millis();
  int state = 0; // 0=wait sync1, 1=wait sync2, 2..=collect header bytes
  uint8_t headerBuf[7];
  int headerIdx = 0;

  while (millis() - start < timeoutMs) {
    if (!Serial2.available()) {
      delay(1); // yields to the scheduler so WiFi/WS keep servicing in the background
      continue;
    }
    uint8_t b = (uint8_t)Serial2.read();

    if (state == 0) {
      if (b == 0xAA) state = 1;
      continue;
    }
    if (state == 1) {
      if (b == 0x55) { state = 2; headerIdx = 0; }
      else state = 0;
      continue;
    }
    // state 2: collecting TYPE + 4 length bytes = 5 bytes
    headerBuf[headerIdx++] = b;
    if (headerIdx == 5) {
      type = (char)headerBuf[0];
      payloadLen = (uint32_t)headerBuf[1] | ((uint32_t)headerBuf[2] << 8) |
                   ((uint32_t)headerBuf[3] << 16) | ((uint32_t)headerBuf[4] << 24);

      if (type == 'D') {
        // sub-type is the first payload byte - read it now (bounded wait)
        unsigned long subStart = millis();
        while (!Serial2.available()) {
          if (millis() - subStart > 2000) return false;
          delay(1);
        }
        subType = (char)Serial2.read();
        payloadLen -= 1; // caller will read the remaining (payloadLen) bytes as file content
      } else {
        subType = 0;
      }
      return true;
    }
  }
  return false; // timed out
}

void camConsumeTrailingChecksum() {
  unsigned long start = millis();
  while (!Serial2.available()) {
    if (millis() - start > 2000) {
      Serial.println("[camlink] WARNING: timed out waiting for trailing checksum byte");
      return;
    }
    delay(1);
  }
  Serial2.read(); // best-effort only - see header comment
}
