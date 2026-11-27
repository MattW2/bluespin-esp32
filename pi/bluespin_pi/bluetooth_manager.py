"""BlueZ D-Bus A2DP Sink manager with pinless SSP Just Works pairing."""

import asyncio
from collections.abc import Awaitable, Callable
import contextlib
import logging
from typing import Any

from .config import AppStatus, BlueSpinConfig

logger = logging.getLogger(__name__)

A2DP_SINK_UUID = "0000110b-0000-1000-8000-00805f9b34fb"
AGENT_PATH = "/org/bluespin/agent"
BLUEZ_SERVICE = "org.bluez"
ADAPTER_PATH = "/org/bluez/hci0"

try:
    from dbus_fast import BusType, DBusError, Variant
    from dbus_fast.aio import MessageBus
    from dbus_fast.service import ServiceInterface, method

    DBUS_AVAILABLE = True
except (ImportError, AttributeError):  # pragma: no cover - non-Linux dev environments
    DBUS_AVAILABLE = False
    BusType = Any  # type: ignore[misc, assignment]
    DBusError = Exception  # type: ignore[misc, assignment]
    Variant = Any  # type: ignore[misc, assignment]
    MessageBus = Any  # type: ignore[misc, assignment]

    class ServiceInterface:  # type: ignore[no-redef]
        def __init__(self, name: str) -> None:
            self.name = name

    def method(name: str | None = None) -> Callable[[Any], Any]:  # type: ignore[no-redef]
        return lambda fn: fn


class _PinlessAgent(ServiceInterface):
    """BlueZ Agent1 implementing NoInputNoOutput SSP gated by the pairing window."""

    def __init__(self, manager: "BluetoothManager") -> None:
        super().__init__("org.bluez.Agent1")
        self._mgr = manager

    @method()
    def Release(self) -> None:  # noqa: N802
        logger.debug("BlueZ agent released")

    @method()
    def RequestConfirmation(self, device: "o", passkey: "u") -> None:  # type: ignore[valid-type]  # noqa: N802
        if not self._mgr.status.bt_pairing_open:
            logger.warning("Rejected SSP confirmation from %s (pairing window closed)", device)
            raise DBusError("org.bluez.Error.Rejected", "Pairing window is closed")
        logger.info("Accepted pinless SSP pairing for %s (passkey=%06d)", device, passkey)

    @method()
    def RequestAuthorization(self, device: "o") -> None:  # type: ignore[valid-type]  # noqa: N802
        if not self._mgr.status.bt_pairing_open and not self._mgr.is_device_paired(device):
            raise DBusError("org.bluez.Error.Rejected", "Pairing window is closed")

    @method()
    def AuthorizeService(self, device: "o", uuid: "s") -> None:  # type: ignore[valid-type]  # noqa: N802
        if not self._mgr.status.bt_allow_connect and not self._mgr.status.bt_pairing_open:
            raise DBusError("org.bluez.Error.Rejected", "Device connections disabled")
        logger.debug("Authorized service %s for %s", uuid, device)

    @method()
    def Cancel(self) -> None:  # noqa: N802
        logger.debug("BlueZ agent canceled")


class BluetoothManager:
    """Manages BlueZ adapter, pairing window, A2DP connection state, and AVRCP volume."""

    def __init__(
        self,
        config: BlueSpinConfig,
        status: AppStatus,
        on_streaming_changed: Callable[[bool], Awaitable[None]] | None = None,
    ) -> None:
        self._config = config
        self.status = status
        self._on_streaming_changed = on_streaming_changed
        self._bus: MessageBus | None = None
        self._agent: _PinlessAgent | None = None
        self._pairing_timer: asyncio.Task[None] | None = None
        self._poll_task: asyncio.Task[None] | None = None
        self._paired_paths: set[str] = set()
        self._current_device_path: str | None = None
        self._last_device_path: str | None = None
        self._running = False

    def is_device_paired(self, device_path: str) -> bool:
        return device_path in self._paired_paths

    async def start(self) -> None:
        self._running = True
        self.status.bt_allow_connect = self._config.allow_device_connect
        if not DBUS_AVAILABLE:
            logger.warning("dbus-fast not available; BluetoothManager running in stub mode")
            return

        try:
            self._bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
            self._agent = _PinlessAgent(self)
            self._bus.export(AGENT_PATH, self._agent)

            agent_mgr_intro = await self._bus.introspect(BLUEZ_SERVICE, "/org/bluez")
            agent_mgr_proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, "/org/bluez", agent_mgr_intro)
            agent_mgr = agent_mgr_proxy.get_interface("org.bluez.AgentManager1")
            with contextlib.suppress(DBusError):
                await agent_mgr.call_unregister_agent(AGENT_PATH)
            await agent_mgr.call_register_agent(AGENT_PATH, "NoInputNoOutput")
            await agent_mgr.call_request_default_agent(AGENT_PATH)

            await self._configure_adapter()
            await self.refresh_state()
            self._poll_task = asyncio.create_task(self._poll_loop(), name="bt-poll")
            logger.info("BlueZ A2DP manager started (name=%s)", self._config.friendly_name)
        except Exception:
            logger.exception("Failed to initialize BlueZ D-Bus manager")

    async def stop(self) -> None:
        self._running = False
        if self._pairing_timer is not None:
            self._pairing_timer.cancel()
        if self._poll_task is not None:
            self._poll_task.cancel()
        if self._bus is not None:
            self._bus.disconnect()
            self._bus = None

    async def _configure_adapter(self) -> None:
        assert self._bus is not None
        intro = await self._bus.introspect(BLUEZ_SERVICE, ADAPTER_PATH)
        proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, ADAPTER_PATH, intro)
        props = proxy.get_interface("org.freedesktop.DBus.Properties")
        await props.call_set("org.bluez.Adapter1", "Powered", Variant("b", True))
        await props.call_set("org.bluez.Adapter1", "Alias", Variant("s", self._config.friendly_name))
        await props.call_set("org.bluez.Adapter1", "Pairable", Variant("b", False))
        await props.call_set("org.bluez.Adapter1", "Discoverable", Variant("b", False))
        await props.call_set("org.bluez.Adapter1", "PairableTimeout", Variant("u", 0))
        await props.call_set("org.bluez.Adapter1", "DiscoverableTimeout", Variant("u", 0))

    async def start_pairing(self, duration_s: int | None = None) -> None:
        window = duration_s or self._config.pairing_window_s
        self.status.bt_pairing_open = True
        if self._pairing_timer is not None:
            self._pairing_timer.cancel()
        await self._set_scan_mode(discoverable=True, pairable=True)
        logger.info("Opened Bluetooth pairing window for %d s", window)

        async def _expire() -> None:
            await asyncio.sleep(window)
            await self.stop_pairing()

        self._pairing_timer = asyncio.create_task(_expire(), name="bt-pair-expire")

    async def stop_pairing(self) -> None:
        if self._pairing_timer is not None:
            self._pairing_timer.cancel()
            self._pairing_timer = None
        if self.status.bt_pairing_open:
            self.status.bt_pairing_open = False
            await self._set_scan_mode(discoverable=False, pairable=False)
            logger.info("Closed Bluetooth pairing window")

    async def _set_scan_mode(self, *, discoverable: bool, pairable: bool) -> None:
        if self._bus is None:
            return
        try:
            intro = await self._bus.introspect(BLUEZ_SERVICE, ADAPTER_PATH)
            proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, ADAPTER_PATH, intro)
            props = proxy.get_interface("org.freedesktop.DBus.Properties")
            await props.call_set("org.bluez.Adapter1", "Pairable", Variant("b", pairable))
            await props.call_set("org.bluez.Adapter1", "Discoverable", Variant("b", discoverable))
        except Exception:
            logger.exception("Failed to update BlueZ scan mode")

    async def set_allow_connect(self, allow: bool) -> None:
        self.status.bt_allow_connect = allow

    async def disconnect_current(self) -> None:
        if self._bus is None or self._current_device_path is None:
            return
        try:
            intro = await self._bus.introspect(BLUEZ_SERVICE, self._current_device_path)
            proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, self._current_device_path, intro)
            dev = proxy.get_interface("org.bluez.Device1")
            await dev.call_disconnect()
        except Exception:
            logger.exception("Failed to disconnect %s", self._current_device_path)

    async def reconnect_last(self) -> None:
        if self._bus is None:
            return
        target = self._last_device_path or (next(iter(self._paired_paths), None))
        if target is None:
            logger.warning("No paired Bluetooth device to reconnect")
            return
        try:
            intro = await self._bus.introspect(BLUEZ_SERVICE, target)
            proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, target, intro)
            dev = proxy.get_interface("org.bluez.Device1")
            await dev.call_connect()
        except Exception:
            logger.exception("Failed to reconnect %s", target)

    async def forget_all(self) -> None:
        if self._bus is None:
            self._paired_paths.clear()
            self.status.bt_paired_count = 0
            return
        try:
            intro = await self._bus.introspect(BLUEZ_SERVICE, ADAPTER_PATH)
            proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, ADAPTER_PATH, intro)
            adapter = proxy.get_interface("org.bluez.Adapter1")
            for path in list(self._paired_paths):
                with contextlib.suppress(DBusError):
                    await adapter.call_remove_device(path)
            await self.refresh_state()
        except Exception:
            logger.exception("Failed to forget paired devices")

    async def refresh_state(self) -> None:
        if self._bus is None:
            return
        intro = await self._bus.introspect(BLUEZ_SERVICE, "/")
        proxy = self._bus.get_proxy_object(BLUEZ_SERVICE, "/", intro)
        obj_mgr = proxy.get_interface("org.freedesktop.DBus.ObjectManager")
        objects: dict[str, dict[str, dict[str, Variant]]] = await obj_mgr.call_get_managed_objects()

        paired: set[str] = set()
        connected_path: str | None = None
        connected_name = "None"
        connected_mac = ""
        streaming = False
        codec_label = "Idle"
        vol_pct = self.status.bt_volume_pct

        for path, ifaces in objects.items():
            if "org.bluez.Device1" in ifaces:
                dev = ifaces["org.bluez.Device1"]
                is_paired = bool(dev.get("Paired", Variant("b", False)).value)
                is_conn = bool(dev.get("Connected", Variant("b", False)).value)
                if is_paired:
                    paired.add(path)
                if is_conn:
                    connected_path = path
                    connected_mac = str(dev.get("Address", Variant("s", "")).value)
                    connected_name = str(dev.get("Alias", Variant("s", connected_mac)).value)

            if "org.bluez.MediaTransport1" in ifaces:
                mt = ifaces["org.bluez.MediaTransport1"]
                state = str(mt.get("State", Variant("s", "idle")).value)
                if state in ("active", "pending"):
                    streaming = True
                codec_val = mt.get("Codec")
                if codec_val is not None:
                    codec_byte = int(codec_val.value)
                    codec_label = {0: "SBC", 2: "AAC"}.get(codec_byte, f"Codec {codec_byte}")
                vol_val = mt.get("Volume")
                if vol_val is not None:
                    vol_pct = round(int(vol_val.value) * 100 / 127)

        self._paired_paths = paired
        self.status.bt_paired_count = len(paired)
        self._current_device_path = connected_path
        if connected_path is not None:
            self._last_device_path = connected_path

        self.status.bt_connected = connected_path is not None
        self.status.bt_device_name = connected_name if connected_path else "None"
        self.status.bt_device_mac = connected_mac if connected_path else ""
        self.status.bt_codec = codec_label if connected_path else "Idle"
        self.status.bt_volume_pct = vol_pct

        prev_streaming = self.status.bt_streaming
        self.status.bt_streaming = streaming
        if prev_streaming != streaming and self._on_streaming_changed is not None:
            await self._on_streaming_changed(streaming)

    async def _poll_loop(self) -> None:
        while self._running:
            try:
                await self.refresh_state()
            except Exception:
                logger.debug("BlueZ poll error", exc_info=True)
            await asyncio.sleep(1.0)
