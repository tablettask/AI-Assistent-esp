# ESP32-C3 SuperMini – WebSocket röstassistentklient

Firmware för ESP32-C3 SuperMini: I2S-mikrofon (INMP441), I2S-DAC (MAX98357A),
I2C OLED (SSD1306) med proceduriell ögonmotor, WiFi + WebSocket-klient.

## Hårdvara / pin-mapping

| Funktion | Pin |
|---|---|
| OLED SDA | GPIO 21 |
| OLED SCL | GPIO 20 |
| I2S BCLK (delad) | GPIO 2 |
| I2S WS/LRCL (delad) | GPIO 1 |
| MAX98357A DIN | GPIO 3 |
| INMP441 SD | GPIO 8 |

INMP441 L/R till GND (vänster kanal). SSD1306 på I2C-adress 0x3C.

## Konfiguration

Redigera konstanterna överst i `esp32c3-voice-client.ino`:

- `WIFI_SSID` / `WIFI_PASSWORD` – lämna platshållaren så startar WiFiManager ett setup-AP (`ESP32-Voice-Setup`).
- `WS_HOST`, `WS_PORT`, `WS_PATH` – WebSocket-servern.
- `CONTINUOUS_STREAM` – `true` strömmar mikrofonljud hela tiden, `false` bara när servern skickat `{"event":"state","status":"listening"}`.

## WebSocket-protokoll

- **ESP32 → server (binärt):** rå PCM 16 kHz, 16-bit, mono från INMP441.
- **Server → ESP32 (binärt):** PCM som spelas upp direkt på MAX98357A via en ringbuffer.
- **Server → ESP32 (text/JSON):**
  - `{"event":"state","status":"listening"}` → mood NEUTRAL, börjar strömma mikrofon
  - `{"event":"state","status":"thinking"}` → mood CONFUSED
  - `{"event":"state","status":"speaking"}` / `"idle"` → hanteras
  - `{"event":"mood","value":"happy"}` → mood enligt namn (`neutral, happy, angry, sad, surprised, suspicious, sleepy, love, confused`)

Humör kan också styras direkt i kod via `setMood(uint8_t)` med enum-index:
`NEUTRAL, HAPPY, ANGRY, SAD, SURPRISED, SUSPICIOUS, SLEEPY, LOVE, CONFUSED`.

## Bygg i Arduino IDE

1. Installera **Arduino IDE 2.x** och kortpaketet **esp32** (Espressif, URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`).
2. Välj bräda: **"ESP32C3 Dev Module"** (SuperMini är en C3). Ställ in t.ex. Flash Mode QIO, Partition Scheme "Huge APP".
3. Installera bibliotek via Library Manager:
   - **U8g2** (oliver)
   - **WebSockets** av Markus Sattler (innehåller `WebSocketsClient`)
   - **WiFiManager** av tzapu
4. Öppna `esp32c3-voice-client.ino`, fyll i WiFi/WebSocket-konstanter, klicka **Verify/Upload**.

## Exportera .bin för webbläsarflashning (web.esphome.io / ESP Web Tools)

Arduino IDE 2.x: **Sketch → Export compiled Binary** (Ctrl+Alt+S). Filen hamnar i sketch-mappen som `esp32c3-voice-client.ino.esp32c3.bin` (beroende på valt kort).

Alternativt via kommandorad (esptool krävs inte för export):

```
arduino-cli compile --fqbn esp32:esp32:esp32c3 --export-binaries /path/to/esp32c3-voice-client
```

## Flashning i webbläsaren

1. Kopiera den exporterade `.bin`-filen till en webbserver (t.ex. GitHub Pages) tillsammans med `manifest.json` och `install.html` i denna mapp.
2. Uppdatera `url` i `manifest.json` till var `.bin`-filen ligger.
3. Öppna `install.html` i Chrome/Edge över HTTPS, anslut ESP32-C3 via USB och klicka på install-knappen (ESP Web Tools). Välj COM-port och flash.

Efter första flashningen: om WiFi-konstanterna inte är ifyllda startar ett WiFi-AP `ESP32-Voice-Setup` – anslut och ange ditt WiFi.

## Felsökning

- Ingen ljud ut/in: kontrollera att BCLK/WS verkligen delas (GPIO 2 / GPIO 1) och att INMP441 L/R är GND.
- WebSocket kopplar ner: kontrollera `WS_HOST`/`WS_PORT` och att servern accepterar binära ramar.
- Ögonen frystes: `eyesUpdate()` körs icke-blockerande i `loop()`; I2S använder timeouts (10–20 ms) så ljudet ska inte blockeras.
