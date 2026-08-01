import json
import os
import uuid
from datetime import datetime
from typing import Optional

from fastapi import (
    APIRouter, Depends, File, Form, HTTPException, UploadFile,
    WebSocket, WebSocketDisconnect, status,
)
from sqlalchemy.orm import Session

from app import ai_client, models, schemas
from app.config import settings
from app.database import SessionLocal, get_db
from app.deps import authenticate_device
from app.schedule_utils import build_device_schedule_payload
from app.ws_manager import manager

router = APIRouter(prefix="/devices", tags=["Devices (hardware)"])


# ---------------------------------------------------------------------------
# WebSocket: persistent connection, real-time commands, schedule push
# ---------------------------------------------------------------------------

@router.websocket("/ws")
async def device_websocket(
    websocket: WebSocket,
    device_uid: str,
    secret: str,
):
    """
    Device connects here right after Wi-Fi + auth, e.g.:
      wss://host/devices/ws?device_uid=DEV123&secret=<device_secret>

    On connect the server authenticates the device, marks it online, and
    immediately pushes the current 7-compartment schedule. After that it's a
    duplex channel: the server can push commands (update_schedule,
    manual_dispense, restart, sync, configure) at any time, and the device can
    send small real-time messages (e.g. {"type":"ack", "command_id": "..."}).

    Bulk data (dispense events, adherence videos, telemetry) should still go
    over the dedicated HTTPS endpoints below, since those need reliable
    delivery of possibly-large payloads (esp. video). This socket is for
    control-plane traffic only.
    """
    db = SessionLocal()
    try:
        device = db.query(models.Device).filter(models.Device.device_uid == device_uid).first()
        if not device or device.device_secret != secret:
            await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
            return

        await manager.connect(device_uid, websocket)
        device.status = models.DeviceStatus.ONLINE
        device.last_seen_at = datetime.utcnow()
        db.commit()

        if device.patient:
            await manager.send_json(device_uid, build_device_schedule_payload(db, device.patient))

        while True:
            raw = await websocket.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            device.last_seen_at = datetime.utcnow()
            db.commit()

            msg_type = msg.get("type")
            if msg_type == "ack" and msg.get("command_id"):
                cmd = db.query(models.DeviceCommand).filter(models.DeviceCommand.id == msg["command_id"]).first()
                if cmd:
                    cmd.status = models.CommandStatus.ACKED
                    cmd.acked_at = datetime.utcnow()
                    db.commit()
            elif msg_type == "heartbeat":
                pass  # last_seen_at already updated above
            # dispense_event / telemetry are also accepted here for convenience,
            # but firmware should prefer the HTTPS endpoints for guaranteed delivery.

    except WebSocketDisconnect:
        pass
    finally:
        manager.disconnect(device_uid)
        device = db.query(models.Device).filter(models.Device.device_uid == device_uid).first()
        if device:
            device.status = models.DeviceStatus.OFFLINE
            db.commit()
        db.close()


# ---------------------------------------------------------------------------
# Pull-based schedule fetch (fallback to the WS push, e.g. right after boot)
# ---------------------------------------------------------------------------

@router.get("/schedule")
def get_schedule(device: models.Device = Depends(authenticate_device), db: Session = Depends(get_db)):
    if not device.patient:
        raise HTTPException(status_code=404, detail="Device not assigned to a patient yet")
    return build_device_schedule_payload(db, device.patient)


# ---------------------------------------------------------------------------
# Dispense events
# ---------------------------------------------------------------------------

def _persist_dispense_event(db: Session, device: models.Device, payload: schemas.DispenseEventIn) -> models.DispenseEvent:
    try:
        compartment = models.Compartment(payload.compartment.upper())
    except ValueError:
        raise HTTPException(status_code=422, detail="compartment must be one letter A-G")
    try:
        dispense_status = models.DispenseStatus(payload.status)
    except ValueError:
        raise HTTPException(status_code=422, detail="Invalid status")

    schedule = (
        db.query(models.Schedule)
        .filter(models.Schedule.patient_id == device.patient_id,
                models.Schedule.compartment == compartment,
                models.Schedule.active.is_(True))
        .first()
    )

    event = models.DispenseEvent(
        device_id=device.id,
        patient_id=device.patient_id,
        schedule_id=schedule.id if schedule else None,
        compartment=compartment,
        status=dispense_status,
        scheduled_time=payload.scheduled_time,
        dispensed_at=payload.dispensed_at,
        was_offline_cached=payload.was_offline_cached,
        raw_payload=payload.model_dump(mode="json"),
    )
    db.add(event)
    db.commit()
    db.refresh(event)
    return event


@router.post("/dispense-event", response_model=schemas.DispenseEventOut, status_code=201)
def report_dispense_event(payload: schemas.DispenseEventIn, device: models.Device = Depends(authenticate_device),
                           db: Session = Depends(get_db)):
    if not device.patient_id:
        raise HTTPException(status_code=409, detail="Device not assigned to a patient")
    event = _persist_dispense_event(db, device, payload)
    out = schemas.DispenseEventOut.model_validate(event)
    out.has_video = False
    return out


# ---------------------------------------------------------------------------
# Adherence video upload (2-minute clip recorded after each dispense)
# ---------------------------------------------------------------------------

@router.post("/adherence-video", response_model=schemas.AdherenceVideoOut, status_code=201)
async def upload_adherence_video(
    dispense_event_id: str = Form(...),
    duration_seconds: int = Form(120),
    video: UploadFile = File(...),
    device: models.Device = Depends(authenticate_device),
    db: Session = Depends(get_db),
):
    event = db.query(models.DispenseEvent).filter(models.DispenseEvent.id == dispense_event_id).first()
    if not event or event.device_id != device.id:
        raise HTTPException(status_code=404, detail="dispense_event_id not found for this device")

    ext = os.path.splitext(video.filename or "")[1] or ".mp4"
    filename = f"{event.id}_{uuid.uuid4().hex[:8]}{ext}"
    dest_path = os.path.join(settings.VIDEO_DIR, filename)
    with open(dest_path, "wb") as f:
        while chunk := await video.read(1024 * 1024):
            f.write(chunk)

    adherence_video = models.AdherenceVideo(
        dispense_event_id=event.id,
        file_path=dest_path,
        duration_seconds=duration_seconds,
    )
    db.add(adherence_video)
    db.commit()
    db.refresh(adherence_video)
    return adherence_video


# ---------------------------------------------------------------------------
# Telemetry
# ---------------------------------------------------------------------------

def _persist_telemetry(db: Session, device: models.Device, payload: schemas.TelemetryIn) -> models.Telemetry:
    telemetry = models.Telemetry(
        device_id=device.id,
        reported_at=payload.reported_at or datetime.utcnow(),
        current_compartment=payload.current_compartment,
        motor_status=payload.motor_status,
        sensor_status=payload.sensor_status,
        person_detected=payload.person_detected,
        tray_state=payload.tray_state,
        battery_level=payload.battery_level,
        wifi_rssi=payload.wifi_rssi,
        uptime_seconds=payload.uptime_seconds,
        dispense_history=payload.dispense_history,
        raw_payload=payload.model_dump(mode="json"),
    )
    db.add(telemetry)

    device.last_seen_at = datetime.utcnow()
    device.status = models.DeviceStatus.ONLINE
    if payload.battery_level is not None:
        device.battery_level = payload.battery_level
    if payload.uptime_seconds is not None:
        device.uptime_seconds = payload.uptime_seconds

    db.commit()
    db.refresh(telemetry)
    return telemetry


@router.post("/telemetry", response_model=schemas.TelemetryOut, status_code=201)
def report_telemetry(payload: schemas.TelemetryIn, device: models.Device = Depends(authenticate_device),
                      db: Session = Depends(get_db)):
    return _persist_telemetry(db, device, payload)


# ---------------------------------------------------------------------------
# Offline cache sync: batch of events + telemetry gathered while disconnected
# ---------------------------------------------------------------------------

@router.post("/sync-offline", status_code=201)
def sync_offline_batch(payload: schemas.OfflineSyncBatch, device: models.Device = Depends(authenticate_device),
                        db: Session = Depends(get_db)):
    saved_events = []
    for e in payload.dispense_events:
        e.was_offline_cached = True
        saved_events.append(_persist_dispense_event(db, device, e).id)

    saved_telemetry = [_persist_telemetry(db, device, t).id for t in payload.telemetry]

    return {"dispense_events_saved": len(saved_events), "telemetry_saved": len(saved_telemetry)}


# ---------------------------------------------------------------------------
# Voice query: device uploads recorded audio, gets back transcript + spoken answer
# ---------------------------------------------------------------------------

@router.post("/voice-query", response_model=schemas.VoiceInteractionOut, status_code=201)
async def voice_query(
    audio: UploadFile = File(...),
    audio_format: str = Form("wav"),
    device: models.Device = Depends(authenticate_device),
    db: Session = Depends(get_db),
):
    """
    Device records a question as a WAV/MP3 FILE (not a stream) and posts it
    here. The backend forwards it to the deployed AI's /voice/ask, stores the
    interaction, and returns the answer. The device then plays the returned
    audio_base64 (see the firmware guide for why file-upload beats streaming
    for an ESP32).
    """
    if not device.patient_id:
        raise HTTPException(status_code=409, detail="Device not assigned to a patient")

    audio_bytes = await audio.read()
    try:
        result = await ai_client.ask_voice(audio_bytes, audio.filename or "query.wav", audio_format)
    except ai_client.AIClientError as exc:
        raise HTTPException(status_code=502, detail=str(exc))

    # Persist the response audio to disk so the frontend can play it back later too.
    audio_bytes_out = ai_client.decode_audio_base64(result["audio_base64"])
    ext = ".mp3" if result.get("audio_format") == "mp3" else ".wav"
    filename = f"{uuid.uuid4().hex}{ext}"
    dest_path = os.path.join(settings.VOICE_DIR, filename)
    with open(dest_path, "wb") as f:
        f.write(audio_bytes_out)

    interaction = models.VoiceInteraction(
        patient_id=device.patient_id,
        device_id=device.id,
        transcript=result.get("transcript"),
        answer_text=result.get("answer"),
        citations=result.get("citations"),
        tool_results=result.get("tool_results"),
        response_audio_path=dest_path,
        audio_format=result.get("audio_format"),
    )
    db.add(interaction)
    db.commit()
    db.refresh(interaction)

    # NOTE: the response model above doesn't carry the audio bytes themselves
    # (kept out of the JSON body to stay light). The device should call
    # GET /devices/voice-query/{id}/audio to stream/download the actual file.
    return interaction


@router.get("/voice-query/{interaction_id}/audio")
async def get_voice_query_audio(interaction_id: str, device: models.Device = Depends(authenticate_device),
                                 db: Session = Depends(get_db)):
    from fastapi.responses import FileResponse
    interaction = db.query(models.VoiceInteraction).filter(models.VoiceInteraction.id == interaction_id).first()
    if not interaction or interaction.device_id != device.id:
        raise HTTPException(status_code=404, detail="Not found")
    media_type = "audio/mpeg" if interaction.audio_format == "mp3" else "audio/wav"
    return FileResponse(interaction.response_audio_path, media_type=media_type)
