from sqlalchemy.orm import Session

from app import models


def build_device_schedule_payload(db: Session, patient) -> dict:
    """
    Builds the full A-G compartment schedule for a patient's device, in the
    shape the firmware expects to store locally and act on autonomously.
    """
    schedules = (
        db.query(models.Schedule)
        .filter(models.Schedule.patient_id == patient.id, models.Schedule.active.is_(True))
        .all()
    )

    compartments = {letter: None for letter in "ABCDEFG"}
    for s in schedules:
        compartments[s.compartment.value] = {
            "schedule_id": s.id,
            "dispense_time": s.dispense_time,
            "frequency": s.frequency.value,
            "days_of_week": s.days_of_week,
            "start_date": s.start_date,
            "end_date": s.end_date,
            "medications": [
                {"id": m.id, "name": m.name, "dosage": m.dosage}
                for m in s.medications
            ],
        }

    return {
        "type": "update_schedule",
        "patient_id": patient.id,
        "timezone": patient.timezone,
        "compartments": compartments,
    }


def schedule_to_out(schedule: models.Schedule) -> dict:
    return {
        "id": schedule.id,
        "patient_id": schedule.patient_id,
        "compartment": schedule.compartment.value,
        "dispense_time": schedule.dispense_time,
        "frequency": schedule.frequency.value,
        "days_of_week": schedule.days_of_week,
        "start_date": schedule.start_date,
        "end_date": schedule.end_date,
        "active": schedule.active,
        "medication_ids": [m.id for m in schedule.medications],
        "medication_names": [m.name for m in schedule.medications],
        "updated_at": schedule.updated_at,
    }
