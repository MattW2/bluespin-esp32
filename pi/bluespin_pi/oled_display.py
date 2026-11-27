"""Optional SSD1306 128x64 I2C OLED status display for Raspberry Pi 3B."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from PIL import Image, ImageDraw, ImageFont

from .config import AppStatus, BlueSpinConfig

logger = logging.getLogger(__name__)

try:
    from luma.core.interface.serial import i2c as luma_i2c
    from luma.oled.device import ssd1306

    LUMA_AVAILABLE = True
except ImportError:  # pragma: no cover - non-Linux dev environments
    LUMA_AVAILABLE = False
    luma_i2c = None  # type: ignore[assignment]
    ssd1306 = None  # type: ignore[assignment]


def _load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def render_status_image(status: AppStatus, width: int = 128, height: int = 64) -> Image.Image:
    """Render the 128x64 1-bit status image matching BlueSpin-esp32's OLED layout."""
    img = Image.new("1", (width, height), 0)
    draw = ImageDraw.Draw(img)
    font_sm = _load_font(10)
    font_md = _load_font(12)
    font_lg = _load_font(22)

    if status.ss_pairing_code:
        draw.text((64, 4), "MA PAIRING CODE", font=font_sm, fill=255, anchor="mt")
        draw.line((8, 17, 120, 17), fill=255)
        draw.text((64, 24), status.ss_pairing_code, font=font_lg, fill=255, anchor="mt")
        draw.text((64, 52), "Enter in Music Assistant", font=font_sm, fill=255, anchor="mt")
        return img

    if status.bt_pairing_open:
        draw.text((64, 4), "BT PAIRING OPEN", font=font_md, fill=255, anchor="mt")
        draw.line((8, 20, 120, 20), fill=255)
        draw.text((64, 26), status.friendly_name[:20], font=font_sm, fill=255, anchor="mt")
        draw.text((64, 44), "Pinless (Just Works)", font=font_sm, fill=255, anchor="mt")
        return img

    # Top bar: title + IP
    draw.text((0, 0), "BlueSpin", font=font_sm, fill=255)
    draw.text((127, 0), status.ip_address, font=font_sm, fill=255, anchor="rt")
    draw.line((0, 13, 127, 13), fill=255)

    # Middle: BT device + playback/volume
    if status.bt_connected:
        dev = status.bt_device_name
        if len(dev) > 19:
            dev = dev[:16] + "..."
        draw.text((0, 16), f"BT: {dev}", font=font_md, fill=255)
        play_lbl = "PLAY" if status.bt_streaming else "IDLE"
        draw.text((0, 32), f"{play_lbl}  Vol: {status.bt_volume_pct}%", font=font_sm, fill=255)
    else:
        draw.text((0, 16), "BT: Waiting...", font=font_md, fill=255)
        draw.text((0, 32), "Ready to connect", font=font_sm, fill=255)

    # Bottom: Codec + Sendspin status + audio level bar
    ss_lbl = "SS:INIT" if not status.ss_running else ("SS:LIVE" if status.ss_streaming else "SS:IDLE")
    codec = status.bt_codec[:11] if status.bt_connected else "Idle"
    draw.text((0, 45), codec, font=font_sm, fill=255)
    draw.text((127, 45), ss_lbl, font=font_sm, fill=255, anchor="rt")

    fill = max(0.0, min(100.0, status.audio_level_pct))
    draw.rectangle((0, 58, 127, 63), outline=255, fill=0)
    bar_w = int((fill / 100.0) * 124.0)
    if bar_w > 0:
        draw.rectangle((2, 60, 2 + bar_w - 1, 61), fill=255)

    return img


class OledDisplay:
    """Async loop driving an optional SSD1306 128x64 I2C display."""

    def __init__(self, config: BlueSpinConfig, status: AppStatus) -> None:
        self._config = config
        self._status = status
        self._device: Any = None
        self._task: asyncio.Task[None] | None = None
        self._running = False

    async def start(self) -> None:
        if not self._config.oled_enabled:
            logger.info("OLED display disabled by configuration")
            return
        if not LUMA_AVAILABLE:
            logger.info("luma.oled not available; OLED display disabled")
            return
        try:
            serial = luma_i2c(
                port=self._config.oled_i2c_port,
                address=self._config.oled_i2c_address,
            )
            self._device = ssd1306(serial, width=128, height=64)
            self._running = True
            self._task = asyncio.create_task(self._render_loop(), name="oled-render")
            logger.info(
                "SSD1306 OLED initialized on I2C bus %d (0x%02X)",
                self._config.oled_i2c_port,
                self._config.oled_i2c_address,
            )
        except Exception as exc:
            logger.info("No SSD1306 OLED detected on I2C (0x%02X): %s", self._config.oled_i2c_address, exc)
            self._device = None

    async def stop(self) -> None:
        self._running = False
        if self._task is not None:
            self._task.cancel()
            self._task = None
        if self._device is not None:
            self._device.clear()

    async def _render_loop(self) -> None:
        while self._running and self._device is not None:
            try:
                img = render_status_image(self._status)
                self._device.display(img)
            except Exception:
                logger.debug("OLED render failed", exc_info=True)
            await asyncio.sleep(0.5)
