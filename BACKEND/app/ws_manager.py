import json
from typing import Dict, Optional

from fastapi import WebSocket


class DeviceConnectionManager:
    """
    Keeps one persistent WebSocket per online device (keyed by device_uid),
    so the backend can push real-time commands (schedule updates, manual
    dispense, restart, sync, configure) at any time.
    """

    def __init__(self):
        self._connections: Dict[str, WebSocket] = {}

    async def connect(self, device_uid: str, websocket: WebSocket):
        await websocket.accept()
        self._connections[device_uid] = websocket

    def disconnect(self, device_uid: str):
        self._connections.pop(device_uid, None)

    def is_online(self, device_uid: str) -> bool:
        return device_uid in self._connections

    async def send_json(self, device_uid: str, data: dict) -> bool:
        ws = self._connections.get(device_uid)
        if not ws:
            return False
        try:
            await ws.send_text(json.dumps(data))
            return True
        except Exception:
            self.disconnect(device_uid)
            return False

    def get_connection(self, device_uid: str) -> Optional[WebSocket]:
        return self._connections.get(device_uid)


manager = DeviceConnectionManager()
