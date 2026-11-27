"""Lightweight aiohttp web dashboard & control API for BlueSpin Pi."""

from __future__ import annotations

from dataclasses import asdict
import html
import logging

from aiohttp import web

from .bluetooth_manager import BluetoothManager
from .config import AppStatus, BlueSpinConfig
from .sendspin_bridge import SendspinBridge

logger = logging.getLogger(__name__)

_HTML_PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>__TITLE__</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #0f172a; color: #e2e8f0; margin: 0; padding: 20px; }
    .wrap { max-width: 720px; margin: 0 auto; }
    .card { background: #1e293b; border-radius: 12px; padding: 16px 20px; margin-bottom: 16px; border: 1px solid #334155; }
    h1 { margin: 0 0 6px; font-size: 1.5rem; }
    h2 { margin: 0 0 12px; font-size: 1.05rem; color: #94a3b8; text-transform: uppercase; letter-spacing: .05em; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 10px; }
    .kv { background: #0f172a; padding: 10px 12px; border-radius: 8px; }
    .k { font-size: 0.78rem; color: #94a3b8; }
    .v { font-size: 1rem; font-weight: 600; margin-top: 2px; word-break: break-all; }
    .btns { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
    button { background: #3b82f6; color: white; border: 0; padding: 9px 14px; border-radius: 8px; cursor: pointer; font-weight: 600; }
    button.sec { background: #475569; }
    button.warn { background: #dc2626; }
    .code { font-size: 2rem; font-weight: 800; letter-spacing: .12em; color: #38bdf8; }
  </style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1 id="title">__TITLE__</h1>
    <div class="k" id="ip">Loading...</div>
  </div>

  <div class="card" id="pair_card" style="display:none">
    <h2>Music Assistant Pairing Code</h2>
    <div class="code" id="ss_code"></div>
  </div>

  <div class="card">
    <h2>Bluetooth &amp; Pairing</h2>
    <div class="grid">
      <div class="kv"><div class="k">Connected Phone</div><div class="v" id="bt_dev">-</div></div>
      <div class="kv"><div class="k">Streaming</div><div class="v" id="bt_stream">-</div></div>
      <div class="kv"><div class="k">Codec</div><div class="v" id="bt_codec">-</div></div>
      <div class="kv"><div class="k">Pairing Window</div><div class="v" id="bt_pair">-</div></div>
      <div class="kv"><div class="k">Volume</div><div class="v" id="bt_vol">-</div></div>
      <div class="kv"><div class="k">Paired Devices</div><div class="v" id="bt_cnt">-</div></div>
    </div>
    <div class="btns">
      <button onclick="post('/api/bt/pair/start')">Start Pairing</button>
      <button class="sec" onclick="post('/api/bt/pair/stop')">Stop Pairing</button>
      <button class="sec" onclick="post('/api/bt/reconnect')">Reconnect Last</button>
      <button class="sec" onclick="post('/api/bt/disconnect')">Disconnect</button>
      <button class="warn" onclick="post('/api/bt/forget')">Forget All</button>
    </div>
  </div>

  <div class="card">
    <h2>Sendspin Source</h2>
    <div class="grid">
      <div class="kv"><div class="k">State</div><div class="v" id="ss_state">-</div></div>
      <div class="kv"><div class="k">Unpaired Access</div><div class="v" id="ss_unpaired">-</div></div>
      <div class="kv"><div class="k">Source URI</div><div class="v" id="ss_uri">-</div></div>
    </div>
    <div class="btns">
      <button class="sec" onclick="toggleUnpaired()">Toggle Unpaired Access</button>
    </div>
  </div>
</div>
<script>
let lastStatus = {};
async function post(url, body) {
  await fetch(url, { method: 'POST', headers: {'Content-Type':'application/json'}, body: body ? JSON.stringify(body) : '{}' });
  refresh();
}
function toggleUnpaired() {
  post('/api/sendspin/unpaired', { enabled: !lastStatus.ss_unpaired_access });
}
async function refresh() {
  const r = await fetch('/api/status');
  const s = await r.json();
  lastStatus = s;
  document.getElementById('title').textContent = s.friendly_name;
  document.getElementById('ip').textContent = 'IP: ' + s.ip_address;
  document.getElementById('bt_dev').textContent = s.bt_connected ? s.bt_device_name + ' (' + s.bt_device_mac + ')' : 'Waiting...';
  document.getElementById('bt_stream').textContent = s.bt_streaming ? 'PLAYING' : 'IDLE';
  document.getElementById('bt_codec').textContent = s.bt_codec;
  document.getElementById('bt_pair').textContent = s.bt_pairing_open ? 'OPEN (Just Works)' : 'Closed';
  document.getElementById('bt_vol').textContent = s.bt_volume_pct + '%';
  document.getElementById('bt_cnt').textContent = s.bt_paired_count;
  document.getElementById('ss_state').textContent = !s.ss_running ? 'INIT' : (s.ss_streaming ? 'LIVE' : 'IDLE');
  document.getElementById('ss_unpaired').textContent = s.ss_unpaired_access ? 'Enabled' : 'Disabled';
  document.getElementById('ss_uri').textContent = s.ss_client_id ? ('sendspin_source://audio_source/' + s.ss_client_id) : '-';
  const pc = document.getElementById('pair_card');
  if (s.ss_pairing_code) { pc.style.display = 'block'; document.getElementById('ss_code').textContent = s.ss_pairing_code; }
  else { pc.style.display = 'none'; }
}
refresh();
setInterval(refresh, 1500);
</script>
</body>
</html>"""


class WebUI:
    """HTTP dashboard & REST API for controlling BlueSpin Pi."""

    def __init__(
        self,
        config: BlueSpinConfig,
        status: AppStatus,
        bt: BluetoothManager,
        sendspin: SendspinBridge,
    ) -> None:
        self._config = config
        self._status = status
        self._bt = bt
        self._sendspin = sendspin
        self._runner: web.AppRunner | None = None

    async def start(self) -> None:
        app = web.Application()
        app.router.add_get("/", self._handle_index)
        app.router.add_get("/api/status", self._handle_status)
        app.router.add_post("/api/bt/pair/start", self._handle_pair_start)
        app.router.add_post("/api/bt/pair/stop", self._handle_pair_stop)
        app.router.add_post("/api/bt/disconnect", self._handle_disconnect)
        app.router.add_post("/api/bt/reconnect", self._handle_reconnect)
        app.router.add_post("/api/bt/forget", self._handle_forget)
        app.router.add_post("/api/sendspin/unpaired", self._handle_unpaired)

        self._runner = web.AppRunner(app)
        await self._runner.setup()
        site = web.TCPSite(self._runner, "0.0.0.0", self._config.web_port)
        await site.start()
        logger.info("Web dashboard listening on http://0.0.0.0:%d/", self._config.web_port)

    async def stop(self) -> None:
        if self._runner is not None:
            await self._runner.cleanup()
            self._runner = None

    async def _handle_index(self, _request: web.Request) -> web.Response:
        page = _HTML_PAGE.replace("__TITLE__", html.escape(self._status.friendly_name))
        return web.Response(text=page, content_type="text/html")

    async def _handle_status(self, _request: web.Request) -> web.Response:
        return web.json_response(asdict(self._status))

    async def _handle_pair_start(self, _request: web.Request) -> web.Response:
        await self._bt.start_pairing()
        return web.json_response({"ok": True})

    async def _handle_pair_stop(self, _request: web.Request) -> web.Response:
        await self._bt.stop_pairing()
        return web.json_response({"ok": True})

    async def _handle_disconnect(self, _request: web.Request) -> web.Response:
        await self._bt.disconnect_current()
        return web.json_response({"ok": True})

    async def _handle_reconnect(self, _request: web.Request) -> web.Response:
        await self._bt.reconnect_last()
        return web.json_response({"ok": True})

    async def _handle_forget(self, _request: web.Request) -> web.Response:
        await self._bt.forget_all()
        return web.json_response({"ok": True})

    async def _handle_unpaired(self, request: web.Request) -> web.Response:
        data = await request.json()
        enabled = bool(data.get("enabled", False))
        await self._sendspin.set_unpaired_access(enabled)
        return web.json_response({"ok": True, "unpaired_access": enabled})
