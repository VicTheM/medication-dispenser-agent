#pragma once
/*
  Framed binary protocol over Serial2 between this board (ESP32-S3, the
  "main" controller) and the ESP32-CAM (Ai Thinker) board, which only
  handles mic/speaker/camera and runs its own letter-driven state machine.

  Full spec: CAM_SERIAL_PROTOCOL.md. Summary:

    byte 0-1 : sync bytes 0xAA 0x55
    byte 2   : TYPE (single ASCII letter)
    byte 3-6 : LEN, uint32 little-endian - length of PAYLOAD that follows
    byte 7.. : PAYLOAD (LEN bytes, may be zero)
    last byte: CHECKSUM = XOR of every byte from TYPE through the end of PAYLOAD

  Commands this board SENDS to the CAM board (drives its state machine):
    'V' - enter Video-capture state.   payload: uint32 duration_ms
    'L' - enter Listen (voice) state.  payload: uint32 max_duration_ms
    'S' - Stop / return to idle.       payload: none
    'P' - Play audio through speaker.  payload: raw audio bytes to play

  Frames this board RECEIVES from the CAM board:
    'D' - Data ready. payload[0] is a sub-type ('V' or 'A' for
          video/adherence-capture vs audio-question), payload[1..] is the
          actual file bytes.
    'A' - short ACK/status text in payload (e.g. "READY").
    'E' - error text in payload.
*/
#include <Arduino.h>

void camlinkInit();

// Send a small control command (fire-and-forget, blocks briefly - frame is
// only 9 bytes plus checksum, negligible even at low baud rates).
void camSendControl(char type, uint32_t param = 0);

// Send raw bytes to be played through the CAM board's speaker (type 'P').
// Blocks until fully written to the UART - fine for typical spoken-answer
// audio sizes (tens to low hundreds of KB). See DEVICE_BRIEF.md for why this
// is an accepted simplification rather than a fully async transfer.
void camSendPlaybackAudio(const uint8_t *data, uint32_t len);

// Waits (bounded by timeoutMs, yielding regularly so WiFi/WS keep working)
// for the next frame's header to arrive. On success, leaves the raw payload
// bytes still sitting in the Serial2 RX buffer/stream for the caller to
// consume directly (e.g. by streaming into an HTTP upload) - only the
// 7-byte header + first payload byte (the sub-type, for 'D' frames) are
// consumed by this call.
// Returns false on timeout or malformed header.
bool camWaitForFrameHeader(char &type, char &subType, uint32_t &payloadLen, unsigned long timeoutMs);

// After the caller has read exactly (payloadLen - 1) further bytes from
// Serial2 (i.e. the actual file content, since the sub-type byte was
// already consumed by camWaitForFrameHeader), call this to read and check
// the trailing checksum byte. Mismatches are logged but not treated as
// fatal - the file itself is still usable.
void camConsumeTrailingChecksum();
