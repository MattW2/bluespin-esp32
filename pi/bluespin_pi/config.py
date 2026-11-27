"""Configuration and runtime state models for BlueSpin Pi."""

from __future__ import annotations

from dataclasses import dataclass, field
import os
from pathlib import Path
import socket


@dataclass(slots=True)
class BlueSpinConfig:
    """Runtime configuration loaded from environment variables / defaults."""

    friendly_name: str = "BlueSpin Pi"
    sendspin_port: int = 8928
    web_port: int = 8080
    data_dir: Path = Path("/var/lib/bluespin")

    # Audio / Sendspin source settings
    sample_rate: int = 48000
    channels: int = 2
    bit_depth: int = 16
    chunk_ms: int = 20
    codec: str = "opus"  # opus | flac | pcm
    capture_backend: str = "auto"  # auto | pipewire | pulse
    capture_device: str = ""  # empty = auto-detect bluez_input / default monitor

    # Bluetooth settings
    pairing_window_s: int = 120
    max_bonded_devices: int = 8
    takeover: bool = True
    allow_device_connect: bool = True

    # Sendspin pairing
    unpaired_access: bool = False

    # Optional SSD1306 I2C OLED
    oled_enabled: bool = True
    oled_i2c_port: int = 1
    oled_i2c_address: int = 0x3C

    @classmethod
    def from_env(cls) -> BlueSpinConfig:
        """Build configuration from BLUESPIN_* environment variables."""
        default_name = f"BlueSpin {socket.gethostname()}"
        return cls(
            friendly_name=os.environ.get("BLUESPIN_NAME", default_name),
            sendspin_port=int(os.environ.get("BLUESPIN_SENDSPIN_PORT", "8928")),
            web_port=int(os.environ.get("BLUESPIN_WEB_PORT", "8080")),
            data_dir=Path(os.environ.get("BLUESPIN_DATA_DIR", "/var/lib/bluespin")),
            codec=os.environ.get("BLUESPIN_CODEC", "opus").lower(),
            capture_backend=os.environ.get("BLUESPIN_CAPTURE_BACKEND", "auto").lower(),
            capture_device=os.environ.get("BLUESPIN_CAPTURE_DEVICE", ""),
            pairing_window_s=int(os.environ.get("BLUESPIN_PAIRING_WINDOW_S", "120")),
            max_bonded_devices=int(os.environ.get("BLUESPIN_MAX_BONDED_DEVICES", "8")),
            takeover=os.environ.get("BLUESPIN_TAKEOVER", "true").lower() in ("1", "true", "yes"),
            allow_device_connect=os.environ.get("BLUESPIN_ALLOW_CONNECT", "true").lower()
            in ("1", "true", "yes"),
            unpaired_access=os.environ.get("BLUESPIN_UNPAIRED_ACCESS", "false").lower()
            in ("1", "true", "yes"),
            oled_enabled=os.environ.get("BLUESPIN_OLED_ENABLED", "true").lower()
            in ("1", "true", "yes"),
            oled_i2c_port=int(os.environ.get("BLUESPIN_OLED_I2C_PORT", "1")),
            oled_i2c_address=int(os.environ.get("BLUESPIN_OLED_I2C_ADDR", "0x3C"), 0),
        )


@dataclass(slots=True)
class AppStatus:
    """Shared live status snapshot for Web UI and OLED display."""

    friendly_name: str = "BlueSpin Pi"
    ip_address: str = "No Net"
    bt_connected: bool = False
    bt_streaming: bool = False
    bt_pairing_open: bool = False
    bt_allow_connect: bool = True
    bt_device_name: str = "None"
    bt_device_mac: str = ""
    bt_codec: str = "Idle"
    bt_volume_pct: int = 75
    bt_paired_count: int = 0
    ss_running: bool = False
    ss_streaming: bool = False
    ss_client_id: str = ""
    ss_pairing_code: str | None = None
    ss_unpaired_access: bool = False
    audio_level_pct: float = 0.0
    extra: dict[str, str] = field(default_factory=dict)
