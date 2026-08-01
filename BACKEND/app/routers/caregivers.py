from datetime import datetime
from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from app import models, schemas
from app.database import get_db
from app.deps import get_current_caregiver, require_owned_patient
from app.schedule_utils import build_device_schedule_payload, schedule_to_out
from app.security import hash_password
from app.ws_manager import manager

router = APIRouter(prefix="/caregivers", tags=["Caregivers"])


@router.get("/me", response_model=schemas.CaregiverOut)
def get_me(caregiver: models.Caregiver = Depends(get_current_caregiver)):
    return caregiver


# ---------------------------------------------------------------------------
# Patient enrolment & management
# ---------------------------------------------------------------------------

@router.post("/patients", response_model=schemas.PatientOut, status_code=201)
def enrol_patient(
    payload: schemas.PatientCreate,
    caregiver: models.Caregiver = Depends(get_current_caregiver),
    db: Session = Depends(get_db),
):
    if payload.email:
        existing = db.query(models.Patient).filter(models.Patient.email == payload.email).first()
        if existing:
            raise HTTPException(status_code=409, detail="Email already used by another patient")

    patient = models.Patient(
        caregiver_id=caregiver.id,
        full_name=payload.full_name,
        date_of_birth=payload.date_of_birth,
        phone=payload.phone,
        email=payload.email,
        hashed_password=hash_password(payload.password) if payload.password else None,
        notes=payload.notes,
        timezone=payload.timezone or "UTC",
    )
    db.add(patient)
    db.commit()
    db.refresh(patient)
    return patient


@router.get("/patients", response_model=List[schemas.PatientOut])
def list_patients(caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    return db.query(models.Patient).filter(models.Patient.caregiver_id == caregiver.id).all()


@router.get("/patients/{patient_id}", response_model=schemas.PatientOut)
def get_patient(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                 db: Session = Depends(get_db)):
    return require_owned_patient(patient_id, caregiver, db)


@router.patch("/patients/{patient_id}", response_model=schemas.PatientOut)
def update_patient(patient_id: str, payload: schemas.PatientUpdate,
                    caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    patient = require_owned_patient(patient_id, caregiver, db)
    for field, value in payload.model_dump(exclude_unset=True).items():
        setattr(patient, field, value)
    db.commit()
    db.refresh(patient)
    return patient


@router.delete("/patients/{patient_id}", status_code=204)
def delete_patient(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                    db: Session = Depends(get_db)):
    patient = require_owned_patient(patient_id, caregiver, db)
    db.delete(patient)
    db.commit()


# ---------------------------------------------------------------------------
# Hardware device assignment
# ---------------------------------------------------------------------------

@router.post("/patients/{patient_id}/device", response_model=schemas.DeviceOut, status_code=201)
def assign_device(patient_id: str, payload: schemas.DeviceRegister,
                   caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    """Give a patient a hardware unit: registers/claims the device_uid printed on it."""
    patient = require_owned_patient(patient_id, caregiver, db)
    if patient.device:
        raise HTTPException(status_code=409, detail="Patient already has a device assigned")

    device = db.query(models.Device).filter(models.Device.device_uid == payload.device_uid).first()
    if device and device.patient_id:
        raise HTTPException(status_code=409, detail="Device already assigned to a patient")

    if not device:
        device = models.Device(device_uid=payload.device_uid, caregiver_id=caregiver.id)
        db.add(device)

    device.caregiver_id = caregiver.id
    device.patient_id = patient.id
    device.status = models.DeviceStatus.OFFLINE
    db.commit()
    db.refresh(device)
    return device


@router.get("/patients/{patient_id}/device", response_model=schemas.DeviceStatusOut)
def get_patient_device(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                        db: Session = Depends(get_db)):
    patient = require_owned_patient(patient_id, caregiver, db)
    if not patient.device:
        raise HTTPException(status_code=404, detail="No device assigned to this patient")
    return patient.device


@router.post("/devices/{device_uid}/commands", status_code=202)
async def send_device_command(device_uid: str, payload: schemas.DeviceCommandRequest,
                               caregiver: models.Caregiver = Depends(get_current_caregiver),
                               db: Session = Depends(get_db)):
    """Real-time command over the device's WebSocket: update_schedule|manual_dispense|restart|sync|configure."""
    device = db.query(models.Device).filter(models.Device.device_uid == device_uid).first()
    if not device or device.caregiver_id != caregiver.id:
        raise HTTPException(status_code=404, detail="Device not found")

    try:
        command_type = models.CommandType(payload.command_type)
    except ValueError:
        raise HTTPException(status_code=422, detail="Invalid command_type")

    command = models.DeviceCommand(device_id=device.id, command_type=command_type, payload=payload.payload)
    db.add(command)
    db.commit()
    db.refresh(command)

    message = {"type": command_type.value, "command_id": command.id, "payload": payload.payload or {}}
    if command_type == models.CommandType.UPDATE_SCHEDULE and device.patient:
        message = build_device_schedule_payload(db, device.patient)
        message["command_id"] = command.id

    delivered = await manager.send_json(device_uid, message)
    command.status = models.CommandStatus.SENT if delivered else models.CommandStatus.QUEUED
    command.sent_at = datetime.utcnow() if delivered else None
    db.commit()

    return {"command_id": command.id, "delivered": delivered,
            "note": None if delivered else "Device is offline; command is queued and will need to be resent, "
                                             "or the device will pick it up via GET /devices/schedule on reconnect."}


# ---------------------------------------------------------------------------
# Medications
# ---------------------------------------------------------------------------

@router.post("/patients/{patient_id}/medications", response_model=schemas.MedicationOut, status_code=201)
def create_medication(patient_id: str, payload: schemas.MedicationCreate,
                       caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    patient = require_owned_patient(patient_id, caregiver, db)
    med = models.Medication(patient_id=patient.id, **payload.model_dump())
    db.add(med)
    db.commit()
    db.refresh(med)
    return med


@router.get("/patients/{patient_id}/medications", response_model=List[schemas.MedicationOut])
def list_medications(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                      db: Session = Depends(get_db)):
    require_owned_patient(patient_id, caregiver, db)
    return db.query(models.Medication).filter(models.Medication.patient_id == patient_id).all()


def _get_owned_medication(medication_id: str, caregiver: models.Caregiver, db: Session) -> models.Medication:
    med = db.query(models.Medication).filter(models.Medication.id == medication_id).first()
    if not med or med.patient.caregiver_id != caregiver.id:
        raise HTTPException(status_code=404, detail="Medication not found")
    return med


@router.patch("/medications/{medication_id}", response_model=schemas.MedicationOut)
def update_medication(medication_id: str, payload: schemas.MedicationUpdate,
                       caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    med = _get_owned_medication(medication_id, caregiver, db)
    for field, value in payload.model_dump(exclude_unset=True).items():
        setattr(med, field, value)
    db.commit()
    db.refresh(med)
    return med


@router.delete("/medications/{medication_id}", status_code=204)
def delete_medication(medication_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                       db: Session = Depends(get_db)):
    med = _get_owned_medication(medication_id, caregiver, db)
    db.delete(med)
    db.commit()


# ---------------------------------------------------------------------------
# Schedules (compartment assignment) - creates/updates auto-sync to hardware
# ---------------------------------------------------------------------------

async def _sync_schedule_to_device(db: Session, patient: models.Patient):
    if patient.device:
        payload = build_device_schedule_payload(db, patient)
        await manager.send_json(patient.device.device_uid, payload)


@router.post("/patients/{patient_id}/schedules", response_model=schemas.ScheduleOut, status_code=201)
async def create_schedule(patient_id: str, payload: schemas.ScheduleCreate,
                           caregiver: models.Caregiver = Depends(get_current_caregiver),
                           db: Session = Depends(get_db)):
    patient = require_owned_patient(patient_id, caregiver, db)

    try:
        compartment = models.Compartment(payload.compartment.upper())
    except ValueError:
        raise HTTPException(status_code=422, detail="compartment must be one letter A-G")
    try:
        frequency = models.Frequency(payload.frequency)
    except ValueError:
        raise HTTPException(status_code=422, detail="Invalid frequency")
    if frequency == models.Frequency.SPECIFIC_DAYS and not payload.days_of_week:
        raise HTTPException(status_code=422, detail="days_of_week is required for specific_days frequency")

    medications = db.query(models.Medication).filter(
        models.Medication.id.in_(payload.medication_ids), models.Medication.patient_id == patient.id
    ).all()
    if len(medications) != len(set(payload.medication_ids)):
        raise HTTPException(status_code=422, detail="One or more medication_ids not found for this patient")

    existing = db.query(models.Schedule).filter(
        models.Schedule.patient_id == patient.id, models.Schedule.compartment == compartment,
        models.Schedule.active.is_(True),
    ).first()
    if existing:
        raise HTTPException(status_code=409, detail=f"Compartment {compartment.value} is already in active use")

    schedule = models.Schedule(
        patient_id=patient.id,
        created_by_caregiver_id=caregiver.id,
        compartment=compartment,
        dispense_time=payload.dispense_time,
        frequency=frequency,
        days_of_week=payload.days_of_week,
        start_date=payload.start_date,
        end_date=payload.end_date,
        medications=medications,
    )
    db.add(schedule)
    db.commit()
    db.refresh(schedule)

    await _sync_schedule_to_device(db, patient)
    _notify(db, patient, models.NotificationType.SCHEDULE_UPDATED,
            f"New schedule added for compartment {compartment.value} at {payload.dispense_time}", caregiver.id)

    return schedule_to_out(schedule)


@router.get("/patients/{patient_id}/schedules", response_model=List[schemas.ScheduleOut])
def list_schedules(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                    db: Session = Depends(get_db)):
    require_owned_patient(patient_id, caregiver, db)
    schedules = db.query(models.Schedule).filter(models.Schedule.patient_id == patient_id).all()
    return [schedule_to_out(s) for s in schedules]


def _get_owned_schedule(schedule_id: str, caregiver: models.Caregiver, db: Session) -> models.Schedule:
    schedule = db.query(models.Schedule).filter(models.Schedule.id == schedule_id).first()
    if not schedule or schedule.patient.caregiver_id != caregiver.id:
        raise HTTPException(status_code=404, detail="Schedule not found")
    return schedule


@router.patch("/schedules/{schedule_id}", response_model=schemas.ScheduleOut)
async def update_schedule(schedule_id: str, payload: schemas.ScheduleUpdate,
                           caregiver: models.Caregiver = Depends(get_current_caregiver),
                           db: Session = Depends(get_db)):
    schedule = _get_owned_schedule(schedule_id, caregiver, db)

    data = payload.model_dump(exclude_unset=True)
    if "medication_ids" in data:
        medications = db.query(models.Medication).filter(
            models.Medication.id.in_(data["medication_ids"]), models.Medication.patient_id == schedule.patient_id
        ).all()
        schedule.medications = medications
        data.pop("medication_ids")
    if "frequency" in data:
        try:
            data["frequency"] = models.Frequency(data["frequency"])
        except ValueError:
            raise HTTPException(status_code=422, detail="Invalid frequency")

    for field, value in data.items():
        setattr(schedule, field, value)
    schedule.updated_at = datetime.utcnow()
    db.commit()
    db.refresh(schedule)

    await _sync_schedule_to_device(db, schedule.patient)
    return schedule_to_out(schedule)


@router.delete("/schedules/{schedule_id}", status_code=204)
async def delete_schedule(schedule_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                           db: Session = Depends(get_db)):
    schedule = _get_owned_schedule(schedule_id, caregiver, db)
    patient = schedule.patient
    db.delete(schedule)
    db.commit()
    await _sync_schedule_to_device(db, patient)


@router.post("/patients/{patient_id}/schedules/sync", status_code=202)
async def force_sync_schedule(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                               db: Session = Depends(get_db)):
    """Re-push the full 7-compartment schedule to the device (e.g. after it reconnects)."""
    patient = require_owned_patient(patient_id, caregiver, db)
    if not patient.device:
        raise HTTPException(status_code=404, detail="No device assigned")
    delivered = await manager.send_json(patient.device.device_uid, build_device_schedule_payload(db, patient))
    return {"delivered": delivered}


# ---------------------------------------------------------------------------
# Monitoring: dispense logs, adherence videos, telemetry, voice history
# ---------------------------------------------------------------------------

@router.get("/patients/{patient_id}/dispense-logs", response_model=List[schemas.DispenseEventOut])
def list_dispense_logs(patient_id: str, limit: int = 100,
                        caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    require_owned_patient(patient_id, caregiver, db)
    events = (
        db.query(models.DispenseEvent)
        .filter(models.DispenseEvent.patient_id == patient_id)
        .order_by(models.DispenseEvent.dispensed_at.desc())
        .limit(limit)
        .all()
    )
    out = []
    for e in events:
        item = schemas.DispenseEventOut.model_validate(e)
        item.has_video = e.adherence_video is not None
        out.append(item)
    return out


@router.get("/patients/{patient_id}/adherence-videos", response_model=List[schemas.AdherenceVideoOut])
def list_adherence_videos(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                           db: Session = Depends(get_db)):
    require_owned_patient(patient_id, caregiver, db)
    return (
        db.query(models.AdherenceVideo)
        .join(models.DispenseEvent)
        .filter(models.DispenseEvent.patient_id == patient_id)
        .order_by(models.AdherenceVideo.uploaded_at.desc())
        .all()
    )


@router.get("/patients/{patient_id}/telemetry", response_model=List[schemas.TelemetryOut])
def list_telemetry(patient_id: str, limit: int = 50,
                    caregiver: models.Caregiver = Depends(get_current_caregiver), db: Session = Depends(get_db)):
    patient = require_owned_patient(patient_id, caregiver, db)
    if not patient.device:
        return []
    return (
        db.query(models.Telemetry)
        .filter(models.Telemetry.device_id == patient.device.id)
        .order_by(models.Telemetry.reported_at.desc())
        .limit(limit)
        .all()
    )


@router.get("/patients/{patient_id}/voice-interactions", response_model=List[schemas.VoiceInteractionOut])
def list_voice_interactions(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                             db: Session = Depends(get_db)):
    require_owned_patient(patient_id, caregiver, db)
    return (
        db.query(models.VoiceInteraction)
        .filter(models.VoiceInteraction.patient_id == patient_id)
        .order_by(models.VoiceInteraction.created_at.desc())
        .all()
    )


@router.get("/patients/{patient_id}/notifications", response_model=List[schemas.NotificationOut])
def list_notifications(patient_id: str, caregiver: models.Caregiver = Depends(get_current_caregiver),
                        db: Session = Depends(get_db)):
    require_owned_patient(patient_id, caregiver, db)
    return (
        db.query(models.Notification)
        .filter(models.Notification.patient_id == patient_id)
        .order_by(models.Notification.created_at.desc())
        .all()
    )


def _notify(db: Session, patient: models.Patient, ntype: models.NotificationType, message: str,
            caregiver_id: Optional[str] = None):
    note = models.Notification(patient_id=patient.id, caregiver_id=caregiver_id, type=ntype, message=message)
    db.add(note)
    db.commit()
