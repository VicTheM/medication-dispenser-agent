from datetime import datetime
from typing import List, Optional

from pydantic import BaseModel, EmailStr, Field, HttpUrl
from enum import Enum


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------


class Token(BaseModel):
    access_token: str
    token_type: str = "bearer"
    role: str


class CaregiverCreate(BaseModel):
    full_name: str
    email: EmailStr
    phone: Optional[str] = None
    password: str = Field(min_length=8)


class CaregiverOut(BaseModel):
    id: str
    full_name: str
    email: EmailStr
    phone: Optional[str] = None
    created_at: datetime

    class Config:
        from_attributes = True


class LoginRequest(BaseModel):
    email: EmailStr
    password: str


# ---------------------------------------------------------------------------
# Patient
# ---------------------------------------------------------------------------


class PatientCreate(BaseModel):
    full_name: str
    date_of_birth: Optional[str] = None
    phone: Optional[str] = None
    email: Optional[EmailStr] = None
    password: Optional[str] = Field(
        default=None,
        min_length=6,
        description="Set to let the patient log in themselves; optional.",
    )
    notes: Optional[str] = None
    timezone: Optional[str] = "UTC"


class PatientUpdate(BaseModel):
    full_name: Optional[str] = None
    phone: Optional[str] = None
    notes: Optional[str] = None
    timezone: Optional[str] = None


class PatientOut(BaseModel):
    id: str
    caregiver_id: str
    full_name: str
    date_of_birth: Optional[str] = None
    phone: Optional[str] = None
    email: Optional[EmailStr] = None
    notes: Optional[str] = None
    timezone: str
    created_at: datetime

    class Config:
        from_attributes = True


# ---------------------------------------------------------------------------
# Device
# ---------------------------------------------------------------------------


class DeviceRegister(BaseModel):
    device_uid: str = Field(
        description="Serial number / QR code printed on the hardware unit"
    )


class DeviceOut(BaseModel):
    id: str
    device_uid: str
    device_secret: str
    patient_id: Optional[str] = None
    status: str
    last_seen_at: Optional[datetime] = None
    firmware_version: Optional[str] = None
    battery_level: Optional[float] = None

    class Config:
        from_attributes = True


class DeviceStatusOut(BaseModel):
    id: str
    device_uid: str
    patient_id: Optional[str] = None
    status: str
    last_seen_at: Optional[datetime] = None
    firmware_version: Optional[str] = None
    wifi_ssid: Optional[str] = None
    battery_level: Optional[float] = None
    uptime_seconds: Optional[int] = None

    class Config:
        from_attributes = True


class DeviceSecretOut(BaseModel):
    device_uid: str
    device_secret: str


class DeviceCommandRequest(BaseModel):
    command_type: str = Field(
        description="update_schedule|manual_dispense|restart|sync|configure"
    )
    payload: Optional[dict] = None


# ---------------------------------------------------------------------------
# Medication
# ---------------------------------------------------------------------------


class MedicationCreate(BaseModel):
    name: str
    dosage: Optional[str] = None
    form: Optional[str] = None
    instructions: Optional[str] = None
    prescribing_doctor: Optional[str] = None


class MedicationUpdate(BaseModel):
    name: Optional[str] = None
    dosage: Optional[str] = None
    form: Optional[str] = None
    instructions: Optional[str] = None
    prescribing_doctor: Optional[str] = None
    active: Optional[bool] = None


class MedicationOut(BaseModel):
    id: str
    patient_id: str
    name: str
    dosage: Optional[str] = None
    form: Optional[str] = None
    instructions: Optional[str] = None
    prescribing_doctor: Optional[str] = None
    active: bool
    created_at: datetime

    class Config:
        from_attributes = True


# ---------------------------------------------------------------------------
# Schedule (compartment assignment)
# ---------------------------------------------------------------------------


class ScheduleCreate(BaseModel):
    compartment: str = Field(description="One letter A-G")
    medication_ids: List[str]
    dispense_time: str = Field(description="24h HH:MM in the patient's local timezone")
    frequency: str = Field(default="daily", description="daily|specific_days|as_needed")
    days_of_week: Optional[List[str]] = Field(
        default=None,
        description="Required when frequency=specific_days, e.g. ['mon','wed','fri']",
    )
    start_date: Optional[str] = None
    end_date: Optional[str] = None


class ScheduleUpdate(BaseModel):
    medication_ids: Optional[List[str]] = None
    dispense_time: Optional[str] = None
    frequency: Optional[str] = None
    days_of_week: Optional[List[str]] = None
    start_date: Optional[str] = None
    end_date: Optional[str] = None
    active: Optional[bool] = None


class ScheduleOut(BaseModel):
    id: str
    patient_id: str
    compartment: str
    dispense_time: str
    frequency: str
    days_of_week: Optional[List[str]] = None
    start_date: Optional[str] = None
    end_date: Optional[str] = None
    active: bool
    medication_ids: List[str]
    medication_names: List[str]
    updated_at: datetime

    class Config:
        from_attributes = True


# ---------------------------------------------------------------------------
# Dispense / adherence / telemetry
# ---------------------------------------------------------------------------


class DispenseEventIn(BaseModel):
    compartment: str
    status: str = Field(default="success", description="success|failed|skipped|manual")
    scheduled_time: Optional[str] = None
    dispensed_at: datetime
    was_offline_cached: bool = False


class DispenseEventOut(BaseModel):
    id: str
    device_id: str
    patient_id: str
    schedule_id: Optional[str] = None
    compartment: str
    status: str
    scheduled_time: Optional[str] = None
    dispensed_at: datetime
    received_at: datetime
    was_offline_cached: bool
    has_video: bool = False

    class Config:
        from_attributes = True


class OfflineSyncBatch(BaseModel):
    dispense_events: List[DispenseEventIn] = []
    telemetry: List["TelemetryIn"] = []


class TelemetryIn(BaseModel):
    current_compartment: Optional[str] = None
    motor_status: Optional[str] = None
    sensor_status: Optional[str] = None
    person_detected: Optional[bool] = None
    tray_state: Optional[str] = None
    battery_level: Optional[float] = None
    wifi_rssi: Optional[int] = None
    uptime_seconds: Optional[int] = None
    dispense_history: Optional[list] = None
    reported_at: Optional[datetime] = None


class TelemetryOut(BaseModel):
    id: str
    device_id: str
    reported_at: datetime
    current_compartment: Optional[str] = None
    motor_status: Optional[str] = None
    sensor_status: Optional[str] = None
    person_detected: Optional[bool] = None
    tray_state: Optional[str] = None
    battery_level: Optional[float] = None
    wifi_rssi: Optional[int] = None
    uptime_seconds: Optional[int] = None

    class Config:
        from_attributes = True


class AdherenceVideoOut(BaseModel):
    id: str
    dispense_event_id: str
    file_path: str
    duration_seconds: int
    person_detected: Optional[bool] = None
    uploaded_at: datetime

    class Config:
        from_attributes = True


# ---------------------------------------------------------------------------
# AI / voice
# ---------------------------------------------------------------------------


class AskRequest(BaseModel):
    question: str


class VoiceInteractionOut(BaseModel):
    id: str
    patient_id: str
    device_id: Optional[str] = None
    transcript: Optional[str] = None
    answer_text: Optional[str] = None
    response_audio_path: Optional[str] = None
    citations: Optional[list] = None
    tool_results: Optional[list] = None
    audio_format: Optional[str] = None
    created_at: datetime

    class Config:
        from_attributes = True


class KnowledgeDocumentOut(BaseModel):
    id: str
    filename: str
    ingest_status: str
    uploaded_at: datetime

    class Config:
        from_attributes = True


# ---------------------------------------------------------------------------
# Notifications
# ---------------------------------------------------------------------------


class NotificationOut(BaseModel):
    id: str
    patient_id: str
    type: str
    message: str
    read: bool
    created_at: datetime

    class Config:
        from_attributes = True


class UploadType(str, Enum):
    VIDEO = "video"
    AUDIO = "audio"

class UploadCreate(BaseModel):
    upload_type: UploadType
    url: HttpUrl

class UploadResponse(BaseModel):
    id: str
    type: UploadType
    url: str
    created_at: datetime

    class Config:
        from_attributes = True
