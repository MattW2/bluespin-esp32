# BlueSpin-esp32

**Turn an ESP32 into a Bluetooth audio receiver that streams into [Music Assistant](https://music-assistant.io) over [Sendspin](https://www.sendspin-audio.com/).**

[![Import Blueprint to My Home Assistant](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2FMattW2%2Fbluespin-esp32%2Fblob%2Fmain%2Fha%2Fblueprints%2Fbluespin_esp32.yaml)

**BlueSpin-esp32** runs on an ESP32 microcontroller using ESPHome. It acts as a standard **Bluetooth Classic speaker** (A2DP Sink + AVRCP Absolute Volume) using Secure Simple Pairing ("Just Works") so **Google Nest Hub / Google Home** smart displays, phones, tablets, and laptops can connect and play audio to it. You open the Bluetooth pairing window on demand from Home Assistant, the on-device ESP32 web dashboard, or the board's physical button.

Captured Bluetooth audio streams over **Sendspin (`source@v1`)** into Music Assistant as a **Live Input**, allowing Music Assistant to play the audio synchronized across any Sendspin speaker, AirPlay/Cast speaker, or multi-room speaker group.

---

## Features

- **On-Demand Pinless Bluetooth Pairing:** Advertises the Bluetooth `Audio/Video → Loudspeaker` Class of Device (`CoD`) with Secure Simple Pairing (`ESP_BT_IO_CAP_NONE`) for seamless pairing with headless sources like **Google Nest Hub** as well as phones, gated by a configurable pairing window (default 120 seconds).
- **Encrypted Sendspin Live Input (`source@v1`):** Streams 44.1 kHz stereo audio (with automatic resampling from 48/32/16 kHz SBC sources) over Noise KKpsk2-encrypted Sendspin, pairing with Music Assistant via a 6-digit code shown in Home Assistant and on the ESP32 web UI.
- **Home Assistant Automation Blueprint (`ha/blueprints/bluespin_esp32.yaml`):**
  - Automatically starts the Sendspin Live Input on your chosen Music Assistant speaker or sync group when Bluetooth playback begins.
  - **Bidirectional volume sync:** Aligns source volume to the target speaker's volume at stream start, then keeps volume changes on either side synchronized with echo suppression.
  - **Pause & disconnect handling:** Maintains a continuous Sendspin stream during brief pauses for instant resume, and stops speaker playback on Bluetooth disconnect or after a configurable pause timeout.
- **Multi-Device Bonding & Takeover:** Stores up to 8 paired Bluetooth sources in NVS and switches active playback automatically when another bonded device connects.
- **On-Device Web Dashboard (`http://<esp32-ip>/`):** Built-in ESPHome `web_server` v3 organized into **Bluetooth & Pairing**, **Audio & Sendspin**, and **System & Debug** sections, providing pairing controls, status sensors, live debug logs, RAM/PSRAM metrics, audio buffer diagnostics, and browser OTA updates.

---

## Supported Hardware

Bluetooth A2DP uses **Bluetooth Classic (BR/EDR)**, which is provided by original **ESP32** (Xtensa LX6) modules—ideally paired with 4–8 MB PSRAM such as **ESP32-WROVER-E**.

| Board | PSRAM | Network | ESPHome Builder Example | Local CLI Config | Notes |
|---|---|---|---|---|---|
| **ESP32-WROVER-E / -IE DevKit** *(primary Wi-Fi)* | 4–8 MB | Wi-Fi | [`examples/esphome-builder/bluespin-wrover-wifi.yaml`](examples/esphome-builder/bluespin-wrover-wifi.yaml) | [`bluespin-esp32-wrover.yaml`](bluespin-esp32-wrover.yaml) | `-IE` with an external u.FL antenna improves 2.4 GHz Wi-Fi + BT coexistence. |
| **ESP32-WROVER + SSD1306 128×64 OLED** | 4–8 MB | Wi-Fi | [`examples/esphome-builder/bluespin-wrover-wifi-oled.yaml`](examples/esphome-builder/bluespin-wrover-wifi-oled.yaml) | Include `common/bluespin-oled-ssd1306.yaml` | Shows IP, BT device, volume, Sendspin status, buffer bar, and pairing codes on I2C (`GPIO21`/`GPIO22`). |
| **Olimex ESP32-POE2** *(recommended for wired setups)* | 8 MB | Ethernet + PoE | [`examples/esphome-builder/bluespin-poe2-ethernet.yaml`](examples/esphome-builder/bluespin-poe2-ethernet.yaml) | [`bluespin-esp32-poe2.yaml`](bluespin-esp32-poe2.yaml) | Wired Ethernet dedicates the 2.4 GHz radio exclusively to Bluetooth. |
| **Olimex ESP32-Gateway (Rev F+)** | 4 MB (WROVER) | Ethernet | Adapt `bluespin-poe2-ethernet.yaml` | Adapt `bluespin-esp32-poe2.yaml` | Match Ethernet PHY clock/power pins to your board revision. |
| **ESP32-A1S AudioKit / LilyGO TTGO T8 / T-Audio** | 4–8 MB | Wi-Fi | [`examples/esphome-builder/bluespin-wrover-wifi.yaml`](examples/esphome-builder/bluespin-wrover-wifi.yaml) | [`bluespin-esp32-wrover.yaml`](bluespin-esp32-wrover.yaml) | WROVER-class ESP32 module with 4–8 MB PSRAM. |

---

## Deploying with ESPHome Builder (Recommended)

Deploy and OTA-manage one or more BlueSpin-esp32 receivers directly from **Home Assistant → ESPHome Device Builder**—ESPHome pulls the `bt_a2dp_sink` component and shared package straight from GitHub.

### Prerequisites
- **ESPHome Device Builder** add-on version **2026.11.1 or newer**
- **Music Assistant** version **2.10.7 or newer**
- Note: Compiling ESP-IDF + Bluedroid + Sendspin uses ~1.5–2 GB of RAM on the machine running ESPHome Builder. On low-RAM hosts (e.g. a 2 GB Raspberry Pi), close heavy add-ons during the initial compile or build locally via CLI and upload via `http://<esp32-ip>/`.

### Step 1: Add Secrets in ESPHome Builder
1. Open **ESPHome Builder** in Home Assistant and click **Secrets** in the top-right corner.
2. Add the following keys to `secrets.yaml` (generate a 32-byte base64 `api_encryption_key` when creating a device in ESPHome Builder, or copy one from ESPHome's wizard):
   ```yaml
   wifi_ssid: "YourWiFiSSID"
   wifi_password: "YourWiFiPassword"
   api_encryption_key: "YOUR_32_BYTE_BASE64_KEY_HERE"
   ota_password: "change-me-ota-password"
   ap_fallback_password: "change-me-ap-password"
   ```
   *(For Ethernet `ESP32-POE2` deployments, `api_encryption_key` and `ota_password` are sufficient.)*

### Step 2: Create the Device YAML
1. In **ESPHome Builder**, click **+ New Device → Continue**.
2. Enter a name (for example `BlueSpin Living Room`), select **ESP32**, and open **Edit** on the device card.
3. Replace the YAML contents with one of the templates in [`examples/esphome-builder/`](examples/esphome-builder/):
   - **Wi-Fi (ESP32-WROVER):** [`examples/esphome-builder/bluespin-wrover-wifi.yaml`](examples/esphome-builder/bluespin-wrover-wifi.yaml)
   - **Wi-Fi (ESP32-WROVER + SSD1306 OLED):** [`examples/esphome-builder/bluespin-wrover-wifi-oled.yaml`](examples/esphome-builder/bluespin-wrover-wifi-oled.yaml)
   - **Ethernet + PoE (Olimex ESP32-POE2):** [`examples/esphome-builder/bluespin-poe2-ethernet.yaml`](examples/esphome-builder/bluespin-poe2-ethernet.yaml)
4. Customize `substitutions.name` and `substitutions.friendly_name` at the top when deploying multiple rooms (e.g., `bluespin-kitchen` / `BlueSpin Kitchen`).

### Step 3: Flash the ESP32
1. Click **Save**, then click **Install** in the top-right corner of the editor:
   - **First-time USB flash (from your browser):** Connect the ESP32 to your computer via USB, select **Plug into this computer**, and flash via WebSerial in Chrome or Edge.
   - **First-time USB flash (plugged into Home Assistant host):** Select **Plug into the computer running ESPHome Device Builder** and choose the serial port.
   - **Subsequent updates:** Select **Wirelessly** for OTA updates.
   - ⚠️ **Olimex ESP32-POE2 hardware note:** PoE on the ESP32-POE2 shares ground with USB. **Unplug the PoE Ethernet cable before connecting a USB-C cable to your computer**, or use a USB isolator. After the initial flash, update via **Wirelessly** (OTA) over PoE Ethernet.

---

## Alternative: Local ESPHome CLI Build & Flash

To build from a local clone on your computer (Python ≥ 3.12, ESPHome ≥ 2026.11.1):

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

### 1. Adopt & Pair with Music Assistant
1. In **Home Assistant → Settings → Devices & Services**, click **Configure / Adopt** on the discovered `BlueSpin ESP32` ESPHome device.
2. Open **Music Assistant** → **Settings → Players / Providers (Sendspin)** and start pairing with `BlueSpin ESP32`.
3. Enter the 6-digit code shown in the **Music Assistant pairing code** sensor in Home Assistant (or at `http://<esp32-ip>/` under *Audio & Sendspin*).
4. `BlueSpin ESP32` appears as a **Live Input** in Music Assistant and reconnects automatically across reboots.

### 2. Import the Home Assistant Automation Blueprint
Click the badge below (or in **Home Assistant → Settings → Automations & Scenes → Blueprints → Import Blueprint**, paste `https://github.com/MattW2/bluespin-esp32/blob/main/ha/blueprints/bluespin_esp32.yaml`):

[![Import Blueprint to My Home Assistant](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2FMattW2%2Fbluespin-esp32%2Fblob%2Fmain%2Fha%2Fblueprints%2Fbluespin_esp32.yaml)

Create an automation from **BlueSpin-esp32 — Sendspin Live Input & Two-Way Volume Sync**, select your ESP32's four entities (`Phone connected`, `Phone streaming`, `Phone volume`, `Sendspin source URI`), and select your **Target Music Assistant Speaker / Group**.

### 3. Pair a Google Nest Hub or Phone over Bluetooth
1. Press **Start Bluetooth pairing** in Home Assistant, on `http://<esp32-ip>/`, or via the board's physical button (`BOOT` on WROVER, `BUT1` on ESP32-POE2) to open the **120-second pairing window**.
2. Connect your audio source:
   - **Google Nest Hub / Nest Audio:** Open the **Google Home** app → select your Nest device → **Settings (gear) → Audio → Default music speaker → Pair Bluetooth speaker** → tap **BlueSpin ESP32**.
   - **Phone / Tablet / Laptop:** Open Bluetooth settings and select **BlueSpin ESP32**.
3. Once paired, the pairing window closes automatically and the bonded device can reconnect anytime.

---

## On-Device Web Dashboard (`http://<esp32-ip>/`)

Opening the ESP32's IP address in a browser displays a local dashboard organized into three groups:
- **Bluetooth & Pairing:** Start/stop pairing, disconnect/reconnect/forget devices, toggle phone-initiated reconnects, view connected source name & MAC, and adjust source volume.
- **Audio & Sendspin:** View the 6-digit Music Assistant pairing code, Sendspin source URI, negotiated SBC sample rate, ring buffer fill %, underruns, and overflows.
- **System & Debug:** Live streaming ESPHome debug logs, Free Internal RAM, Free PSRAM, Max Free Block, Uptime, IP/RSSI, Restart button, and `.bin` OTA firmware upload.

> **Tip (Wi-Fi boards):** Close the web dashboard browser tab during extended listening sessions to keep 2.4 GHz radio traffic minimal.

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
