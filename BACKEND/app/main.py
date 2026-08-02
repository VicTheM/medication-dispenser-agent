from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app import models
from app.database import Base, engine
from app.routers import ai, auth, caregivers, devices, patients
from app.scheduler import start_scheduler, stop_scheduler

Base.metadata.create_all(bind=engine)  # SQLite file created/migrated on boot - no separate DB server needed


@asynccontextmanager
async def lifespan(_: FastAPI):
    start_scheduler()  # missed-dose + device-health background checks
    yield
    stop_scheduler()


app = FastAPI(
    title="MedAdhere API",
    description="Medication scheduling & adherence backend: caregivers, patients, "
                 "the 7-compartment dispenser hardware, and the Ally Healthwise AI assistant.",
    version="1.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # tighten to your app's origin(s) in production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(auth.router)
app.include_router(caregivers.router)
app.include_router(patients.router)
app.include_router(devices.router)
app.include_router(ai.router)


@app.get("/", tags=["System"])
def root():
    return {
        "name": "MedAdhere API",
        "status": "ok",
        "docs": "/docs",
    }


@app.get("/healthz", tags=["System"])
def healthz():
    return {"status": "ok"}
