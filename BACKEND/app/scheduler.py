"""
Background jobs that turn raw data (schedules, dispense events, telemetry)
into actionable Notification rows:

  - missed dose:      a schedule's dispense_time has passed (with a grace
                       window) today and no matching dispense event exists
  - device offline:   a device that was online has gone quiet past the
                       configured grace period
  - low battery:       device-reported battery has dropped under a threshold

Runs inside the same process via APScheduler - no separate worker/queue
needed, which keeps this a single deployable unit alongside the SQLite file.
"""
import logging
from datetime import datetime, timedelta, date
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from apscheduler.schedulers.asyncio import AsyncIOScheduler
from sqlalchemy.orm import Session

from app import models
from app.config import settings
from app.database import SessionLocal

logger = logging.getLogger("medadhere.scheduler")

MISSED_DOSE_GRACE_MINUTES = 30
LOW_BATTERY_THRESHOLD = 15.0
LOW_BATTERY_RENOTIFY_HOURS = 12
DAY_CODES = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]


def _patient_now(patient: models.Patient) -> datetime:
    try:
        tz = ZoneInfo(patient.timezone or "UTC")
    except ZoneInfoNotFoundError:
        tz = ZoneInfo("UTC")
    return datetime.now(tz)


def _already_notified_today(db: Session, patient_id: str, ntype: models.NotificationType, marker: str) -> bool:
    """Cheap de-dupe: look for a notification of this type mentioning `marker` created today."""
    today_start = datetime.utcnow().replace(hour=0, minute=0, second=0, microsecond=0)
    existing = (
        db.query(models.Notification)
        .filter(
            models.Notification.patient_id == patient_id,
            models.Notification.type == ntype,
            models.Notification.created_at >= today_start,
            models.Notification.message.contains(marker),
        )
        .first()
    )
    return existing is not None


def _notified_within(db: Session, patient_id: str, ntype: models.NotificationType, hours: int) -> bool:
    cutoff = datetime.utcnow() - timedelta(hours=hours)
    existing = (
        db.query(models.Notification)
        .filter(
            models.Notification.patient_id == patient_id,
            models.Notification.type == ntype,
            models.Notification.created_at >= cutoff,
        )
        .first()
    )
    return existing is not None


def check_missed_doses():
    db = SessionLocal()
    try:
        schedules = db.query(models.Schedule).filter(models.Schedule.active.is_(True)).all()
        for schedule in schedules:
            if schedule.frequency == models.Frequency.AS_NEEDED:
                continue

            patient = schedule.patient
            now_local = _patient_now(patient)
            today_str = now_local.date().isoformat()
            weekday_code = DAY_CODES[now_local.weekday()]

            if schedule.frequency == models.Frequency.SPECIFIC_DAYS:
                if not schedule.days_of_week or weekday_code not in schedule.days_of_week:
                    continue

            if schedule.start_date and today_str < schedule.start_date:
                continue
            if schedule.end_date and today_str > schedule.end_date:
                continue

            try:
                hh, mm = map(int, schedule.dispense_time.split(":"))
            except ValueError:
                continue
            scheduled_dt_local = now_local.replace(hour=hh, minute=mm, second=0, microsecond=0)
            grace_cutoff = scheduled_dt_local + timedelta(minutes=MISSED_DOSE_GRACE_MINUTES)
            if now_local < grace_cutoff:
                continue  # not late yet

            already_dispensed = (
                db.query(models.DispenseEvent)
                .filter(
                    models.DispenseEvent.schedule_id == schedule.id,
                    models.DispenseEvent.status.in_([models.DispenseStatus.SUCCESS, models.DispenseStatus.MANUAL]),
                    models.DispenseEvent.dispensed_at >= scheduled_dt_local.replace(tzinfo=None) - timedelta(hours=1),
                )
                .first()
            )
            if already_dispensed:
                continue

            marker = f"schedule:{schedule.id}:{today_str}"
            if _already_notified_today(db, patient.id, models.NotificationType.MISSED_DOSE, marker):
                continue

            med_names = ", ".join(m.name for m in schedule.medications) or "medication"
            note = models.Notification(
                patient_id=patient.id,
                caregiver_id=patient.caregiver_id,
                type=models.NotificationType.MISSED_DOSE,
                message=(f"Missed dose: compartment {schedule.compartment.value} ({med_names}) "
                         f"was due at {schedule.dispense_time} and hasn't been dispensed. [{marker}]"),
            )
            db.add(note)
        db.commit()
    except Exception:
        logger.exception("check_missed_doses failed")
        db.rollback()
    finally:
        db.close()


def check_device_health():
    db = SessionLocal()
    try:
        devices = db.query(models.Device).filter(models.Device.patient_id.isnot(None)).all()
        offline_cutoff = datetime.utcnow() - timedelta(seconds=settings.DEVICE_OFFLINE_AFTER_SECONDS)

        for device in devices:
            patient = device.patient
            if not patient:
                continue

            # Offline detection (transition-only, so we don't spam)
            stale = device.last_seen_at is None or device.last_seen_at < offline_cutoff
            if stale and device.status == models.DeviceStatus.ONLINE:
                device.status = models.DeviceStatus.OFFLINE
                db.add(models.Notification(
                    patient_id=patient.id,
                    caregiver_id=patient.caregiver_id,
                    type=models.NotificationType.DEVICE_OFFLINE,
                    message=f"Device {device.device_uid} has gone offline (no contact since "
                            f"{device.last_seen_at.isoformat() if device.last_seen_at else 'never'}).",
                ))

            # Low battery (re-notify at most every LOW_BATTERY_RENOTIFY_HOURS)
            if (device.battery_level is not None and device.battery_level < LOW_BATTERY_THRESHOLD
                    and not _notified_within(db, patient.id, models.NotificationType.LOW_BATTERY,
                                              LOW_BATTERY_RENOTIFY_HOURS)):
                db.add(models.Notification(
                    patient_id=patient.id,
                    caregiver_id=patient.caregiver_id,
                    type=models.NotificationType.LOW_BATTERY,
                    message=f"Device {device.device_uid} battery is low ({device.battery_level:.0f}%).",
                ))
        db.commit()
    except Exception:
        logger.exception("check_device_health failed")
        db.rollback()
    finally:
        db.close()


scheduler = AsyncIOScheduler()


def start_scheduler():
    scheduler.add_job(check_missed_doses, "interval", minutes=5, id="check_missed_doses", replace_existing=True)
    scheduler.add_job(check_device_health, "interval", minutes=1, id="check_device_health", replace_existing=True)
    scheduler.start()
    logger.info("Background scheduler started (missed-dose + device-health checks)")


def stop_scheduler():
    if scheduler.running:
        scheduler.shutdown(wait=False)
