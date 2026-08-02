#!/usr/bin/env python3
"""
MedAdhere mock hardware device.

Simulates a real 7-compartment dispenser talking to the MedAdhere backend:
opens the persistent WebSocket, receives/stores its schedule, dispenses
autonomously when a scheduled time hits (or on command), uploads fake
adherence video + telemetry over HTTPS, and can send a recorded "voice
question" through the AI pipeline — all while you drive it live from the
terminal so you can test the rest of the app without real hardware.

SETUP
-----
    pip install httpx websockets
    python mock_device.py --device-uid DEV-001 --device-secret <secret>

Get device_uid/device_secret from the caregiver dashboard (or the API
directly): POST /caregivers/patients/{id}/device returns both, once.

By default it talks to http://localhost:8000 / ws://localhost:8000. Override
with --api-base / --ws-base for a deployed backend (ws-base defaults to
api-base with http(s) swapped for ws(s) if not given explicitly).

USAGE
-----
Once connected, type commands at the `device>` prompt. Type `help` to list
them. Key ones:

    status                    show connection / schedule / sensor summary
    schedule                  print the schedule currently stored on "device"
    dispense C                simulate dispensing compartment C right now
                               (reports the event + uploads a fake adherence video)
    telemetry                 send one telemetry report immediately
    battery 42                set simulated battery % used in future telemetry
    tray low                  set simulated tray state (full|low|empty)
    person on|off             set simulated person-detected sensor
    offline / online          simulate losing/regaining connectivity;
                               events queue locally while offline and flush
                               via /devices/sync-offline when back online
    restart                   simulate a reboot (reconnect + refetch schedule)
    voice demo                send a tiny silent WAV through the AI voice pipeline
    voice /path/to/file.wav   send a real recorded question file instead
    autodispense on|off       toggle whether the sim fires doses on schedule
    quit                      stop the simulator

Autonomous dispensing checks the locally stored schedule every 15s and fires
a dispense automatically when the clock matches — same as real firmware is
expected to behave, so you can also just leave this running and watch the
caregiver dashboard update on its own.
"""
import argparse
import asyncio
import io
import json
import random
import struct
import sys
import wave
from datetime import datetime, date
from pathlib import Path
from typing import Optional

import httpx
import websockets

DAY_CODES = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
LETTERS = ["A", "B", "C", "D", "E", "F", "G"]


def now_hhmm() -> str:
    return datetime.now().strftime("%H:%M")


def make_silent_wav(seconds: float = 1.5) -> bytes:
    """Generates a tiny valid silent WAV file (16kHz mono) as a stand-in for a recorded question."""
    buf = io.BytesIO()
    n_frames = int(16000 * seconds)
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(struct.pack("<%dh" % n_frames, *([0] * n_frames)))
    return buf.getvalue()


def make_fake_video(seconds: int = 120) -> bytes:
    """A small placeholder blob standing in for a 2-minute adherence clip - not a real video file,
    just enough bytes for the upload endpoint to accept and store."""
    return b"FAKEMP4" + random.randbytes(2048) + f"__duration={seconds}s__".encode()


class MockDevice:
    def __init__(self, api_base: str, ws_base: str, device_uid: str, device_secret: str,
                 telemetry_interval: float, autodispense: bool):
        self.api_base = api_base.rstrip("/")
        self.ws_base = ws_base.rstrip("/")
        self.device_uid = device_uid
        self.device_secret = device_secret
        self.telemetry_interval = telemetry_interval
        self.autodispense_enabled = autodispense

        self.headers = {"X-Device-Id": device_uid, "X-Device-Secret": device_secret}
        self.client = httpx.AsyncClient(base_url=self.api_base, headers=self.headers, timeout=15)

        self.schedule: dict = {}
        self.online = True
        self.ws: Optional[websockets.WebSocketClientProtocol] = None
        self.ws_task: Optional[asyncio.Task] = None
        self._stop = asyncio.Event()

        # simulated sensors
        self.battery = 92.0
        self.tray_state = "full"
        self.person_detected = True
        self.uptime_started = datetime.now()

        # offline cache
        self.offline_dispense_queue: list = []
        self.offline_telemetry_queue: list = []
        self._dispensed_today: dict = {}  # compartment -> date string

    # ------------------------------------------------------------------ #
    # Connection lifecycle
    # ------------------------------------------------------------------ #
    async def connect_ws(self):
        url = f"{self.ws_base}/devices/ws?device_uid={self.device_uid}&secret={self.device_secret}"
        try:
            self.ws = await websockets.connect(url, ping_interval=20, ping_timeout=20)
            print(f"\n[ws] connected -> {url}")
        except Exception as exc:
            print(f"\n[ws] connection failed: {exc}")
            self.ws = None
            return

        try:
            async for raw in self.ws:
                await self._handle_ws_message(raw)
        except websockets.ConnectionClosed:
            print("\n[ws] connection closed")
        finally:
            self.ws = None

    async def _handle_ws_message(self, raw: str):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            print(f"\n[ws] non-JSON message: {raw!r}")
            return

        msg_type = msg.get("type")
        print(f"\n[ws] <- {msg_type}  {json.dumps(msg)[:200]}")

        if msg_type == "update_schedule":
            self.schedule = msg.get("compartments", {})
            print("[device] schedule updated from server")
        elif msg_type == "manual_dispense":
            compartment = (msg.get("payload") or {}).get("compartment")
            if compartment:
                await self.dispense(compartment, status="manual")
        elif msg_type == "restart":
            print("[device] server requested restart — simulating reboot")
            asyncio.create_task(self.simulate_restart())
        elif msg_type == "sync":
            print("[device] server requested offline sync")
            await self.flush_offline()
        elif msg_type == "configure":
            print(f"[device] configure payload received: {(msg.get('payload') or {})}")

        if msg.get("command_id") and self.ws:
            await self.ws.send(json.dumps({"type": "ack", "command_id": msg["command_id"]}))
            print(f"[ws] -> ack {msg['command_id']}")

    async def simulate_restart(self):
        if self.ws:
            await self.ws.close()
        await asyncio.sleep(1.5)
        print("[device] back up — reconnecting")
        try:
            self.schedule = await self.client.get("/devices/schedule")
        except Exception:
            pass
        self.ws_task = asyncio.create_task(self.connect_ws())

    # ------------------------------------------------------------------ #
    # HTTPS actions
    # ------------------------------------------------------------------ #
    async def dispense(self, compartment: str, status: str = "success", scheduled_time: Optional[str] = None):
        compartment = compartment.upper()
        dispensed_at = datetime.now().isoformat()
        payload = {
            "compartment": compartment,
            "status": status,
            "scheduled_time": scheduled_time,
            "dispensed_at": dispensed_at,
            "was_offline_cached": not self.online,
        }

        if not self.online:
            self.offline_dispense_queue.append(payload)
            print(f"[dispense] {compartment} ({status}) queued — device is offline ({len(self.offline_dispense_queue)} queued)")
            return

        try:
            res = await self.client.post("/devices/dispense-event", json=payload)
            res.raise_for_status()
            event = res.json()
            print(f"[dispense] {compartment} ({status}) reported -> event {event['id']}")
            await self._upload_fake_video(event["id"])
        except httpx.HTTPStatusError as exc:
            print(f"[dispense] backend rejected event: {exc.response.status_code} {exc.response.text}")
        except httpx.HTTPError as exc:
            print(f"[dispense] network error: {exc}")

    async def _upload_fake_video(self, dispense_event_id: str):
        files = {"video": ("adherence.mp4", make_fake_video(), "video/mp4")}
        data = {"dispense_event_id": dispense_event_id, "duration_seconds": "120"}
        try:
            res = await self.client.post("/devices/adherence-video", data=data, files=files)
            res.raise_for_status()
            print(f"[video] uploaded adherence clip for event {dispense_event_id}")
        except httpx.HTTPError as exc:
            print(f"[video] upload failed: {exc}")

    async def send_telemetry(self):
        payload = {
            "current_compartment": None,
            "motor_status": "idle",
            "sensor_status": "ok",
            "person_detected": self.person_detected,
            "tray_state": self.tray_state,
            "battery_level": self.battery,
            "wifi_rssi": -1 * random.randint(40, 70),
            "uptime_seconds": int((datetime.now() - self.uptime_started).total_seconds()),
        }
        if not self.online:
            self.offline_telemetry_queue.append(payload)
            print(f"[telemetry] queued — device is offline ({len(self.offline_telemetry_queue)} queued)")
            return
        try:
            res = await self.client.post("/devices/telemetry", json=payload)
            res.raise_for_status()
            print(f"[telemetry] sent (battery {self.battery:.0f}%, tray {self.tray_state})")
        except httpx.HTTPError as exc:
            print(f"[telemetry] failed: {exc}")

    async def flush_offline(self):
        if not self.offline_dispense_queue and not self.offline_telemetry_queue:
            print("[sync] nothing queued")
            return
        payload = {"dispense_events": self.offline_dispense_queue, "telemetry": self.offline_telemetry_queue}
        try:
            res = await self.client.post("/devices/sync-offline", json=payload)
            res.raise_for_status()
            print(f"[sync] flushed -> {res.json()}")
            self.offline_dispense_queue = []
            self.offline_telemetry_queue = []
        except httpx.HTTPError as exc:
            print(f"[sync] failed: {exc}")

    async def send_voice_query(self, audio_bytes: bytes, audio_format: str = "wav"):
        files = {"audio": (f"query.{audio_format}", audio_bytes, "application/octet-stream")}
        data = {"audio_format": audio_format}
        try:
            res = await self.client.post("/devices/voice-query", data=data, files=files, timeout=60)
            res.raise_for_status()
            interaction = res.json()
            print(f"[voice] transcript: {interaction.get('transcript')!r}")
            print(f"[voice] answer:     {interaction.get('answer_text')!r}")

            audio_res = await self.client.get(f"/devices/voice-query/{interaction['id']}/audio")
            audio_res.raise_for_status()
            out_path = Path(f"voice_response_{interaction['id']}.{interaction.get('audio_format') or 'wav'}")
            out_path.write_bytes(audio_res.content)
            print(f"[voice] response audio saved to {out_path} (play it locally — this simulator has no speaker)")
        except httpx.HTTPStatusError as exc:
            print(f"[voice] backend rejected request: {exc.response.status_code} {exc.response.text}")
        except httpx.HTTPError as exc:
            print(f"[voice] network error: {exc}")

    # ------------------------------------------------------------------ #
    # Autonomous behavior (mirrors what real firmware is expected to do)
    # ------------------------------------------------------------------ #
    async def autodispense_loop(self):
        while not self._stop.is_set():
            if self.autodispense_enabled:
                today_str = date.today().isoformat()
                weekday_code = DAY_CODES[datetime.now().weekday()]
                hhmm = now_hhmm()
                for letter, entry in (self.schedule or {}).items():
                    if not entry:
                        continue
                    if entry.get("dispense_time") != hhmm:
                        continue
                    freq = entry.get("frequency")
                    if freq == "as_needed":
                        continue
                    if freq == "specific_days" and weekday_code not in (entry.get("days_of_week") or []):
                        continue
                    if self._dispensed_today.get(letter) == today_str:
                        continue
                    self._dispensed_today[letter] = today_str
                    print(f"\n[auto] scheduled time hit for compartment {letter} — dispensing")
                    await self.dispense(letter, status="success", scheduled_time=entry.get("dispense_time"))
            await asyncio.sleep(15)

    async def telemetry_loop(self):
        while not self._stop.is_set():
            await self.send_telemetry()
            await asyncio.sleep(self.telemetry_interval)

    # ------------------------------------------------------------------ #
    # Terminal command interface
    # ------------------------------------------------------------------ #
    def print_status(self):
        print(f"""
device_uid       : {self.device_uid}
connectivity     : {'ONLINE' if self.online else 'OFFLINE (simulated)'}
websocket        : {'connected' if self.ws else 'disconnected'}
battery          : {self.battery:.0f}%
tray             : {self.tray_state}
person_detected  : {self.person_detected}
autodispense     : {'on' if self.autodispense_enabled else 'off'}
queued dispenses : {len(self.offline_dispense_queue)}
queued telemetry : {len(self.offline_telemetry_queue)}
compartments set : {', '.join(k for k, v in (self.schedule or {}).items() if v) or '(none)'}
""".strip())

    def print_schedule(self):
        if not any(self.schedule.values()):
            print("[schedule] nothing assigned yet")
            return
        for letter in LETTERS:
            entry = (self.schedule or {}).get(letter)
            if entry:
                meds = ", ".join(m["name"] for m in entry.get("medications", []))
                print(f"  {letter}: {entry['dispense_time']}  {entry['frequency']:<14} {meds}")
            else:
                print(f"  {letter}: —")

    async def handle_command(self, line: str):
        parts = line.strip().split()
        if not parts:
            return
        cmd, *args = parts

        if cmd in ("quit", "exit"):
            self._stop.set()
            if self.ws:
                await self.ws.close()
            return

        elif cmd == "help":
            print(__doc__)

        elif cmd == "status":
            self.print_status()

        elif cmd == "schedule":
            self.print_schedule()

        elif cmd == "dispense":
            if not args:
                print("usage: dispense <A-G> [status]")
            else:
                status = args[1] if len(args) > 1 else "success"
                await self.dispense(args[0], status=status)

        elif cmd == "telemetry":
            await self.send_telemetry()

        elif cmd == "battery":
            if args:
                self.battery = max(0.0, min(100.0, float(args[0])))
                print(f"[sim] battery set to {self.battery:.0f}%")

        elif cmd == "tray":
            if args and args[0] in ("full", "low", "empty"):
                self.tray_state = args[0]
                print(f"[sim] tray set to {self.tray_state}")
            else:
                print("usage: tray <full|low|empty>")

        elif cmd == "person":
            if args and args[0] in ("on", "off"):
                self.person_detected = args[0] == "on"
                print(f"[sim] person_detected set to {self.person_detected}")
            else:
                print("usage: person <on|off>")

        elif cmd == "offline":
            self.online = False
            if self.ws:
                await self.ws.close()
            print("[sim] now offline — dispenses/telemetry will queue locally")

        elif cmd == "online":
            self.online = True
            print("[sim] back online — reconnecting and flushing queue")
            self.ws_task = asyncio.create_task(self.connect_ws())
            await self.flush_offline()

        elif cmd == "restart":
            await self.simulate_restart()

        elif cmd == "autodispense":
            if args and args[0] in ("on", "off"):
                self.autodispense_enabled = args[0] == "on"
                print(f"[sim] autodispense {'enabled' if self.autodispense_enabled else 'disabled'}")
            else:
                print("usage: autodispense <on|off>")

        elif cmd == "voice":
            if not args:
                print("usage: voice demo | voice /path/to/file.wav")
            elif args[0] == "demo":
                print("[voice] recording a demo (silent) question...")
                await self.send_voice_query(make_silent_wav(), audio_format="wav")
            else:
                path = Path(" ".join(args))
                if not path.exists():
                    print(f"[voice] file not found: {path}")
                else:
                    fmt = "mp3" if path.suffix.lower() == ".mp3" else "wav"
                    await self.send_voice_query(path.read_bytes(), audio_format=fmt)

        else:
            print(f"unknown command: {cmd!r} (type 'help' for the list)")

    async def stdin_loop(self):
        loop = asyncio.get_event_loop()
        while not self._stop.is_set():
            try:
                line = await loop.run_in_executor(None, sys.stdin.readline)
            except Exception:
                break
            if line == "":  # EOF
                self._stop.set()
                break
            try:
                await self.handle_command(line)
            except Exception as exc:
                print(f"[error] {exc}")
            if not self._stop.is_set():
                print("device> ", end="", flush=True)

    async def run(self):
        print(f"MedAdhere mock device — {self.device_uid}")
        print(f"API: {self.api_base}   WS: {self.ws_base}")
        await self.fetch_schedule()

        self.ws_task = asyncio.create_task(self.connect_ws())
        telemetry_task = asyncio.create_task(self.telemetry_loop())
        autodispense_task = asyncio.create_task(self.autodispense_loop())

        print("Type 'help' for commands.\ndevice> ", end="", flush=True)
        await self.stdin_loop()

        for t in (self.ws_task, telemetry_task, autodispense_task):
            if t:
                t.cancel()
        await self.client.aclose()
        print("\n[device] shut down")

    async def fetch_schedule(self):
        try:
            res = await self.client.get("/devices/schedule")
            res.raise_for_status()
            self.schedule = res.json().get("compartments", {})
            print("[device] fetched initial schedule")
        except httpx.HTTPStatusError as exc:
            print(f"[device] couldn't fetch schedule yet: {exc.response.status_code} {exc.response.text}")
        except httpx.HTTPError as exc:
            print(f"[device] couldn't reach backend: {exc}")


def main():
    parser = argparse.ArgumentParser(description="Mock MedAdhere hardware device for testing.")
    parser.add_argument("--api-base", default="http://localhost:8000", help="Backend HTTP base URL")
    parser.add_argument("--ws-base", default=None, help="Backend WS base URL (derived from --api-base if omitted)")
    parser.add_argument("--device-uid", required=True, help="Device serial/QR code, from device assignment")
    parser.add_argument("--device-secret", required=True, help="Device secret, from device assignment")
    parser.add_argument("--telemetry-interval", type=float, default=30.0, help="Seconds between telemetry reports")
    parser.add_argument("--no-autodispense", action="store_true", help="Disable automatic dispensing on schedule")
    args = parser.parse_args()

    ws_base = args.ws_base or args.api_base.replace("https://", "wss://").replace("http://", "ws://")

    device = MockDevice(
        api_base=args.api_base,
        ws_base=ws_base,
        device_uid=args.device_uid,
        device_secret=args.device_secret,
        telemetry_interval=args.telemetry_interval,
        autodispense=not args.no_autodispense,
    )
    try:
        asyncio.run(device.run())
    except KeyboardInterrupt:
        print("\n[device] interrupted, exiting")


if __name__ == "__main__":
    main()
