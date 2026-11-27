"""aiosendspin source@v1 bridge for BlueSpin Pi."""

from __future__ import annotations

import asyncio
import contextlib
import logging
from pathlib import Path

from aiohttp import web
from aiosendspin.client import ClientListener, PairingSupport, SendspinClient, SourceCapture
from aiosendspin.models.core import ServerCommandPayload
from aiosendspin.models.player import SupportedAudioFormat
from aiosendspin.models.types import AudioCodec, GoodbyeReason, Roles, SignalState
from aiosendspin.noise import FileClientPairingStore, Identity, b64url_decode

from .config import AppStatus, BlueSpinConfig

logger = logging.getLogger(__name__)

_CODEC_MAP = {
    "opus": AudioCodec.OPUS,
    "flac": AudioCodec.FLAC,
    "pcm": AudioCodec.PCM,
}


def load_or_create_identity(data_dir: Path) -> Identity:
    """Load persistent X25519 identity from disk or generate a new one."""
    data_dir.mkdir(parents=True, exist_ok=True)
    key_path = data_dir / "identity.key"
    if key_path.exists():
        raw = key_path.read_text(encoding="utf-8").strip()
        if raw:
            return Identity.from_private_bytes(b64url_decode(raw))
    identity = Identity.generate()
    key_path.write_text(identity.private_b64u, encoding="utf-8")
    key_path.chmod(0o600)
    return identity


class SendspinBridge:
    """Manages SendspinClient (source@v1), mDNS ClientListener, and SourceCapture."""

    def __init__(self, config: BlueSpinConfig, status: AppStatus) -> None:
        self._config = config
        self.status = status
        self._client: SendspinClient | None = None
        self._listener: ClientListener | None = None
        self._capture: SourceCapture | None = None
        self._capture_lock = asyncio.Lock()
        self._pairing_store: FileClientPairingStore | None = None
        self._next_chunk_ts_us: int = 0

    async def start(self) -> None:
        identity = load_or_create_identity(self._config.data_dir)
        store_path = self._config.data_dir / "pairing_store.json"
        self._pairing_store = await FileClientPairingStore.open(store_path)

        async def _on_pairing_code(code: str | None, *, grouped: str | None) -> None:
            self.status.ss_pairing_code = grouped or code
            if self.status.ss_pairing_code:
                logger.info("Sendspin pairing code: %s", self.status.ss_pairing_code)

        pairing_support = PairingSupport(pairing_code_display=_on_pairing_code)
        self._client = SendspinClient(
            identity=identity,
            client_name=self._config.friendly_name,
            roles=[Roles.SOURCE],
            pairing_store=self._pairing_store,
            pairing_support=pairing_support,
        )
        await self._client.set_unpaired_access(enabled=self._config.unpaired_access)
        self.status.ss_unpaired_access = self._config.unpaired_access
        self.status.ss_client_id = identity.peer_id

        self._client.add_server_command_listener(self._handle_server_command)
        self._client.add_disconnect_listener(self._handle_disconnect)

        self._listener = ClientListener(
            client_id=identity.peer_id,
            on_connection=self._handle_ws_connection,
            port=self._config.sendspin_port,
            client_name=self._config.friendly_name,
        )
        await self._listener.start()
        self.status.ss_running = True
        logger.info(
            "Sendspin source listening on port %d (client_id=%s)",
            self._config.sendspin_port,
            identity.peer_id,
        )

    async def stop(self) -> None:
        self.status.ss_running = False
        await self._stop_capture()
        if self._listener is not None:
            await self._listener.stop()
            self._listener = None
        if self._client is not None:
            await self._client.disconnect(GoodbyeReason.SHUTDOWN)
            self._client = None

    async def _handle_ws_connection(self, ws: web.WebSocketResponse) -> None:
        assert self._client is not None
        await self._client.attach_websocket(ws)

    def _handle_disconnect(self) -> None:
        self.status.ss_streaming = False
        asyncio.create_task(self._stop_capture())

    def _handle_server_command(self, payload: ServerCommandPayload) -> None:
        source_cmd = getattr(payload, "source", None)
        if source_cmd is None:
            return
        cmd_name = getattr(source_cmd, "command", None)
        if cmd_name == "start":
            asyncio.create_task(self._start_capture())
        elif cmd_name == "stop":
            asyncio.create_task(self._stop_capture())

    async def _start_capture(self) -> None:
        async with self._capture_lock:
            if self._client is None or self._capture is not None:
                return
            codec = _CODEC_MAP.get(self._config.codec, AudioCodec.OPUS)
            fmt = SupportedAudioFormat(
                codec=codec,
                sample_rate=self._config.sample_rate,
                channels=self._config.channels,
                bit_depth=self._config.bit_depth,
            )
            try:
                capture = self._client.create_source_capture(fmt)
                await capture.start()
                self._capture = capture
                self._next_chunk_ts_us = 0
                self.status.ss_streaming = True
                logger.info(
                    "Started Sendspin source stream (codec=%s, rate=%d Hz)",
                    capture.codec.value,
                    self._config.sample_rate,
                )
            except Exception:
                logger.exception("Failed to start Sendspin SourceCapture")
                self._capture = None
                self.status.ss_streaming = False

    async def _stop_capture(self) -> None:
        async with self._capture_lock:
            if self._capture is None:
                self.status.ss_streaming = False
                return
            capture = self._capture
            self._capture = None
            self.status.ss_streaming = False
            with contextlib.suppress(Exception):
                await capture.stop()
            logger.info("Stopped Sendspin source stream")

    async def set_signal_present(self, present: bool) -> None:
        if self._client is None:
            return
        conn = getattr(self._client, "_admitted_connection", None)
        if conn is not None:
            with contextlib.suppress(Exception):
                await conn.send_source_signal(
                    SignalState.PRESENT if present else SignalState.ABSENT
                )

    async def set_unpaired_access(self, enabled: bool) -> None:
        self.status.ss_unpaired_access = enabled
        if self._client is not None:
            await self._client.set_unpaired_access(enabled=enabled)

    async def feed_pcm(self, pcm: bytes, _: int = 0) -> None:
        if self._capture is None or self._client is None:
            return
        now_us = self._client.now_us()
        chunk_us = self._config.chunk_ms * 1000
        if self._next_chunk_ts_us == 0 or abs(now_us - self._next_chunk_ts_us) > 100_000:
            self._next_chunk_ts_us = now_us - chunk_us
        ts_us = self._next_chunk_ts_us
        self._next_chunk_ts_us += chunk_us
        try:
            await self._capture.feed(pcm, ts_us)
        except Exception:
            logger.debug("SourceCapture.feed failed", exc_info=True)
