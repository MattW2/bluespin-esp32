"""Main application orchestrator for BlueSpin Pi."""

from __future__ import annotations

import asyncio
import logging
import socket

from .audio_capture import AudioCaptureService
from .bluetooth_manager import BluetoothManager
from .config import AppStatus, BlueSpinConfig
from .oled_display import OledDisplay
from .sendspin_bridge import SendspinBridge
from .web_ui import WebUI

logger = logging.getLogger(__name__)


def detect_local_ip() -> str:
    """Return the primary outbound IPv4 address without sending packets."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("1.1.1.1", 80))
            return str(sock.getsockname()[0])
    except OSError:
        return "No Net"


class BlueSpinApp:
    """Coordinates BlueZ A2DP Sink, PipeWire/PulseAudio capture, Sendspin, OLED, and Web UI."""

    def __init__(self, config: BlueSpinConfig) -> None:
        self.config = config
        self.status = AppStatus(
            friendly_name=config.friendly_name,
            ip_address=detect_local_ip(),
        )
        self.sendspin = SendspinBridge(config, self.status)
        self.bt = BluetoothManager(
            config,
            self.status,
            on_streaming_changed=self.sendspin.set_signal_present,
        )
        self.audio = AudioCaptureService(
            config,
            self.status,
            on_chunk=self.sendspin.feed_pcm,
        )
        self.oled = OledDisplay(config, self.status)
        self.web = WebUI(config, self.status, self.bt, self.sendspin)
        self._ip_task: asyncio.Task[None] | None = None
        self._running = False

    async def start(self) -> None:
        self._running = True
        await self.sendspin.start()
        await self.bt.start()
        await self.audio.start()
        await self.oled.start()
        await self.web.start()
        self._ip_task = asyncio.create_task(self._refresh_ip_loop(), name="ip-refresh")
        logger.info("BlueSpin Pi is running (%s)", self.status.friendly_name)

    async def stop(self) -> None:
        self._running = False
        if self._ip_task is not None:
            self._ip_task.cancel()
        await self.web.stop()
        await self.oled.stop()
        await self.audio.stop()
        await self.bt.stop()
        await self.sendspin.stop()
        logger.info("BlueSpin Pi stopped")

    async def _refresh_ip_loop(self) -> None:
        while self._running:
            self.status.ip_address = detect_local_ip()
            await asyncio.sleep(15.0)
