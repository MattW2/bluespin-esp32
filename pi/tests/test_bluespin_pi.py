"""Local unit tests for BlueSpin Pi (runnable on Windows/Linux without Pi hardware)."""

from __future__ import annotations

import asyncio
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bluespin_pi.audio_capture import compute_rms_percent
from bluespin_pi.config import AppStatus, BlueSpinConfig
from bluespin_pi.oled_display import render_status_image
from bluespin_pi.sendspin_bridge import load_or_create_identity


def test_config_from_env(monkeypatch) -> None:
    monkeypatch.setenv("BLUESPIN_NAME", "Kitchen Pi")
    monkeypatch.setenv("BLUESPIN_CODEC", "flac")
    monkeypatch.setenv("BLUESPIN_OLED_ENABLED", "false")
    cfg = BlueSpinConfig.from_env()
    assert cfg.friendly_name == "Kitchen Pi"
    assert cfg.codec == "flac"
    assert cfg.oled_enabled is False
    assert cfg.sample_rate == 48000


def test_compute_rms_percent() -> None:
    assert compute_rms_percent(b"") == 0.0
    silence = b"\x00" * 160
    assert compute_rms_percent(silence) == 0.0
    loud = struct.pack("<4h", 16000, -16000, 16000, -16000)
    assert compute_rms_percent(loud) > 50.0


def test_oled_render_all_modes() -> None:
    # Normal status
    st = AppStatus(
        friendly_name="Kitchen Pi",
        ip_address="192.168.1.50",
        bt_connected=True,
        bt_streaming=True,
        bt_device_name="Pixel 10 Pro",
        bt_codec="AAC",
        bt_volume_pct=65,
        ss_running=True,
        ss_streaming=True,
        audio_level_pct=42.0,
    )
    img = render_status_image(st)
    assert img.size == (128, 64)
    assert img.getbbox() is not None

    # BT pairing screen
    st.bt_pairing_open = True
    img_bt = render_status_image(st)
    assert img_bt.getbbox() is not None

    # MA pairing code priority screen
    st.ss_pairing_code = "123-456"
    img_ma = render_status_image(st)
    assert img_ma.getbbox() is not None


def test_identity_persistence(tmp_path: Path) -> None:
    id1 = load_or_create_identity(tmp_path)
    id2 = load_or_create_identity(tmp_path)
    assert id1.peer_id == id2.peer_id
    assert (tmp_path / "identity.key").exists()


def test_app_start_stop_and_web_status(tmp_path: Path) -> None:
    from bluespin_pi.app import BlueSpinApp

    cfg = BlueSpinConfig(
        friendly_name="Test Pi",
        sendspin_port=0,
        web_port=0,
        data_dir=tmp_path,
        oled_enabled=False,
    )
    app = BlueSpinApp(cfg)

    async def _exercise() -> None:
        await app.start()
        assert app.status.ss_running is True
        assert app.status.ss_client_id != ""
        await app.sendspin.set_unpaired_access(True)
        assert app.status.ss_unpaired_access is True
        await app.stop()
        assert app.status.ss_running is False

    asyncio.run(_exercise())
