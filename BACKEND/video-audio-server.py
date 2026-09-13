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
import base64
import io

import numpy as np
import cv2
import tempfile
import httpx

import s3fs

s3 = s3fs.S3FileSystem(
    key='50929696d185dd613c0d3a94daeeac7a',
    secret='00c3524b928e0270fbd3fea11b30285a5f1c3a9162f06966258209fba9c4d451',
    endpoint_url='https://dc63d7e1f34a4437a67e242e912fda34.r2.cloudflarestorage.com',
)

VIDEO_HOST = "0.0.0.0"
VIDEO_PORT = 5001
AUDIO_HOST = "0.0.0.0"
AUDIO_PORT = 5002
OUTPUT_DIR = "medadhere/recordings"
AI_VOICE_URL = "https://ally-project-c2h6bgdgf3hngzdv.spaincentral-01.azurewebsites.net/voice/ask"


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

    # os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_path = os.path.join(OUTPUT_DIR, f"video_{int(time.time())}.mp4")
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")

    with tempfile.NamedTemporaryFile(suffix='.mp4') as temp_vid:
        temp_path = temp_vid.file.name
        writer = cv2.VideoWriter(temp_path, fourcc, fps, (width, height))
        log("VIDEO", f"Writing in-memory to {temp_path}")

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

        temp_vid.seek(0)
        with s3.open(out_path, "wb") as f:
            log("VIDEO", f"Writing to bucket at {out_path}")
            f.write(temp_vid.read())

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


def resample_to_pcm_16k_mono(pcm_bytes, sample_rate, channels, bits):
    if bits not in (8, 16, 24, 32):
        raise ValueError(f"Unsupported PCM bit depth: {bits}")

    sample_width = bits // 8
    if len(pcm_bytes) % (sample_width * channels) != 0:
        raise ValueError("PCM data is not aligned to complete samples")

    if bits == 8:
        samples = np.frombuffer(pcm_bytes, dtype=np.uint8).astype(np.float32)
        samples = (samples - 128.0) / 128.0
    elif bits == 16:
        samples = np.frombuffer(pcm_bytes, dtype="<i2").astype(np.float32) / 32768.0
    elif bits == 24:
        raw = np.frombuffer(pcm_bytes, dtype=np.uint8).reshape(-1, 3)
        samples = (
            raw[:, 0].astype(np.int32)
            | (raw[:, 1].astype(np.int32) << 8)
            | (raw[:, 2].astype(np.int32) << 16)
        )
        samples = ((samples ^ 0x800000) - 0x800000).astype(np.float32) / 8388608.0
    else:
        samples = np.frombuffer(pcm_bytes, dtype="<i4").astype(np.float32) / 2147483648.0

    samples = samples.reshape(-1, channels)
    if channels > 1:
        samples = samples.mean(axis=1)
    else:
        samples = samples[:, 0]

    if sample_rate != 16000:
        target_length = max(1, round(len(samples) * 16000 / sample_rate))
        source_positions = np.arange(len(samples), dtype=np.float32)
        target_positions = np.linspace(0, len(samples) - 1, target_length)
        samples = np.interp(target_positions, source_positions, samples)

    return np.clip(np.round(samples * 32767.0), -32768, 32767).astype("<i2").tobytes()


def generate_response(pcm_bytes, sample_rate, channels, bits):
    """
    Send the recording as a WAV file and return the response as raw PCM.
    Fall back to the original recording if the AI request or response
    decoding fails.
    """
    try:
        request_wav = io.BytesIO()
        save_wav(request_wav, pcm_bytes, sample_rate, channels, bits)
        request_wav.seek(0)

        filename = f"recording_{int(time.time())}.wav"
        with httpx.Client(timeout=180.0) as client:
            response = client.post(
                AI_VOICE_URL,
                files={
                    "audio": (filename, request_wav, "audio/wav"),
                },
                data={"audio_format": "wav"},
                headers={"accept": "application/json"},
            )
            response.raise_for_status()

        response_data = response.json()
        print("No exception yet")
        encoded_audio = response_data["audio_base64"]
        if encoded_audio.startswith("data:"):
            encoded_audio = encoded_audio.split(",", 1)[1]
        encoded_audio = "".join(encoded_audio.split())
        response_wav = base64.b64decode(encoded_audio, validate=True)

        with wave.open(io.BytesIO(response_wav), "rb") as wav_file:
            response_sample_rate = wav_file.getframerate()
            response_channels = wav_file.getnchannels()
            response_bits = wav_file.getsampwidth() * 8
            response_pcm = wav_file.readframes(wav_file.getnframes())

        if not response_pcm:
            raise ValueError("AI response contains no audio frames")

        response_pcm = resample_to_pcm_16k_mono(
            response_pcm,
            response_sample_rate,
            response_channels,
            response_bits,
        )

        log("AUDIO", f"Received {len(response_pcm)} bytes of AI response audio")
        return response_pcm
    except Exception as e:
        log("AUDIO", f"AI request failed, using original audio: {e}")
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

    # os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_path = os.path.join(OUTPUT_DIR, f"recording_{int(time.time())}.wav")

    with s3.open(out_path, "wb") as f:
        save_wav(f, pcm_bytes, sample_rate, channels, bits)
    log("AUDIO", f"Saved recording to {out_path}")

    response_pcm = generate_response(pcm_bytes, sample_rate, channels, bits)

    response_path = os.path.join(OUTPUT_DIR, f"response_{int(time.time())}.wav")
    with s3.open(response_path, "wb") as f:
        save_wav(f, response_pcm, 16000, 1, 16)
    log("AUDIO", f"Saved response audio to {response_path}")

    if response_pcm:
        response_chunk = response_pcm[:600000]
        conn.sendall(struct.pack(">I", len(response_chunk)))
        conn.sendall(response_chunk)
        log("AUDIO", f"Sent {len(response_chunk)} of {len(response_pcm)} bytes of response audio")
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
