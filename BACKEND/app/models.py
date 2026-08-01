import enum
import secrets
import uuid
from datetime import datetime

from sqlalchemy import (
    Boolean, Column, DateTime, Enum, Float, ForeignKey, Integer,
    JSON, String, Table, Text,
)
from sqlalchemy.orm import relationship

from app.database import Base


def gen_id() -> str:
    return uuid.uuid4().hex


def gen_secret() -> str:
    return secrets.token_hex(24)


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------

class Compartment(str, enum.Enum):
    A = "A"; B = "B"; C = "C"; D = "D"; E = "E"; F = "F"; G = "G"  # noqa: E702


class Frequency(str, enum.Enum):
    DAILY = "daily"
    SPECIFIC_DAYS = "specific_days"  # uses days_of_week
    AS_NEEDED = "as_needed"


class DispenseStatus(str, enum.Enum):
    SUCCESS = "success"
    FAILED = "failed"
    SKIPPED = "skipped"
    MANUAL = "manual"


class DeviceStatus(str, enum.Enum):
    UNASSIGNED = "unassigned"
    ONLINE = "online"
    OFFLINE = "offline"


class CommandType(str, enum.Enum):
    UPDATE_SCHEDULE = "update_schedule"
    MANUAL_DISPENSE = "manual_dispense"
    RESTART = "restart"
    SYNC = "sync"
    CONFIGURE = "configure"


class CommandStatus(str, enum.Enum):
    QUEUED = "queued"
    SENT = "sent"
    ACKED = "acked"
    FAILED = "failed"


class NotificationType(str, enum.Enum):
    MISSED_DOSE = "missed_dose"
    DEVICE_OFFLINE = "device_offline"
    LOW_BATTERY = "low_battery"
    TRAY_EMPTY = "tray_empty"
    SCHEDULE_UPDATED = "schedule_updated"
    GENERAL = "general"


# ---------------------------------------------------------------------------
# Association table: which medications are bundled into a compartment schedule
# ---------------------------------------------------------------------------

schedule_medication = Table(
    "schedule_medication",
    Base.metadata,
    Column("schedule_id", String, ForeignKey("schedules.id"), primary_key=True),
    Column("medication_id", String, ForeignKey("medications.id"), primary_key=True),
    Column("dose_note", String, nullable=True),  # e.g. "2 tablets"
)


# ---------------------------------------------------------------------------
# Core actors
# ---------------------------------------------------------------------------

class Caregiver(Base):
    __tablename__ = "caregivers"

    id = Column(String, primary_key=True, default=gen_id)
    full_name = Column(String, nullable=False)
    email = Column(String, unique=True, index=True, nullable=False)
    phone = Column(String, nullable=True)
    hashed_password = Column(String, nullable=False)
    created_at = Column(DateTime, default=datetime.utcnow)

    patients = relationship("Patient", back_populates="caregiver", cascade="all, delete-orphan")
    knowledge_documents = relationship("KnowledgeDocument", back_populates="caregiver", cascade="all, delete-orphan")


class Patient(Base):
    __tablename__ = "patients"

    id = Column(String, primary_key=True, default=gen_id)
    caregiver_id = Column(String, ForeignKey("caregivers.id"), nullable=False)
    full_name = Column(String, nullable=False)
    date_of_birth = Column(String, nullable=True)  # ISO date string
    phone = Column(String, nullable=True)
    email = Column(String, unique=True, index=True, nullable=True)
    hashed_password = Column(String, nullable=True)  # patient login (read-only account)
    notes = Column(Text, nullable=True)
    timezone = Column(String, default="UTC")
    created_at = Column(DateTime, default=datetime.utcnow)

    caregiver = relationship("Caregiver", back_populates="patients")
    device = relationship("Device", back_populates="patient", uselist=False)
    medications = relationship("Medication", back_populates="patient", cascade="all, delete-orphan")
    schedules = relationship("Schedule", back_populates="patient", cascade="all, delete-orphan")
    voice_interactions = relationship("VoiceInteraction", back_populates="patient", cascade="all, delete-orphan")
    notifications = relationship("Notification", back_populates="patient", cascade="all, delete-orphan")


class Device(Base):
    __tablename__ = "devices"

    id = Column(String, primary_key=True, default=gen_id)
    device_uid = Column(String, unique=True, index=True, nullable=False)  # e.g. printed serial / QR code
    device_secret = Column(String, nullable=False, default=gen_secret)  # shared secret for HTTPS + WS auth
    caregiver_id = Column(String, ForeignKey("caregivers.id"), nullable=False)
    patient_id = Column(String, ForeignKey("patients.id"), unique=True, nullable=True)

    status = Column(Enum(DeviceStatus), default=DeviceStatus.UNASSIGNED)
    last_seen_at = Column(DateTime, nullable=True)
    firmware_version = Column(String, nullable=True)
    wifi_ssid = Column(String, nullable=True)
    battery_level = Column(Float, nullable=True)
    uptime_seconds = Column(Integer, nullable=True)

    created_at = Column(DateTime, default=datetime.utcnow)

    patient = relationship("Patient", back_populates="device")
    telemetry = relationship("Telemetry", back_populates="device", cascade="all, delete-orphan")
    dispense_events = relationship("DispenseEvent", back_populates="device", cascade="all, delete-orphan")
    commands = relationship("DeviceCommand", back_populates="device", cascade="all, delete-orphan")


# ---------------------------------------------------------------------------
# Medication & scheduling
# ---------------------------------------------------------------------------

class Medication(Base):
    __tablename__ = "medications"

    id = Column(String, primary_key=True, default=gen_id)
    patient_id = Column(String, ForeignKey("patients.id"), nullable=False)
    name = Column(String, nullable=False)
    dosage = Column(String, nullable=True)  # "500mg"
    form = Column(String, nullable=True)    # tablet, capsule, liquid...
    instructions = Column(Text, nullable=True)  # "take with food"
    prescribing_doctor = Column(String, nullable=True)
    active = Column(Boolean, default=True)
    created_at = Column(DateTime, default=datetime.utcnow)

    patient = relationship("Patient", back_populates="medications")
    schedules = relationship("Schedule", secondary=schedule_medication, back_populates="medications")


class Schedule(Base):
    """
    One row = one compartment (A-G) assignment for a patient: what's bundled
    into it, what time(s) it dispenses, and how often.
    """
    __tablename__ = "schedules"

    id = Column(String, primary_key=True, default=gen_id)
    patient_id = Column(String, ForeignKey("patients.id"), nullable=False)
    created_by_caregiver_id = Column(String, ForeignKey("caregivers.id"), nullable=False)

    compartment = Column(Enum(Compartment), nullable=False)
    dispense_time = Column(String, nullable=False)  # "HH:MM" 24h, patient-local time
    frequency = Column(Enum(Frequency), default=Frequency.DAILY)
    days_of_week = Column(JSON, nullable=True)  # e.g. ["mon","wed","fri"] when SPECIFIC_DAYS
    start_date = Column(String, nullable=True)  # ISO date
    end_date = Column(String, nullable=True)    # ISO date, nullable = indefinite
    active = Column(Boolean, default=True)

    created_at = Column(DateTime, default=datetime.utcnow)
    updated_at = Column(DateTime, default=datetime.utcnow, onupdate=datetime.utcnow)

    patient = relationship("Patient", back_populates="schedules")
    medications = relationship("Medication", secondary=schedule_medication, back_populates="schedules")
    dispense_events = relationship("DispenseEvent", back_populates="schedule")


# ---------------------------------------------------------------------------
# Device activity: dispensing, adherence video, telemetry, commands
# ---------------------------------------------------------------------------

class DispenseEvent(Base):
    __tablename__ = "dispense_events"

    id = Column(String, primary_key=True, default=gen_id)
    device_id = Column(String, ForeignKey("devices.id"), nullable=False)
    patient_id = Column(String, ForeignKey("patients.id"), nullable=False)
    schedule_id = Column(String, ForeignKey("schedules.id"), nullable=True)

    compartment = Column(Enum(Compartment), nullable=False)
    status = Column(Enum(DispenseStatus), default=DispenseStatus.SUCCESS)
    scheduled_time = Column(String, nullable=True)   # what time it was supposed to fire
    dispensed_at = Column(DateTime, nullable=False)  # device-reported timestamp
    received_at = Column(DateTime, default=datetime.utcnow)  # server receipt time
    was_offline_cached = Column(Boolean, default=False)  # synced later from device cache
    raw_payload = Column(JSON, nullable=True)

    device = relationship("Device", back_populates="dispense_events")
    schedule = relationship("Schedule", back_populates="dispense_events")
    adherence_video = relationship("AdherenceVideo", back_populates="dispense_event", uselist=False,
                                    cascade="all, delete-orphan")


class AdherenceVideo(Base):
    __tablename__ = "adherence_videos"

    id = Column(String, primary_key=True, default=gen_id)
    dispense_event_id = Column(String, ForeignKey("dispense_events.id"), unique=True, nullable=False)
    file_path = Column(String, nullable=False)
    duration_seconds = Column(Integer, default=120)
    person_detected = Column(Boolean, nullable=True)
    uploaded_at = Column(DateTime, default=datetime.utcnow)

    dispense_event = relationship("DispenseEvent", back_populates="adherence_video")


class Telemetry(Base):
    __tablename__ = "telemetry"

    id = Column(String, primary_key=True, default=gen_id)
    device_id = Column(String, ForeignKey("devices.id"), nullable=False)
    reported_at = Column(DateTime, default=datetime.utcnow)

    current_compartment = Column(String, nullable=True)
    motor_status = Column(String, nullable=True)
    sensor_status = Column(String, nullable=True)
    person_detected = Column(Boolean, nullable=True)
    tray_state = Column(String, nullable=True)  # e.g. "full", "low", "empty"
    battery_level = Column(Float, nullable=True)
    wifi_rssi = Column(Integer, nullable=True)
    uptime_seconds = Column(Integer, nullable=True)
    dispense_history = Column(JSON, nullable=True)  # device's local recent-history snapshot
    raw_payload = Column(JSON, nullable=True)

    device = relationship("Device", back_populates="telemetry")


class DeviceCommand(Base):
    __tablename__ = "device_commands"

    id = Column(String, primary_key=True, default=gen_id)
    device_id = Column(String, ForeignKey("devices.id"), nullable=False)
    command_type = Column(Enum(CommandType), nullable=False)
    payload = Column(JSON, nullable=True)
    status = Column(Enum(CommandStatus), default=CommandStatus.QUEUED)
    created_at = Column(DateTime, default=datetime.utcnow)
    sent_at = Column(DateTime, nullable=True)
    acked_at = Column(DateTime, nullable=True)

    device = relationship("Device", back_populates="commands")


# ---------------------------------------------------------------------------
# AI / voice
# ---------------------------------------------------------------------------

class VoiceInteraction(Base):
    __tablename__ = "voice_interactions"

    id = Column(String, primary_key=True, default=gen_id)
    patient_id = Column(String, ForeignKey("patients.id"), nullable=False)
    device_id = Column(String, ForeignKey("devices.id"), nullable=True)

    transcript = Column(Text, nullable=True)
    answer_text = Column(Text, nullable=True)
    citations = Column(JSON, nullable=True)
    tool_results = Column(JSON, nullable=True)
    response_audio_path = Column(String, nullable=True)
    audio_format = Column(String, nullable=True)
    created_at = Column(DateTime, default=datetime.utcnow)

    patient = relationship("Patient", back_populates="voice_interactions")


class KnowledgeDocument(Base):
    __tablename__ = "knowledge_documents"

    id = Column(String, primary_key=True, default=gen_id)
    caregiver_id = Column(String, ForeignKey("caregivers.id"), nullable=False)
    filename = Column(String, nullable=False)
    file_path = Column(String, nullable=False)
    ingest_status = Column(String, default="pending")  # pending|ingested|failed
    ingest_response = Column(JSON, nullable=True)
    uploaded_at = Column(DateTime, default=datetime.utcnow)

    caregiver = relationship("Caregiver", back_populates="knowledge_documents")


class Notification(Base):
    __tablename__ = "notifications"

    id = Column(String, primary_key=True, default=gen_id)
    patient_id = Column(String, ForeignKey("patients.id"), nullable=False)
    caregiver_id = Column(String, ForeignKey("caregivers.id"), nullable=True)
    type = Column(Enum(NotificationType), default=NotificationType.GENERAL)
    message = Column(String, nullable=False)
    read = Column(Boolean, default=False)
    created_at = Column(DateTime, default=datetime.utcnow)

    patient = relationship("Patient", back_populates="notifications")
