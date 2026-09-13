"""
Combined backend for the ESP32-CAM combined video+audio firmware.

Runs two TCP servers in parallel threads:
  - Video server (default port 5001): receives raw RGB565 frames,
    writes an MP4.
  - Audio server (default port 5002): receives a raw PCM recording,
    saves a WAV, and sends back a response clip (placeholder: echoes
    the recording -- replace generate_response() with real logic).

Install deps:
    pip install opencv-python numpy

Run:
    python backend_server.py
"""

import socket
import threading
import json
import struct
import time
import os
import wave

import numpy as np
import cv2

VIDEO_HOST = "0.0.0.0"
VIDEO_PORT = 5001
AUDIO_HOST = "0.0.0.0"
AUDIO_PORT = 5002
OUTPUT_DIR = "recordings"


def log(tag, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}][{tag}] {msg}")


# ---------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------
def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_line(conn):
    buf = b""
    while True:
        b = conn.recv(1)
        if not b:
            return None
        if b == b"\n":
            return buf
        buf += b


# ---------------------------------------------------------------------
# VIDEO SERVER
# ---------------------------------------------------------------------
def rgb565_to_bgr(frame_bytes, width, height):
    raw = np.frombuffer(frame_bytes, dtype=">u2").reshape((height, width))
    r = ((raw >> 11) & 0x1F).astype(np.uint8) << 3
    g = ((raw >> 5) & 0x3F).astype(np.uint8) << 2
    b = (raw & 0x1F).astype(np.uint8) << 3
    return np.dstack((b, g, r))


def handle_video_connection(conn, addr):
    log("VIDEO", f"Connection from {addr}")

    header_line = recv_line(conn)
    if not header_line:
        log("VIDEO", "No header received, closing.")
        return

    try:
        meta = json.loads(header_line.decode("utf-8"))
    except Exception as e:
        log("VIDEO", f"Bad header: {e}")
        return

    width = meta["width"]
    height = meta["height"]
    fps = meta.get("fps", 10)
    log("VIDEO", f"Header OK: {width}x{height} @ {fps}fps, format={meta.get('format')}")

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_path = os.path.join(OUTPUT_DIR, f"video_{int(time.time())}.mp4")
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(out_path, fourcc, fps, (width, height))
    log("VIDEO", f"Writing to {out_path}")

    frame_count = 0
    frame_byte_size = width * height * 2
    t0 = time.time()

    while True:
        len_bytes = recv_exact(conn, 4)
        if len_bytes is None:
            log("VIDEO", "Connection closed unexpectedly.")
            break

        (frame_len,) = struct.unpack(">I", len_bytes)
        if frame_len == 0:
            log("VIDEO", "End-of-stream marker received.")
            break

        frame_bytes = recv_exact(conn, frame_len)
        if frame_bytes is None:
            log("VIDEO", "Connection closed mid-frame.")
            break

        if frame_len != frame_byte_size:
            log("VIDEO", f"WARNING: unexpected frame size {frame_len} (expected {frame_byte_size}), skipping.")
            continue

        bgr = rgb565_to_bgr(frame_bytes, width, height)
        writer.write(bgr)
        frame_count += 1
        if frame_count % 20 == 0:
            log("VIDEO", f"...received frame {frame_count}")

    writer.release()
    elapsed = time.time() - t0
    log("VIDEO", f"Saved {frame_count} frames to {out_path} in {elapsed:.2f}s")


def video_server():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((VIDEO_HOST, VIDEO_PORT))
        s.listen(1)
        log("VIDEO", f"Listening on {VIDEO_HOST}:{VIDEO_PORT}")
        while True:
            conn, addr = s.accept()
            with conn:
                try:
                    handle_video_connection(conn, addr)
                except Exception as e:
                    log("VIDEO", f"ERROR handling connection: {e}")


# ---------------------------------------------------------------------
# AUDIO SERVER
# ---------------------------------------------------------------------
def save_wav(path, pcm_bytes, sample_rate, channels, bits):
    with wave.open(path, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(bits // 8)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)


def generate_response(pcm_bytes, sample_rate, channels, bits):
    """
    Placeholder: echoes the recording back. Replace with real logic
    (TTS, an LLM voice pipeline, etc). Must return raw 16kHz mono
    16-bit PCM bytes.
    """
    log("AUDIO", "generate_response(): using placeholder echo logic")
    return pcm_bytes


def handle_audio_connection(conn, addr):
    log("AUDIO", f"Connection from {addr}")

    header_line = recv_line(conn)
    if not header_line:
        log("AUDIO", "No header received, closing.")
        return

    try:
        meta = json.loads(header_line.decode("utf-8"))
    except Exception as e:
        log("AUDIO", f"Bad header: {e}")
        return

    sample_rate = meta["sample_rate"]
    channels = meta.get("channels", 1)
    bits = meta.get("bits", 16)
    log("AUDIO", f"Header OK: {sample_rate}Hz {channels}ch {bits}bit, samples={meta.get('samples')}")

    len_bytes = recv_exact(conn, 4)
    if len_bytes is None:
        log("AUDIO", "Connection closed before length header.")
        return
    (pcm_len,) = struct.unpack(">I", len_bytes)
    log("AUDIO", f"Expecting {pcm_len} bytes of PCM...")

    t0 = time.time()
    pcm_bytes = recv_exact(conn, pcm_len)
    if pcm_bytes is None:
        log("AUDIO", "Connection closed mid-audio.")
        return
    log("AUDIO", f"Received {pcm_len} bytes in {time.time() - t0:.2f}s")

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_path = os.path.join(OUTPUT_DIR, f"recording_{int(time.time())}.wav")
    save_wav(out_path, pcm_bytes, sample_rate, channels, bits)
    log("AUDIO", f"Saved recording to {out_path}")

    response_pcm = generate_response(pcm_bytes, sample_rate, channels, bits)

    if response_pcm:
        conn.sendall(struct.pack(">I", len(response_pcm)))
        conn.sendall(response_pcm)
        log("AUDIO", f"Sent {len(response_pcm)} bytes of response audio")
    else:
        conn.sendall(struct.pack(">I", 0))
        log("AUDIO", "Sent empty response (no audio)")


def audio_server():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((AUDIO_HOST, AUDIO_PORT))
        s.listen(1)
        log("AUDIO", f"Listening on {AUDIO_HOST}:{AUDIO_PORT}")
        while True:
            conn, addr = s.accept()
            with conn:
                try:
                    handle_audio_connection(conn, addr)
                except Exception as e:
                    log("AUDIO", f"ERROR handling connection: {e}")


# ---------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------
def main():
    log("MAIN", "Starting combined video+audio backend")
    t1 = threading.Thread(target=video_server, daemon=True)
    t2 = threading.Thread(target=audio_server, daemon=True)
    t1.start()
    t2.start()
    log("MAIN", "Both servers started. Press Ctrl+C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        log("MAIN", "Shutting down.")


if __name__ == "__main__":
    main()
