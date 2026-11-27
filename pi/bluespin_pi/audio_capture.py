"""PipeWire / PulseAudio 48 kHz stereo PCM audio capture for BlueSpin Pi."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
import logging
import shutil

import numpy as np

from .config import AppStatus, BlueSpinConfig

logger = logging.getLogger(__name__)


def compute_rms_percent(pcm_s16le: bytes) -> float:
    """Return normalized RMS audio level (0.0 .. 100.0) from interleaved s16le PCM."""
    if not pcm_s16le:
        return 0.0
    samples = np.frombuffer(pcm_s16le, dtype=np.int16)
    if samples.size == 0:
        return 0.0
    rms = float(np.sqrt(np.mean(samples.astype(np.float32) ** 2)))
    return min(100.0, (rms / 32768.0) * 100.0 * 2.5)


class AudioCaptureService:
    """Captures 48 kHz 16-bit stereo PCM in exact 20 ms chunks from PipeWire or PulseAudio."""

    def __init__(
        self,
        config: BlueSpinConfig,
        status: AppStatus,
        on_chunk: Callable[[bytes, int], Awaitable[None]],
    ) -> None:
        self._config = config
        self._status = status
        self._on_chunk = on_chunk
        self._chunk_bytes = (
            config.sample_rate * config.chunk_ms // 1000
        ) * config.channels * (config.bit_depth // 8)
        self._proc: asyncio.subprocess.Process | None = None
        self._task: asyncio.Task[None] | None = None
        self._running = False

    @property
    def chunk_bytes(self) -> int:
        return self._chunk_bytes

    def _build_command(self, target: str) -> list[str]:
        backend = self._config.capture_backend
        if backend == "auto":
            if shutil.which("pw-record"):
                backend = "pipewire"
            elif shutil.which("parec"):
                backend = "pulse"
            else:
                raise RuntimeError("Neither pw-record (PipeWire) nor parec (PulseAudio) is installed")

        if backend == "pipewire":
            cmd = [
                "pw-record",
                "--raw",
                "--rate",
                str(self._config.sample_rate),
                "--channels",
                str(self._config.channels),
                "--format",
                "s16",
            ]
            if target:
                cmd.extend(["--target", target])
            cmd.append("-")
            return cmd

        cmd = [
            "parec",
            "--raw",
            "--rate",
            str(self._config.sample_rate),
            "--channels",
            str(self._config.channels),
            "--format",
            "s16le",
            "--latency-msec",
            str(self._config.chunk_ms),
        ]
        if target:
            cmd.extend(["--device", target])
        return cmd

    async def _discover_bluez_target(self) -> str:
        """Find an active bluez_input / bluez_source device if not explicitly configured."""
        if self._config.capture_device:
            return self._config.capture_device
        if shutil.which("pactl"):
            proc = await asyncio.create_subprocess_exec(
                "pactl",
                "list",
                "short",
                "sources",
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
            stdout, _ = await proc.communicate()
            for line in stdout.decode(errors="ignore").splitlines():
                parts = line.split("\t")
                if len(parts) >= 2 and "bluez" in parts[1]:
                    return parts[1]
        return ""

    async def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._task = asyncio.create_task(self._capture_loop(), name="audio-capture")

    async def stop(self) -> None:
        self._running = False
        if self._proc is not None and self._proc.returncode is None:
            self._proc.terminate()
            try:
                await asyncio.wait_for(self._proc.wait(), timeout=2.0)
            except TimeoutError:
                self._proc.kill()
        if self._task is not None:
            self._task.cancel()
            self._task = None
        self._status.audio_level_pct = 0.0

    async def _capture_loop(self) -> None:
        silence = bytes(self._chunk_bytes)
        while self._running:
            if not self._status.bt_connected:
                self._status.audio_level_pct = 0.0
                await self._on_chunk(silence, 0)
                await asyncio.sleep(self._config.chunk_ms / 1000.0)
                continue

            try:
                target = await self._discover_bluez_target()
                cmd = self._build_command(target)
                logger.info("Starting audio capture: %s", " ".join(cmd))
                self._proc = await asyncio.create_subprocess_exec(
                    *cmd,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.DEVNULL,
                )
                assert self._proc.stdout is not None
                while self._running and self._proc.returncode is None:
                    data = await self._proc.stdout.readexactly(self._chunk_bytes)
                    self._status.audio_level_pct = compute_rms_percent(data)
                    await self._on_chunk(data, 0)
            except asyncio.IncompleteReadError:
                logger.debug("Audio capture EOF; restarting")
            except asyncio.CancelledError:
                break
            except Exception:
                logger.warning("Audio capture error; retrying in 1s", exc_info=True)
                await asyncio.sleep(1.0)
            finally:
                if self._proc is not None and self._proc.returncode is None:
                    self._proc.kill()
                self._proc = None
