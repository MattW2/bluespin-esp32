# BlueSpin Pi (Raspberry Pi 3B)

Bluetooth A2DP Sink to Sendspin `source@v1` bridge for **Raspberry Pi 3B** (built-in Wi-Fi + Bluetooth), using BlueZ + PipeWire/PulseAudio + `aiosendspin` (`48 kHz` stereo **Opus** by default, with automatic FLAC/PCM fallback) and an optional SSD1306 128×64 I²C OLED.

## Hardware Wiring (Optional SSD1306 128×64 I²C OLED)

| OLED Pin | Raspberry Pi 3B Pin |
| --- | --- |
| `VCC` | Pin 1 (`3.3V`) |
| `GND` | Pin 6 (`GND`) |
| `SDA` | Pin 3 (`GPIO 2` / I2C1 SDA) |
| `SCL` | Pin 5 (`GPIO 3` / I2C1 SCL) |

If no display is connected at `0x3C`, BlueSpin Pi logs that no OLED was detected and runs headlessly without error.

## Secrets & Deployment Setup

1. Copy `pi/deploy/secrets.env.example` to `pi/deploy/secrets.env` (gitignored):
   - Set `PI_HOST`, `PI_USER`, and either `PI_SSH_KEY` (recommended) or `PI_PASSWORD`.
2. Deploy from Windows PowerShell or Linux/macOS:
   - PowerShell: `.\pi\deploy\deploy.ps1`
   - Bash: `./pi/deploy/deploy.sh`
3. Or install directly on the Pi:
   ```bash
   sudo bash pi/scripts/install.sh
   ```

## Web Dashboard & Controls

Once running, open `http://<pi-ip>:8080/` to:
- Open/close the pinless Bluetooth pairing window
- Disconnect, reconnect, or forget paired phones
- View the 6-digit Music Assistant pairing code and Sendspin `source_id`
- Toggle `Sendspin unpaired access`
