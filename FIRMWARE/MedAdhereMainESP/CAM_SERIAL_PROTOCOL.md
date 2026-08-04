# CAM Board Serial2 Protocol

Wiring: MainESP GPIO17(RX)←CAM TX, GPIO18(TX)→CAM RX, common GND. Baud: **921600** (not 115200 — see "Bandwidth" below). `SERIAL_8N1`.

## Frame format (both directions)
| bytes | field |
|---|---|
| 0-1 | sync `0xAA 0x55` |
| 2 | TYPE (ASCII letter) |
| 3-6 | LEN, uint32 little-endian (payload byte count) |
| 7..7+LEN-1 | PAYLOAD |
| last byte | CHECKSUM = XOR of TYPE byte + all LEN/payload bytes |

## Commands MainESP → CAM (drive the CAM's state machine)
- `'V'` payload=uint32 duration_ms → enter **Video** state, record an adherence clip for that long, then send it back as a `'D'` frame with sub-type `'V'`.
- `'L'` payload=uint32 max_duration_ms → enter **Listen** state, record mic audio until silence or the max duration, send back as `'D'` with sub-type `'A'`.
- `'S'` no payload → **Stop**, return to idle.
- `'P'` payload=raw audio bytes → **Play** those bytes through the speaker, then return to idle.

## Frames CAM → MainESP
- `'D'` payload[0] = sub-type (`'V'` or `'A'`), payload[1..] = the actual file bytes (video or audio). Send the header+sub-type first, then stream the file bytes — MainESP starts consuming them immediately, no need to buffer the whole file on the CAM side first.
- `'A'` short status text, e.g. `"READY"`.
- `'E'` short error text.

## Bandwidth (important)
At 921600 baud, realistic throughput is roughly **80-90 KB/s**. A 2-minute adherence "video" at typical low-res settings can easily be several MB, which would take a minute or more just to cross the wire. **Recommendation: don't send continuous full-motion video.** Instead have the CAM board capture a low-fps JPEG sequence (e.g. one 320×240 frame every 2-3s for 2 minutes ≈ 24-40 frames, ~15-30KB each ≈ under 1MB total) and either concatenate them into a simple MJPEG/AVI container or send them as a small zip-like blob — either way, it's still just "N bytes with a length" as far as this protocol and the MainESP relay code are concerned, so this is entirely a CAM-side decision. Keep the same `'D'`/`'V'` framing regardless of what's inside.

## Reliability
MainESP validates the trailing checksum but treats a mismatch as a warning, not a failure (a corrupted-but-mostly-intact adherence clip is still more useful than none). Keep cable runs short and add a common ground if you see corruption in practice.
