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

## Konfiguration – konstanter att fylla i

Redigera konstanterna överst i `esp32c3-voice-client.ino` innan du kompilerar:

| Konst | Exempel | Förklaring |
|---|---|---|
| `WIFI_SSID` | `"MittWifi"` | Ditt WiFi-namn. Lämna `"DITT_WIFI_SSID"` så startar enheten i stället WiFiManager-läge (AP `ESP32-Voice-Setup`) där du anger WiFi via telefonen. |
| `WIFI_PASSWORD` | `"mittlosenord"` | WiFi-lösenord (tom sträng för öppet nät). |
| `WS_HOST` | `"192.168.1.50"` | IP-adress (eller hostname) till din WebSocket-server, t.ex. datorn som kör röstassistent-servern. Hitta den med `ipconfig` (Windows) / `ip addr` (Linux) på serverdatorn. |
| `WS_PORT` | `8080` | Port som servern lyssnar på (ofta 8080, 3000 eller 8888). |
| `WS_PATH` | `"/"` | WebSocket-sökväg, ofta `/` eller t.ex. `/ws`. |
| `CONTINUOUS_STREAM` | `false` | `true` = mikrofonen strömmas hela tiden; `false` = bara när servern skickat `{"event":"state","status":"listening"}`. |

Tips: om servern kör på samma nät, använd dess lokala IP (inte `127.0.0.1` – det pekar på ESP32 själv). Se till att brandväggen tillåter porten på serverdatorn.

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

Arduino IDE 2.x: **Sketch → Export compiled Binary** (Ctrl+Alt+S). I sketch-mappens `build/`-mapp skapas då flera filer – använd den **sammanfogade** varianten:

- `esp32c3-voice-client.ino.merged.bin` – komplett firmware (bootloader + partitioner + app) som flashas på offset `0x0`. **Rekommenderas** för webbflashning.
- `esp32c3-voice-client.ino.esp32c3.bin` – bara applikationen (offset `0x10000`), behöver kombineras med bootloader/partitioner.

Alternativt via kommandorad:

```
arduino-cli compile --fqbn esp32:esp32:esp32c3 --export-binaries /path/to/esp32c3-voice-client
# eller bygg merged manuellt:
esptool --chip esp32c3 merge_bin -o firmware-merged.bin --flash_mode dio --flash_freq 40m --flash_size 4MB 0x0 firmware-merged.bin
```

## Flashning i webbläsaren

### Alternativ A – direkt från web.esphome.io (fungerar!)

web.esphome.io låter dig peka på **din egen manifest-fil** via URL-parametern `?manifest=`:

1. Aktivera **GitHub Pages** för detta repo: Settings → Pages → Source: `Deploy from a branch` → välj din gren (t.ex. `vibe/esp32c3-voice-client-528c91`) och mappen `/esp32c3-voice-client` (eller `/` om du flyttar filerna till roten).
2. Ladda upp den exporterade `.merged.bin` i samma mapp som `manifest.json` (committa bin-filen till repot, eller lägg i GitHub Releases och uppdatera `path` i manifestet).
3. Öppna i Chrome/Edge:

```
https://web.esphome.io/?manifest=https://tablettask.github.io/AI-Assistent-esp/esp32c3-voice-client/manifest.json
```

4. Anslut ESP32-C3 SuperMini via USB, klicka **Connect**, välj COM-porten och sedan **Install**.

Obs: web.esphome.io kräver att manifestet ligger på en publik HTTPS-adress (GitHub Pages fungerar) och att sidan öppnas i Chrome/Edge/Opera (Web Serial stöds inte i Firefox/Safari/iOS).

### Alternativ B – egen installationssida

1. Kopiera `manifest.json` + `install.html` + din `.merged.bin` till en webbserver (t.ex. GitHub Pages).
2. Uppdatera `path` i `manifest.json` till var `.bin`-filen ligger (relativ sökväg till manifestet).
3. Öppna `install.html` i Chrome/Edge över HTTPS, anslut ESP32-C3 via USB och klicka på install-knappen.

Efter första flashningen: om WiFi-konstanterna inte är ifyllda startar ett WiFi-AP `ESP32-Voice-Setup` – anslut och ange ditt WiFi.

## Felsökning

### Webbflashning: "Serial data stream stopped: Possible serial noise or corruption" / "Failed to connect"

Detta är nästan alltid ett **anslutningsproblem, inte ett fel på .bin-filen**. Prova i denna ordning:

1. **Stäng Serial Monitor/Plotter i Arduino IDE** (och andra program som håller COM-porten, t.ex. PuTTY/Tera Term). Porten måste vara ledig.
2. **Rätt COM-port**: i Chrome/Edge, välj den port som dyker upp när du kopplar i USB (kolla Enhetshanteraren – ska vara t.ex. "USB-SERIAL CH340"/"CH343" eller "USB JTAG/serial debug unit"). Välj INTE Bluetooth-porten.
3. **Sätt ESP32-C3 i download-läge manuellt** (SuperMini med CH340/CH343-brygga gör inte alltid detta automatiskt):
   - Håll in **BOOT**-knappen (GPIO9), tryck kort på **RESET/EN**, släpp BOOT. Alternativt: håll BOOT och klicka "Connect"/"Install" i webbläsaren, släpp när synkningen börjat.
4. **Byt USB-kabel och port**: använd en riktig datakabel (vissa är bara laddkablar) och koppla direkt i datorn (inte via hubb).
5. **Sänk baud rate**: i install-dialogen, välj 115200 i stället för 921600.
6. **Rätt webbläsare**: Chrome, Edge eller Opera över HTTPS. Fungerar inte i Firefox/Safari/iOS (saknar Web Serial).
7. **Drivrutin**: installera CH340/CH343 eller CP210x-drivrutin om porten saknas i Enhetshanteraren.

### Övrigt
- Ingen ljud ut/in: kontrollera att BCLK/WS verkligen delas (GPIO 2 / GPIO 1) och att INMP441 L/R är GND.
- WebSocket kopplar ner: kontrollera `WS_HOST`/`WS_PORT` och att servern accepterar binära ramar.
- Ögonen frystes: `eyesUpdate()` körs icke-blockerande i `loop()`; I2S använder timeouts (10–20 ms) så ljudet ska inte blockeras.
