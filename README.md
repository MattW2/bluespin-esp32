# BlueSpin-esp32

**Turn an ESP32 into a Bluetooth speaker for [Music Assistant](https://music-assistant.io) and [Sendspin](https://www.sendspin-audio.com/) — with zero expiring tokens.**

[![Import Blueprint to My Home Assistant](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2FMattW2%2Fbluespin-esp32%2Fblob%2Fmain%2Fha%2Fblueprints%2Fbluespin_esp32.yaml)

**BlueSpin-esp32** runs directly on an ESP32 microcontroller using ESPHome. It presents itself as a standard **Bluetooth Classic speaker** (A2DP Sink + AVRCP Absolute Volume) with **pinless Secure Simple Pairing ("Just Works")**, specifically compatible with **Google Nest Hub / Google Home** smart displays as well as phones, tablets, and laptops. Pairing is **only accepted while you open a pairing window** from Home Assistant, the on-device ESP32 web dashboard, or the board's physical button.

Received Bluetooth audio streams over **Sendspin (`source@v1`)** into Music Assistant as a **Live Input**, so Music Assistant can play it synchronized on any Sendspin speaker, AirPlay/Cast speaker, or multi-room group.

---

## Features

- **Pinless Bluetooth Pairing Window:** Advertises the Bluetooth `Audio/Video → Loudspeaker` Class of Device (`CoD`) with `ESP_BT_IO_CAP_NONE` (SSP Just Works) so headless devices like a **Google Nest Hub** discover and pair with zero PIN prompts—while blocking unauthorized pairing whenever the window is closed.
- **Tokenless Permanent Sendspin Pairing:** Pairs once with Music Assistant using a 6-digit dynamic Sendspin code displayed in Home Assistant and on the ESP32 web UI. Uses Noise KKpsk2 encryption with a permanent long-term key that **never expires**.
- **Home Assistant Blueprint (`ha/blueprints/bluespin_esp32.yaml`):**
  - Automatically starts the Sendspin Live Input on your chosen Music Assistant speaker or sync group when Bluetooth audio begins.
  - **Two-way volume sync:** Speaker volume wins on stream start (preventing a phone left at 100% from blasting the room), then syncs volume changes in either direction with echo suppression.
  - **Pause & disconnect handling:** Fills silence while paused so resuming within 10 minutes is instantaneous, and stops the speakers on Bluetooth disconnect or after the pause timeout.
- **Last-One-Wins Multi-Device Bonding:** Remembers up to 8 paired Bluetooth sources in NVS; when another bonded source connects, it cleanly takes over.
- **Lightweight On-Device Web Dashboard (`http://<esp32-ip>/`):** Offline-capable ESPHome `web_server` v3 organized into **Bluetooth & Pairing**, **Audio & Sendspin**, and **System & Debug** groups, with live SSE debug logs, RAM/PSRAM diagnostics, audio buffer/underrun/overflow counters, and browser OTA firmware upload.

---

## Supported Hardware

> **Important:** Bluetooth A2DP requires **Bluetooth Classic (BR/EDR)**, which is **only hardware-supported on the original ESP32** (Xtensa LX6). Newer BLE-only chips (**ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2**) **cannot** be used and are rejected at compile-time validation.

| Board | PSRAM | Network | ESPHome Builder Example | Local CLI Config | Notes |
|---|---|---|---|---|---|
| **ESP32-WROVER-E / -IE DevKit** *(primary Wi-Fi)* | 4–8 MB | Wi-Fi | [`examples/esphome-builder/bluespin-wrover-wifi.yaml`](examples/esphome-builder/bluespin-wrover-wifi.yaml) | [`bluespin-esp32-wrover.yaml`](bluespin-esp32-wrover.yaml) | `-IE` with an external u.FL antenna improves 2.4 GHz Wi-Fi + BT coexistence. |
| **Olimex ESP32-POE2** *(recommended for best audio)* | 8 MB | Ethernet + PoE | [`examples/esphome-builder/bluespin-poe2-ethernet.yaml`](examples/esphome-builder/bluespin-poe2-ethernet.yaml) | [`bluespin-esp32-poe2.yaml`](bluespin-esp32-poe2.yaml) | Wired Ethernet frees the 2.4 GHz radio to run Bluetooth exclusively. |
| **Olimex ESP32-Gateway (Rev F+)** | 4 MB (WROVER) | Ethernet | Adapt `bluespin-poe2-ethernet.yaml` | Adapt `bluespin-esp32-poe2.yaml` | Check board revision pinout for Ethernet PHY clock/power pins. |
| **ESP32-A1S AudioKit / LilyGO TTGO T8 / T-Audio** | 4–8 MB | Wi-Fi | [`examples/esphome-builder/bluespin-wrover-wifi.yaml`](examples/esphome-builder/bluespin-wrover-wifi.yaml) | [`bluespin-esp32-wrover.yaml`](bluespin-esp32-wrover.yaml) | On-board DAC/codec is left unused. |

---

## Deploying with ESPHome Builder (Recommended)

You can deploy and OTA-manage one or more BlueSpin-esp32 receivers directly from **Home Assistant → ESPHome Device Builder** without copying any C++ files onto your Home Assistant server.

### Prerequisites
- **ESPHome Device Builder** add-on version **2026.11.1 or newer**
- **Music Assistant** version **2.10.7 or newer**
- Note: Compiling ESP-IDF + Bluedroid + Sendspin uses ~1.5–2 GB of RAM on the machine running ESPHome Builder. On low-RAM hosts (e.g. a 2 GB Raspberry Pi), either close heavy add-ons during the initial compile or build locally once via CLI and upload via `http://<esp32-ip>/`.

### Step 1: Add Secrets in ESPHome Builder
1. Open **ESPHome Builder** in Home Assistant and click **Secrets** in the top-right corner.
2. Ensure these keys exist in your `secrets.yaml` (generate a new 32-byte base64 `api_encryption_key` when creating a device in ESPHome Builder, or copy one from ESPHome's wizard):
   ```yaml
   wifi_ssid: "YourWiFiSSID"
   wifi_password: "YourWiFiPassword"
   api_encryption_key: "YOUR_32_BYTE_BASE64_KEY_HERE"
   ota_password: "change-me-ota-password"
   ap_fallback_password: "change-me-ap-password"
   ```
   *(For Ethernet-only `ESP32-POE2` deployments, only `api_encryption_key` and `ota_password` are needed.)*

### Step 2: Create the Device YAML
1. In **ESPHome Builder**, click **+ New Device → Continue**.
2. Enter a name (for example `BlueSpin Living Room`), select **ESP32** (or click **Skip**), and open **Edit** on the newly created device card.
3. Replace the YAML contents with one of the ready-to-use templates from [`examples/esphome-builder/`](examples/esphome-builder/):
   - **Wi-Fi (ESP32-WROVER):** copy [`examples/esphome-builder/bluespin-wrover-wifi.yaml`](examples/esphome-builder/bluespin-wrover-wifi.yaml)
   - **Ethernet + PoE (Olimex ESP32-POE2):** copy [`examples/esphome-builder/bluespin-poe2-ethernet.yaml`](examples/esphome-builder/bluespin-poe2-ethernet.yaml)
4. Adjust `substitutions.name` and `substitutions.friendly_name` at the top if deploying multiple rooms (e.g., `bluespin-kitchen` / `BlueSpin Kitchen`). ESPHome automatically clones `https://github.com/MattW2/bluespin-esp32` to build the `bt_a2dp_sink` component and include [`common/bluespin-base.yaml`](common/bluespin-base.yaml).

### Step 3: Flash the ESP32
1. Click **Save**, then click **Install** in the top-right corner of the editor:
   - **First-time USB flash (from your laptop/PC browser):** Connect the ESP32 to your computer via USB, choose **Plug into this computer**, let ESPHome Builder compile the firmware, click **Download project** (Factory format) if prompted, and flash directly in Chrome or Edge via WebSerial.
   - **First-time USB flash (plugged into Home Assistant host):** Choose **Plug into the computer running ESPHome Device Builder** and select the serial port.
   - **Subsequent updates:** Choose **Wirelessly** for encrypted OTA updates.
   - ⚠️ **Olimex ESP32-POE2 hardware caution:** PoE on the ESP32-POE2 is not galvanically isolated from USB. **Always unplug the PoE Ethernet cable before connecting a USB-C cable to your computer**, or use a USB isolator. Once flashed once, use **Wirelessly** (OTA) over PoE Ethernet.

---

## Alternative: Local ESPHome CLI Build & Flash

If you prefer building from a local clone on your computer (Python ≥ 3.12, ESPHome ≥ 2026.11.1):

```bash
git clone https://github.com/MattW2/bluespin-esp32.git
cd bluespin-esp32
cp secrets.yaml.example secrets.yaml
# Edit secrets.yaml with your Wi-Fi / API / OTA credentials

# Wi-Fi (ESP32-WROVER)
esphome run bluespin-esp32-wrover.yaml

# Ethernet PoE (Olimex ESP32-POE2 — disconnect PoE before connecting USB-C!)
esphome run bluespin-esp32-poe2.yaml
```

---

## Post-Flash Setup in Home Assistant & Music Assistant

### 1. Adopt & Pair with Music Assistant (One-Time)
1. In **Home Assistant → Settings → Devices & Services**, click **Configure / Adopt** on the discovered `BlueSpin ESP32` ESPHome device (enter your `api_encryption_key` if prompted).
2. Open **Music Assistant** → **Settings → Players / Providers (Sendspin)** and start pairing with `BlueSpin ESP32`.
3. Read the 6-digit code from the **Music Assistant pairing code** sensor in Home Assistant (or at `http://<esp32-ip>/` under *Audio & Sendspin*) and enter it into Music Assistant.
4. A new **Live Input** (`BlueSpin ESP32`) now appears permanently in Music Assistant—this Noise keypair survives reboots and never expires.

### 2. Import the Home Assistant Automation Blueprint
Click the badge below (or in **Home Assistant → Settings → Automations & Scenes → Blueprints → Import Blueprint**, paste `https://github.com/MattW2/bluespin-esp32/blob/main/ha/blueprints/bluespin_esp32.yaml`):

[![Import Blueprint to My Home Assistant](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2FMattW2%2Fbluespin-esp32%2Fblob%2Fmain%2Fha%2Fblueprints%2Fbluespin_esp32.yaml)

Create an automation from **BlueSpin-esp32 — Sendspin Live Input & Two-Way Volume Sync**, select your ESP32's four entities (`Phone connected`, `Phone streaming`, `Phone volume`, `Sendspin source URI`), and pick your **Target Music Assistant Speaker / Group**.

### 3. Pair a Google Nest Hub or Phone over Bluetooth
1. Press **Start Bluetooth pairing** in Home Assistant, on `http://<esp32-ip>/`, or via the board's physical button (`BOOT` on WROVER, `BUT1` on ESP32-POE2). This opens a **120-second pinless pairing window**.
2. Pair your audio source:
   - **Google Nest Hub / Nest Audio:** Open the **Google Home** app → select your Nest device → **Settings (gear) → Audio → Default music speaker → Pair Bluetooth speaker** → tap **BlueSpin ESP32**. It pairs immediately with no PIN prompt.
   - **Phone / Tablet / Laptop:** Open Bluetooth settings and select **BlueSpin ESP32**.
3. Once paired, the pairing window closes automatically so random nearby devices cannot pair.

---

## On-Device Web Dashboard (`http://<esp32-ip>/`)

Opening the ESP32's IP address in any browser shows a local dashboard organized into three sections:
- **Bluetooth & Pairing:** Start/stop pairing, disconnect/reconnect/forget devices, toggle phone-initiated reconnects, view connected source name & MAC, and adjust source volume.
- **Audio & Sendspin:** View the 6-digit Music Assistant pairing code, Sendspin source URI, negotiated SBC sample rate, ring buffer fill %, underruns, and overflows.
- **System & Debug:** Live streaming ESPHome debug logs, Free Internal RAM, Free PSRAM, Max Free Block, Uptime, IP/RSSI, Restart button, and `.bin` OTA firmware upload.

> **Tip (Wi-Fi boards):** Close the web dashboard browser tab during long listening sessions so live SSE log traffic doesn't compete for 2.4 GHz airtime.

---

## Running Host Unit Tests

```bash
cmake -S tests/host -B build/tests
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure -V
```

---

## Acknowledgments

- [Music Assistant](https://music-assistant.io/), [Sendspin](https://github.com/Sendspin/sendspin-cpp), & [ESPHome](https://esphome.io/) by the Open Home Foundation.
