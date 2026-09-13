"""
Hardware emulator for the ESP32-S3 combined video+audio firmware.

This script pretends to BE the device, network-wise: it generates a
synthetic 10s test video and a synthetic 5s test audio tone, then sends
them to backend_server.py using the exact same wire protocol the real
firmware uses. This lets you fully test the backend (video saving,
audio saving, and the audio response round-trip) without any hardware.

It mirrors the firmware's serial-command UX: run it, then type a
command at the prompt.

Commands:
  v  - Generate + send a synthetic video clip (like pressing 'v' on the device)
  a  - Generate + send a synthetic audio clip, wait for + save the response
  b  - Do both, one after another
  h  - Show this menu
  q  - Quit

Install deps:
    pip install numpy

Optional (for nicer output, gracefully skipped if missing):
    pip install pillow        # animated test pattern with frame counter text
    pip install opencv-python # also save a local copy of the sent video as .mp4
    pip install sounddevice   # play the backend's audio response through your speakers

Run:
    python hardware_emulator.py --host 127.0.0.1
"""

import argparse
import socket
import struct
import json
import time
import math
import os
import wave

import numpy as np

try:
    from PIL import Image, ImageDraw

    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

try:
    import cv2

    HAVE_CV2 = True
except ImportError:
    HAVE_CV2 = False

try:
    import sounddevice as sd

    HAVE_SOUNDDEVICE = True
except (ImportError, OSError):
    # ImportError: package not installed.
    # OSError: package installed but the system PortAudio library is missing.
    HAVE_SOUNDDEVICE = False


# =====================================================================
# CONFIG (mirrors the firmware's constants)
# =====================================================================
HOST_AUDIO = "maglev.proxy.rlwy.net"
AUDIO_PORT = 53228

HOST_VIDEO = "tokaido.proxy.rlwy.net"
VIDEO_PORT = 18747

VIDEO_WIDTH = 160
VIDEO_HEIGHT = 120
VIDEO_FPS = 10
VIDEO_DURATION_S = 10
VIDEO_FRAME_COUNT = VIDEO_FPS * VIDEO_DURATION_S

AUDIO_SAMPLE_RATE = 16000
AUDIO_DURATION_S = 5
AUDIO_TONE_HZ = 440.0

LOCAL_OUTPUT_DIR = "emulator_output"


def log(tag, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}][{tag}] {msg}")


# =====================================================================
# NETWORK HELPERS (mirror the firmware's send/recv helpers exactly)
# =====================================================================
def send_uint32_be(sock, value):
    sock.sendall(struct.pack(">I", value))


def recv_exact(sock, n, timeout_s=20.0):
    sock.settimeout(timeout_s)
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_uint32_be(sock, timeout_s=20.0):
    buf = recv_exact(sock, 4, timeout_s)
    if buf is None:
        return None
    return struct.unpack(">I", buf)[0]


# =====================================================================
# SYNTHETIC VIDEO GENERATION
# =====================================================================
def rgb888_to_rgb565_bytes(rgb_array):
    """rgb_array: HxWx3 uint8, returns raw bytes in big-endian RGB565 (matches firmware/backend)."""
    r = (rgb_array[:, :, 0].astype(np.uint16) >> 3) << 11
    g = (rgb_array[:, :, 1].astype(np.uint16) >> 2) << 5
    b = rgb_array[:, :, 2].astype(np.uint16) >> 3
    packed = (r | g | b).astype(">u2")  # big-endian uint16
    return packed.tobytes()


def generate_frame_rgb888(index, total, width, height):
    """A moving box + cycling gradient background + frame counter text, so the
    resulting video is easy to visually verify (not just noise)."""
    if HAVE_PIL:
        hue_deg = (index / max(total, 1)) * 360.0
        img = Image.new("RGB", (width, height))
        draw = ImageDraw.Draw(img)

        # cycling gradient background
        for x in range(width):
            local_hue = (hue_deg + (x / width) * 60) % 360
            r, g, b = _hsv_to_rgb(local_hue, 0.5, 0.9)
            draw.line([(x, 0), (x, height)], fill=(r, g, b))

        # bouncing box
        box_size = 20
        t = index / max(total - 1, 1)
        bounce = abs(((t * 2) % 2) - 1)  # triangle wave 0..1..0
        box_x = int(bounce * (width - box_size))
        box_y = height // 2 - box_size // 2
        draw.rectangle(
            [box_x, box_y, box_x + box_size, box_y + box_size], fill=(255, 255, 255)
        )

        draw.text((4, 4), f"Frame {index}", fill=(0, 0, 0))
        return np.array(img, dtype=np.uint8)
    else:
        # Fallback with no PIL: simple animated gradient, no text.
        arr = np.zeros((height, width, 3), dtype=np.uint8)
        shift = int((index / max(total, 1)) * width)
        for x in range(width):
            v = ((x + shift) % width) / width
            arr[:, x, 0] = int(v * 255)
            arr[:, x, 1] = int((1 - v) * 255)
            arr[:, x, 2] = 128
        return arr


def _hsv_to_rgb(h, s, v):
    h = h / 60.0
    i = int(h) % 6
    f = h - int(h)
    p = v * (1 - s)
    q = v * (1 - f * s)
    t = v * (1 - (1 - f) * s)
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return int(r * 255), int(g * 255), int(b * 255)


def generate_video_clip(width, height, frame_count):
    log(
        "VIDEO",
        f"Generating {frame_count} synthetic frames ({width}x{height})"
        f"{' with PIL patterns' if HAVE_PIL else ' (install pillow for nicer patterns)'}...",
    )
    frames_rgb565 = []
    frames_rgb888_for_local_save = [] if HAVE_CV2 else None

    for i in range(frame_count):
        rgb888 = generate_frame_rgb888(i, frame_count, width, height)
        frames_rgb565.append(rgb888_to_rgb565_bytes(rgb888))
        if frames_rgb888_for_local_save is not None:
            frames_rgb888_for_local_save.append(rgb888)
        if i % 20 == 0:
            log("VIDEO", f"...generated frame {i}/{frame_count}")

    total_bytes = sum(len(f) for f in frames_rgb565)
    log(
        "VIDEO",
        f"Generation complete: {frame_count} frames, {total_bytes} bytes total "
        f"({total_bytes / 1024 / 1024:.2f} MB)",
    )

    if frames_rgb888_for_local_save is not None:
        _save_local_video_copy(frames_rgb888_for_local_save, width, height)

    return frames_rgb565


def _save_local_video_copy(frames_rgb888, width, height):
    os.makedirs(LOCAL_OUTPUT_DIR, exist_ok=True)
    path = os.path.join(LOCAL_OUTPUT_DIR, f"sent_video_{int(time.time())}.mp4")
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(path, fourcc, VIDEO_FPS, (width, height))
    for rgb in frames_rgb888:
        bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        writer.write(bgr)
    writer.release()
    log("VIDEO", f"Local reference copy of the sent clip saved to {path}")


# =====================================================================
# SYNTHETIC AUDIO GENERATION
# =====================================================================
def generate_audio_clip(sample_rate, duration_s, tone_hz):
    log(
        "AUDIO",
        f"Generating {duration_s}s synthetic tone at {tone_hz}Hz, {sample_rate}Hz sample rate...",
    )
    n = int(sample_rate * duration_s)
    t = np.arange(n) / sample_rate

    # simple tone with a fade in/out to avoid clicks, plus a slow frequency
    # wobble so it's obviously a synthetic "test" tone rather than silence
    wobble = 1.0 + 0.02 * np.sin(2 * math.pi * 0.5 * t)
    signal = np.sin(2 * math.pi * tone_hz * t * wobble)

    fade_len = int(0.05 * sample_rate)
    fade_in = np.linspace(0, 1, fade_len)
    fade_out = np.linspace(1, 0, fade_len)
    signal[:fade_len] *= fade_in
    signal[-fade_len:] *= fade_out

    pcm = (signal * 0.8 * 32767).astype("<i2")  # little-endian int16, matches firmware
    pcm_bytes = pcm.tobytes()

    log("AUDIO", f"Generation complete: {n} samples, {len(pcm_bytes)} bytes")

    os.makedirs(LOCAL_OUTPUT_DIR, exist_ok=True)
    local_path = os.path.join(LOCAL_OUTPUT_DIR, f"sent_audio_{int(time.time())}.wav")
    with wave.open(local_path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    log("AUDIO", f"Local reference copy of the sent clip saved to {local_path}")

    return pcm_bytes


# =====================================================================
# EMULATED "VIDEO RECORD + UPLOAD" (mirrors doVideoRecordAndUpload())
# =====================================================================
def emulate_video(host, port, width, height, fps, frame_count):
    log("VIDEO", "=" * 50)
    log("VIDEO", "Starting emulated record+upload sequence")

    t_capture0 = time.time()
    frames = generate_video_clip(width, height, frame_count)
    log("VIDEO", f"'Capture' phase took {time.time() - t_capture0:.2f}s (simulated)")

    max_attempts = 5
    backoff = 0.5

    for attempt in range(1, max_attempts + 1):
        log(
            "VIDEO",
            f"[NET] Attempt {attempt}/{max_attempts}: connecting to {host}:{port}...",
        )
        try:
            sock = socket.create_connection((host, port), timeout=10)
        except OSError as e:
            log("VIDEO", f"[NET] Connect failed: {e}")
            time.sleep(backoff)
            backoff *= 2
            continue

        try:
            header = (
                json.dumps(
                    {"width": width, "height": height, "format": "rgb565", "fps": fps}
                )
                + "\n"
            )
            sock.sendall(header.encode("utf-8"))
            log("VIDEO", f"[NET] Header sent: {header.strip()}")

            t0 = time.time()
            ok = True
            for i, frame in enumerate(frames):
                send_uint32_be(sock, len(frame))
                sock.sendall(frame)
                if i % 20 == 0:
                    log("VIDEO", f"[NET] ...sent frame {i}/{len(frames)}")

            send_uint32_be(sock, 0)  # end-of-stream marker
            log("VIDEO", f"[NET] Upload complete in {time.time() - t0:.2f}s")
            sock.close()
            log("VIDEO", "Sequence done.")
            log("VIDEO", "=" * 50)
            return True
        except OSError as e:
            log("VIDEO", f"[NET] Upload error: {e}, will retry.")
            sock.close()
            time.sleep(backoff)
            backoff *= 2

    log("VIDEO", "[NET] Upload failed after all retries.")
    log("VIDEO", "=" * 50)
    return False


# =====================================================================
# EMULATED "AUDIO RECORD + SEND + PLAY RESPONSE" (mirrors doAudioRecordSendPlay())
# =====================================================================
def emulate_audio(host, port, sample_rate, duration_s, tone_hz):
    log("AUDIO", "=" * 50)
    log("AUDIO", "Starting emulated record+send+respond sequence")

    t_record0 = time.time()
    pcm_bytes = generate_audio_clip(sample_rate, duration_s, tone_hz)
    log("AUDIO", f"'Recording' phase took {time.time() - t_record0:.2f}s (simulated)")

    max_attempts = 3
    backoff = 0.5

    for attempt in range(1, max_attempts + 1):
        log(
            "AUDIO",
            f"[NET] Attempt {attempt}/{max_attempts}: connecting to {host}:{port}...",
        )
        try:
            sock = socket.create_connection((host, port), timeout=10)
        except OSError as e:
            log("AUDIO", f"[NET] Connect failed: {e}")
            time.sleep(backoff)
            backoff *= 2
            continue

        try:
            header = (
                json.dumps(
                    {
                        "sample_rate": sample_rate,
                        "channels": 1,
                        "bits": 16,
                        "samples": len(pcm_bytes) // 2,
                    }
                )
                + "\n"
            )
            sock.sendall(header.encode("utf-8"))
            log("AUDIO", f"[NET] Header sent: {header.strip()}")

            send_uint32_be(sock, len(pcm_bytes))
            t0 = time.time()
            sock.sendall(pcm_bytes)
            log(
                "AUDIO",
                f"[NET] Sent {len(pcm_bytes)} bytes in {time.time() - t0:.2f}s, "
                f"waiting for response...",
            )

            resp_len = recv_uint32_be(sock, timeout_s=20)
            if resp_len is None:
                log("AUDIO", "[NET] No response length received, retrying.")
                sock.close()
                time.sleep(backoff)
                backoff *= 2
                continue

            log("AUDIO", f"[NET] Response length = {resp_len} bytes")

            if resp_len == 0:
                log("AUDIO", "[NET] Backend sent no response audio.")
                sock.close()
                log("AUDIO", "Sequence done.")
                log("AUDIO", "=" * 50)
                return True

            t1 = time.time()
            resp_bytes = recv_exact(sock, resp_len, timeout_s=20)
            sock.close()

            if resp_bytes is None:
                log("AUDIO", "[NET] Response read incomplete, retrying.")
                time.sleep(backoff)
                backoff *= 2
                continue

            log(
                "AUDIO",
                f"[NET] Response received ({len(resp_bytes)} bytes) "
                f"in {time.time() - t1:.2f}s",
            )
            _handle_audio_response(resp_bytes, sample_rate)
            log("AUDIO", "Sequence done.")
            log("AUDIO", "=" * 50)
            return True

        except OSError as e:
            log("AUDIO", f"[NET] Error: {e}, will retry.")
            sock.close()
            time.sleep(backoff)
            backoff *= 2

    log("AUDIO", "[NET] Giving up after all retries.")
    log("AUDIO", "=" * 50)
    return False


def _handle_audio_response(resp_bytes, sample_rate):
    os.makedirs(LOCAL_OUTPUT_DIR, exist_ok=True)
    path = os.path.join(LOCAL_OUTPUT_DIR, f"received_response_{int(time.time())}.wav")
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(resp_bytes)
    log("AUDIO", f"[SPK] (emulated) Response audio saved to {path}")

    if HAVE_SOUNDDEVICE:
        log("AUDIO", "[SPK] (emulated) Playing response through your speakers...")
        pcm = np.frombuffer(resp_bytes, dtype="<i2")
        sd.play(pcm, sample_rate)
        sd.wait()
        log("AUDIO", "[SPK] (emulated) Playback complete.")
    else:
        log(
            "AUDIO",
            "[SPK] (emulated) sounddevice not installed -- "
            "skipping playback, but the file above is ready to open.",
        )


# =====================================================================
# MAIN / SERIAL-STYLE MENU
# =====================================================================
def print_help():
    print("=" * 50)
    print("[MENU] (emulated device)")
    print("  v - Generate + send synthetic video clip")
    print("  a - Generate + send synthetic audio clip, save/play response")
    print("  b - Do both")
    print("  h - Show this menu")
    print("  q - Quit")
    print("=" * 50)


def main():
    parser = argparse.ArgumentParser(
        description="Emulate the ESP32 firmware's network behavior for backend testing."
    )
    parser.add_argument(
        "--host", default=HOST_VIDEO, help=f"Backend host (default {HOST_VIDEO})"
    )
    parser.add_argument("--video-port", type=int, default=VIDEO_PORT)
    parser.add_argument("--audio-port", type=int, default=AUDIO_PORT)
    parser.add_argument("--width", type=int, default=VIDEO_WIDTH)
    parser.add_argument("--height", type=int, default=VIDEO_HEIGHT)
    parser.add_argument("--fps", type=int, default=VIDEO_FPS)
    parser.add_argument("--video-duration", type=int, default=VIDEO_DURATION_S)
    parser.add_argument("--sample-rate", type=int, default=AUDIO_SAMPLE_RATE)
    parser.add_argument("--audio-duration", type=int, default=AUDIO_DURATION_S)
    parser.add_argument(
        "--once",
        choices=["v", "a", "b"],
        default=None,
        help="Run one command non-interactively then exit (for scripting/CI)",
    )
    args = parser.parse_args()

    frame_count = args.fps * args.video_duration

    log("BOOT", "Hardware emulator starting")
    log(
        "BOOT",
        f"Video target: {args.host}:{args.video_port}  "
        f"({args.width}x{args.height} @ {args.fps}fps, {args.video_duration}s = {frame_count} frames)",
    )
    log(
        "BOOT",
        f"Audio target: {args.host}:{args.audio_port}  "
        f"({args.sample_rate}Hz, {args.audio_duration}s)",
    )
    log(
        "BOOT",
        f"Optional deps -- Pillow: {HAVE_PIL}, OpenCV: {HAVE_CV2}, sounddevice: {HAVE_SOUNDDEVICE}",
    )

    def run_video():
        emulate_video(
            HOST_VIDEO, args.video_port, args.width, args.height, args.fps, frame_count
        )

    def run_audio():
        emulate_audio(
            HOST_AUDIO,
            args.audio_port,
            args.sample_rate,
            args.audio_duration,
            AUDIO_TONE_HZ,
        )

    if args.once:
        if args.once == "v":
            run_video()
        elif args.once == "a":
            run_audio()
        elif args.once == "b":
            run_video()
            run_audio()
        return

    print_help()
    while True:
        try:
            cmd = input("> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if cmd == "v":
            run_video()
        elif cmd == "a":
            run_audio()
        elif cmd == "b":
            run_video()
            run_audio()
        elif cmd == "h":
            print_help()
        elif cmd == "q":
            log("BOOT", "Exiting.")
            break
        elif cmd == "":
            continue
        else:
            print(f"[CMD] Unknown command '{cmd}'. Type 'h' for help.")


if __name__ == "__main__":
    main()
