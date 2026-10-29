#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <string.h>

#define WIFI_SSID          "DITT_WIFI_SSID"
#define WIFI_PASSWORD      "DITT_WIFI_LOSENORD"
#define WIFI_AP_NAME       "ESP32-Voice-Setup"
#define WS_HOST            "192.168.1.100"
#define WS_PORT            8080
#define WS_PATH            "/"

#define OLED_SDA           21
#define OLED_SCL           20
#define OLED_ADDR          0x3C

#define I2S_PORT_NUM       I2S_NUM_0
#define I2S_BCLK           GPIO_NUM_2
#define I2S_WS             GPIO_NUM_1
#define I2S_DAC_DIN        GPIO_NUM_3
#define I2S_MIC_SD         GPIO_NUM_8

#define SAMPLE_RATE        16000
#define I2S_DATA_BITS      I2S_DATA_BIT_WIDTH_16BIT
#define I2S_DMA_DESC       8
#define I2S_DMA_FRAMES     256
#define I2S_IO_BYTES       512

#define TX_RING_BYTES      16384
#define EYE_FRAME_MS       40
#define WS_RETRY_MS        5000
#define WIFI_WAIT_MS       10000
#define CONTINUOUS_STREAM  false

enum { NEUTRAL, HAPPY, ANGRY, SAD, SURPRISED, SUSPICIOUS, SLEEPY, LOVE, CONFUSED, MOOD_COUNT };

enum ListenState { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING };

static const char *MOOD_NAMES[MOOD_COUNT] = {
  "neutral", "happy", "angry", "sad", "surprised", "suspicious", "sleepy", "love", "confused"
};

struct MoodParams {
  uint8_t eyeH;
  uint8_t lidDrop;
  int8_t browTilt;
  uint8_t pupil;
  int8_t lookX;
  int8_t lookY;
  bool hearts;
};

static const MoodParams moodTable[MOOD_COUNT] = {
  { 16, 0,  0, 4,  0,  0, false },
  { 10, 6, -3, 4,  0, -2, false },
  {  8, 2,  4, 3,  0,  0, false },
  { 12, 3, -4, 4,  0,  2, false },
  { 20, 0,  0, 6,  0,  0, false },
  {  8, 6,  3, 3,  0,  0, false },
  {  4, 12, 0, 3,  0,  3, false },
  { 16, 0,  0, 0,  0,  0, true  },
  { 14, 2,  2, 4,  2,  0, false }
};

static uint8_t currentMood = NEUTRAL;
static uint8_t blinkLevel = 0;
static uint32_t blinkStartMs = 0;
static uint32_t nextBlinkMs = 0;
static uint32_t lastEyeMs = 0;

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

static WebSocketsClient ws;
static bool wsConnected = false;
static uint32_t lastWsRetryMs = 0;
static ListenState listenState = ST_IDLE;

static i2s_chan_handle_t i2sTx = NULL;
static i2s_chan_handle_t i2sRx = NULL;

static uint8_t txRing[TX_RING_BYTES];
static volatile size_t txHead = 0;
static volatile size_t txTail = 0;

static size_t ringUsed() {
  return (txHead - txTail) & (TX_RING_BYTES - 1);
}

static void ringPush(const uint8_t *data, size_t len) {
  size_t space = TX_RING_BYTES - 1 - ringUsed();
  if (len > space) len = space;
  for (size_t i = 0; i < len; i++) {
    txRing[txHead] = data[i];
    txHead = (txHead + 1) & (TX_RING_BYTES - 1);
  }
}

static size_t ringPop(uint8_t *dst, size_t len) {
  size_t avail = ringUsed();
  if (len > avail) len = avail;
  for (size_t i = 0; i < len; i++) {
    dst[i] = txRing[txTail];
    txTail = (txTail + 1) & (TX_RING_BYTES - 1);
  }
  return len;
}

void setMood(uint8_t moodIndex) {
  if (moodIndex >= MOOD_COUNT) moodIndex = NEUTRAL;
  currentMood = moodIndex;
}

static String jsonStringValue(const String &json, const char *key) {
  String k = "\"";
  k += key;
  k += "\"";
  int i = json.indexOf(k);
  if (i < 0) return "";
  int c = json.indexOf(':', i + k.length());
  if (c < 0) return "";
  int q1 = json.indexOf('"', c + 1);
  if (q1 < 0) return "";
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

static int moodFromName(const String &name) {
  for (int i = 0; i < MOOD_COUNT; i++) {
    if (name == MOOD_NAMES[i]) return i;
  }
  return -1;
}

static void applyStateStatus(const String &status) {
  if (status == "listening") {
    listenState = ST_LISTENING;
    setMood(NEUTRAL);
  } else if (status == "thinking") {
    listenState = ST_THINKING;
    setMood(CONFUSED);
  } else if (status == "speaking") {
    listenState = ST_SPEAKING;
  } else if (status == "idle") {
    listenState = ST_IDLE;
    setMood(NEUTRAL);
  } else if (status == "surprised") {
    setMood(SURPRISED);
  }
}

static void handleTextMessage(const uint8_t *payload, size_t length) {
  String json((const char *)payload, length);
  String event = jsonStringValue(json, "event");
  if (event == "state") {
    applyStateStatus(jsonStringValue(json, "status"));
  } else if (event == "mood") {
    String v = jsonStringValue(json, "value");
    int m = moodFromName(v);
    if (m >= 0) setMood((uint8_t)m);
  }
}

static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      wsConnected = false;
      break;
    case WStype_CONNECTED:
      wsConnected = true;
      break;
    case WStype_TEXT:
      handleTextMessage(payload, length);
      break;
    case WStype_BINARY:
      ringPush(payload, length);
      break;
    default:
      break;
  }
}

static void wsConnect() {
  ws.begin(WS_HOST, WS_PORT, WS_PATH);
  ws.onEvent(webSocketEvent);
  ws.setReconnectInterval(WS_RETRY_MS);
}

static void i2sInit() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = I2S_DMA_DESC;
  chanCfg.dma_frame_num = I2S_DMA_FRAMES;
  i2s_new_channel(&chanCfg, &i2sTx, &i2sRx);

  i2s_std_config_t stdCfg;
  memset(&stdCfg, 0, sizeof(stdCfg));
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BITS, I2S_SLOT_MODE_MONO);
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.bclk = I2S_BCLK;
  stdCfg.gpio_cfg.ws = I2S_WS;
  stdCfg.gpio_cfg.dout = I2S_DAC_DIN;
  stdCfg.gpio_cfg.din = I2S_MIC_SD;
  i2s_channel_init_std_mode(i2sTx, &stdCfg);
  i2s_channel_init_std_mode(i2sRx, &stdCfg);
  i2s_channel_enable(i2sTx);
  i2s_channel_enable(i2sRx);
}

static void wifiInit() {
  WiFi.mode(WIFI_STA);
  if (strcmp(WIFI_SSID, "DITT_WIFI_SSID") != 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_WAIT_MS) {
      delay(200);
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFiManager wm;
    wm.autoConnect(WIFI_AP_NAME);
  }
}

static void drawHeart(int16_t x, int16_t y, int16_t s) {
  u8g2.drawDisc(x - s / 2, y - s / 4, s / 2, U8G2_DRAW_ALL);
  u8g2.drawDisc(x + s / 2, y - s / 4, s / 2, U8G2_DRAW_ALL);
  u8g2.drawTriangle(x - s, y, x + s, y, x, y + s);
}

static void drawEye(int16_t cx, int16_t cy, const MoodParams &m, bool leftEye) {
  int16_t halfW = 11;
  int16_t eyeH = m.eyeH;
  int16_t top = cy - eyeH / 2;
  int16_t bottom = cy + eyeH / 2;

  u8g2.drawEllipse(cx, cy, halfW, eyeH / 2, U8G2_DRAW_ALL);

  if (m.hearts) {
    u8g2.setDrawColor(0);
    drawHeart(cx + m.lookX, cy + m.lookY, 7);
    u8g2.setDrawColor(1);
  } else {
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx + m.lookX, cy + m.lookY, m.pupil, U8G2_DRAW_ALL);
    u8g2.setDrawColor(1);
  }

  int16_t lid = m.lidDrop + blinkLevel;
  if (lid > 0) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(cx - halfW - 1, top - 1, (halfW + 1) * 2, lid);
    u8g2.setDrawColor(1);
  }

  int16_t browY = top - 6;
  int16_t tilt = m.browTilt;
  if (!leftEye) tilt = -tilt;
  int16_t innerX = leftEye ? cx + halfW - 2 : cx - halfW + 2;
  int16_t outerX = leftEye ? cx - halfW + 2 : cx + halfW - 2;
  u8g2.drawLine(outerX, browY - tilt, innerX, browY + tilt);
  u8g2.drawLine(outerX, browY - tilt + 1, innerX, browY + tilt + 1);

  u8g2.drawEllipse(cx, cy, halfW, eyeH / 2, U8G2_DRAW_ALL);
}

static void eyesUpdate() {
  uint32_t now = millis();

  if (blinkStartMs == 0 && now >= nextBlinkMs) {
    blinkStartMs = now;
    nextBlinkMs = now + 160 + random(2400, 5200);
  }
  if (blinkStartMs != 0) {
    uint32_t e = now - blinkStartMs;
    if (e >= 140) {
      blinkStartMs = 0;
      blinkLevel = 0;
    } else if (e < 70) {
      blinkLevel = 14;
    } else {
      blinkLevel = 14 * (140 - e) / 70;
    }
  }

  if (now - lastEyeMs < EYE_FRAME_MS) return;
  lastEyeMs = now;

  const MoodParams &m = moodTable[currentMood];

  u8g2.clearBuffer();
  drawEye(44, 34, m, true);
  drawEye(84, 34, m, false);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(4, 10);
  u8g2.print(MOOD_NAMES[currentMood]);
  if (listenState == ST_LISTENING) {
    u8g2.setCursor(96, 10);
    u8g2.print("REC");
  }
  u8g2.sendBuffer();
}

static void audioTxTask() {
  uint8_t buf[I2S_IO_BYTES];
  size_t n = ringPop(buf, sizeof(buf));
  if (n < sizeof(buf)) memset(buf + n, 0, sizeof(buf) - n);
  size_t written = 0;
  i2s_channel_write(i2sTx, buf, sizeof(buf), &written, 20);
}

static void audioRxTask() {
  uint8_t buf[I2S_IO_BYTES];
  size_t readBytes = 0;
  i2s_channel_read(i2sRx, buf, sizeof(buf), &readBytes, 10);
  if (readBytes > 0 && wsConnected && (listenState == ST_LISTENING || CONTINUOUS_STREAM)) {
    ws.sendBIN(buf, readBytes);
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.setI2CAddress(OLED_ADDR * 2);
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(10, 32);
  u8g2.print("Voice Client...");
  u8g2.sendBuffer();

  randomSeed(esp_random());
  nextBlinkMs = millis() + 2000;

  i2sInit();
  wifiInit();
  wsConnect();
}

void loop() {
  ws.loop();

  if (!wsConnected && millis() - lastWsRetryMs > WS_RETRY_MS) {
    lastWsRetryMs = millis();
    ws.disconnect();
    wsConnect();
  }

  audioTxTask();
  audioRxTask();
  eyesUpdate();
}
