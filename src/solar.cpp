// CYD solar: painel local 320x240 com toque XPT2046 e API /solar.json.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>

static const char *DEFAULT_API = "http://192.168.3.17:9881/solar.json";
static const uint32_t POLL_MS = 10000;
static const uint32_t STALE_MS = 30000;
static const int W = 320, H = 240;
static const int TOUCH_CS = 33, TOUCH_IRQ = 36;

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;
char apiUrl[160];

struct SolarData {
  bool ok = false;
  float power = NAN, today = NAN, total = NAN, month = NAN, year = NAN;
  int8_t daylight = -1, live = -1;
  int32_t history[49] = {0};
  uint8_t count = 0;
};
SolarData data;
uint32_t lastPoll = 0, lastGood = 0;
bool received = false;
TaskHandle_t fetchTaskHandle = nullptr;
QueueHandle_t dataQueue = nullptr;
uint8_t page = 0;
int selected = -1;
bool calibrated = false;
// Raw touch -> screen. Three reference points handle swapped or inverted axes.
float ax, bx, cx, ay, by, cy;

uint16_t BG, PANEL, LINE, TEXT, DIM, SUN, GREEN, RED;

static void palette() {
  BG = tft.color565(6, 11, 20);
  PANEL = tft.color565(14, 25, 39);
  LINE = tft.color565(37, 58, 74);
  TEXT = tft.color565(227, 238, 245);
  DIM = tft.color565(124, 150, 166);
  SUN = tft.color565(255, 191, 53);
  GREEN = tft.color565(46, 221, 153);
  RED = tft.color565(255, 89, 105);
}

static void label(const String &s, int x, int y, uint8_t font, uint16_t fg,
                  uint16_t bg = 0, uint8_t datum = TL_DATUM) {
  tft.setTextDatum(datum);
  tft.setTextFont(font);
  tft.setTextColor(fg, bg ? bg : BG);
  tft.drawString(s, x, y);
}

static String watts(float v) {
  if (!isfinite(v)) return "--";
  if (fabsf(v) >= 10000) return String(v / 1000.0f, 1) + " kW";
  return String((int)roundf(v)) + " W";
}

static String kwh(float v) {
  if (!isfinite(v)) return "--";
  return String(v, v >= 1000 ? 0 : 1) + " kWh";
}

static void button(int x, int y, int w, const char *title, bool active) {
  tft.fillRoundRect(x, y, w, 31, 6, active ? SUN : PANEL);
  tft.drawRoundRect(x, y, w, 31, 6, active ? SUN : LINE);
  label(title, x + w / 2, y + 8, 2, active ? BG : TEXT,
        active ? SUN : PANEL, TC_DATUM);
}

static void chrome() {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, W, 29, PANEL);
  tft.fillRect(0, 0, 5, 29, SUN);
  label("SOLAR", 12, 5, 2, TEXT, PANEL);
  bool stale = !received || !data.ok || millis() - lastGood > STALE_MS;
  bool online = WiFi.status() == WL_CONNECTED;
  const char *state = !online ? "SEM WIFI" : stale ? "SEM DADOS" :
      data.daylight == 0 ? "NOITE" : data.live == 0 ? "SEM SINAL" : "ATUALIZADO";
  uint16_t stateColor = !online ? RED : stale || data.live == 0 ? SUN : GREEN;
  label(state, 296, 6, 2, stateColor, PANEL, TR_DATUM);
  tft.fillCircle(307, 13, 4, stateColor);
  tft.drawFastHLine(0, 29, W, LINE);
  tft.drawFastHLine(0, 30, 95, SUN);
  tft.drawFastHLine(0, 200, W, LINE);
  button(6, 205, 103, "AGORA", page == 0);
  button(113, 205, 103, "24 HORAS", page == 1);
  button(220, 205, 94, "ATUALIZAR", false);
}

static void overview() {
  label(data.daylight == 0 ? "FORA DO HORARIO SOLAR" : "GERANDO AGORA", 12, 38, 2, DIM);
  bool kw = isfinite(data.power) && fabsf(data.power) >= 10000;
  String mainValue = !isfinite(data.power) ? "--" : kw
      ? String(data.power / 1000.0f, 1) : String((int)roundf(data.power));
  uint16_t powerColor = isfinite(data.power) && data.power > 0 ? SUN : TEXT;
  label(mainValue, 151, 52, 7, powerColor, BG, TC_DATUM);
  label(kw ? "kW" : "W", 243, 77, 4, powerColor);
  tft.drawFastHLine(10, 103, 300, LINE);
  label("HOJE", 13, 113, 2, DIM);
  label(kwh(data.today), 307, 110, 4, TEXT, BG, TR_DATUM);
  tft.drawFastHLine(10, 141, 300, LINE);
  label("TOTAL", 13, 151, 2, DIM);
  label(kwh(data.total), 307, 148, 4, TEXT, BG, TR_DATUM);
  tft.drawFastHLine(10, 179, 300, LINE);
  label("MES " + kwh(data.month), 12, 184, 2, DIM);
  label("ANO " + kwh(data.year), 307, 184, 2, DIM, BG, TR_DATUM);
}

static void chart() {
  label("POTENCIA / ULTIMAS 24H", 10, 37, 2, DIM);
  int peak = -1;
  for (int i = 0; i < data.count; ++i) peak = max(peak, (int)data.history[i]);
  label(peak < 0 ? "--" : watts((float)peak), 309, 37, 2, SUN, BG, TR_DATUM);
  const int gx = 12, gy = 70, gw = 296, gh = 92;
  for (int i = 0; i <= 2; ++i) {
    int yy = gy + i * gh / 2;
    for (int x = gx; x < gx + gw; x += 5) tft.drawPixel(x, yy, LINE);
  }
  if (peak >= 0) {
    for (int i = 0; i < data.count; ++i) {
      int x = gx + i * gw / data.count;
      int next = gx + (i + 1) * gw / data.count;
      if (data.history[i] < 0) continue;
      int h = (int)((float)data.history[i] * (gh - 3) / max(1, peak));
      tft.fillRect(x, gy + gh - h, max(1, next - x - 1), max(1, h),
                   i == selected ? GREEN : SUN);
    }
  } else {
    label("Sem historico", 160, 112, 2, DIM, BG, MC_DATUM);
  }
  tft.drawRect(gx - 1, gy - 1, gw + 2, gh + 2, LINE);
  label("-24h", gx, 168, 2, DIM);
  label("agora", gx + gw, 168, 2, DIM, BG, TR_DATUM);
  if (selected >= 0 && selected < data.count) {
    int minutesAgo = (data.count - 1 - selected) * 30;
    String age = String(minutesAgo / 60) + "h" + (minutesAgo % 60 ? "30" : "00");
    label(age + " atras  " + (data.history[selected] < 0 ? "sem leitura" : watts(data.history[selected])),
          160, 185, 2, GREEN, BG, TC_DATUM);
  } else {
    label("Toque numa barra para ver o valor", 160, 185, 2, DIM, BG, TC_DATUM);
  }
}

static void render() {
  chrome();
  if (page == 0) overview(); else chart();
}

static bool fetch(SolarData &next) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(6000);
  if (!http.begin(apiUrl)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) return false;

  next.ok = doc["ok"] | false;
  auto number = [](JsonVariant v) -> float { return v.isNull() ? NAN : v.as<float>(); };
  next.power = number(doc["p"]);
  next.today = number(doc["d"]);
  next.total = number(doc["t"]);
  next.month = number(doc["m"]);
  next.year = number(doc["y"]);
  next.daylight = doc["daylight"].isNull() ? -1 : doc["daylight"].as<int>();
  next.live = doc["live"].isNull() ? -1 : doc["live"].as<int>();
  for (JsonVariant v : doc["h"].as<JsonArray>()) {
    if (next.count >= 49) break;
    next.history[next.count++] = v.isNull() ? -1 : max(0, v.as<int>());
  }
  return true;
}

// HTTP runs separately so a slow server never blocks the touchscreen.
static void fetchWorker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    SolarData next;
    if (fetch(next)) xQueueOverwrite(dataQueue, &next);
  }
}

static void requestFetch() {
  lastPoll = millis();
  if (fetchTaskHandle) xTaskNotifyGive(fetchTaskHandle);
}

static bool rawTouch(TS_Point &p) {
  if (!touch.touched()) return false;
  p = touch.getPoint();
  return p.z > 100;
}

static void waitRelease() {
  while (touch.touched()) delay(20);
  delay(150);
}

static TS_Point calibrationPoint(int x, int y, const char *step) {
  Serial.println(step);
  tft.fillScreen(BG);
  label("CALIBRAR TOQUE", 160, 20, 4, TEXT, BG, TC_DATUM);
  label(step, 160, 61, 2, DIM, BG, TC_DATUM);
  tft.drawCircle(x, y, 14, SUN);
  tft.drawFastHLine(x - 20, y, 40, SUN);
  tft.drawFastVLine(x, y - 20, 40, SUN);
  TS_Point p;
  while (!rawTouch(p)) delay(30);
  waitRelease();
  return p;
}

static void calibrate() {
  if (prefs.getBool("cal", false)) {
    ax = prefs.getFloat("ax", 0); bx = prefs.getFloat("bx", 0);
    cx = prefs.getFloat("cx", 0); ay = prefs.getFloat("ay", 0);
    by = prefs.getFloat("by", 0); cy = prefs.getFloat("cy", 0);
    calibrated = true;
    return;
  }
  float det;
  do {
    TS_Point a = calibrationPoint(30, 95, "1/3: toque o alvo");
    TS_Point b = calibrationPoint(290, 95, "2/3: toque o alvo");
    TS_Point c = calibrationPoint(30, 185, "3/3: toque o alvo");
    float ux = b.x - a.x, uy = b.y - a.y;
    float vx = c.x - a.x, vy = c.y - a.y;
    det = ux * vy - uy * vx;
    if (fabsf(det) < 10000) continue;
    ax = (260 * vy) / det; bx = (-260 * vx) / det;
    ay = (-90 * uy) / det; by = (90 * ux) / det;
    cx = 30 - ax * a.x - bx * a.y;
    cy = 95 - ay * a.x - by * a.y;
  } while (fabsf(det) < 10000);
  prefs.putFloat("ax", ax); prefs.putFloat("bx", bx); prefs.putFloat("cx", cx);
  prefs.putFloat("ay", ay); prefs.putFloat("by", by); prefs.putFloat("cy", cy);
  prefs.putBool("cal", true);
  calibrated = true;
}

static void handleTouch() {
  if (!calibrated) return;
  TS_Point p;
  if (!rawTouch(p)) return;
  int x = constrain((int)(ax * p.x + bx * p.y + cx), 0, W - 1);
  int y = constrain((int)(ay * p.x + by * p.y + cy), 0, H - 1);
  uint32_t pressedAt = millis();
  waitRelease();
  if (y >= 200) {
    if (x < 110) page = 0;
    else if (x < 218) page = 1;
    else {
      if (millis() - pressedAt >= 5000) {
        prefs.remove("cal");
        ESP.restart();
      }
      requestFetch();
    }
    render();
  } else if (page == 1 && y >= 70 && y <= 164 && data.count) {
    selected = constrain((x - 12) * data.count / 296, 0, (int)data.count - 1);
    render();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nCYD Solar - toque + Prometheus");
  pinMode(4, OUTPUT); pinMode(16, OUTPUT); pinMode(17, OUTPUT);
  digitalWrite(4, HIGH); digitalWrite(16, HIGH); digitalWrite(17, HIGH);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  tft.init(); tft.setRotation(1); palette();
  touchSpi.begin(25, 39, 32, TOUCH_CS);
  touch.begin(touchSpi);
  prefs.begin("solar", false);
  calibrate();

  String saved = prefs.getString("api", DEFAULT_API);
  saved.toCharArray(apiUrl, sizeof(apiUrl));
  WiFiManager wm;
  WiFiManagerParameter apiParam("api", "URL do solar.json", apiUrl, sizeof(apiUrl) - 1);
  wm.addParameter(&apiParam);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  wm.setConnectRetries(3);
  tft.fillScreen(BG);
  label("Conectando WiFi", 160, 110, 4, SUN, BG, MC_DATUM);
  label("Se precisar, conecte ao AP CYD-Solar", 160, 150, 2, DIM, BG, MC_DATUM);
  if (!wm.autoConnect("CYD-Solar")) ESP.restart();
  if (strlen(apiParam.getValue())) {
    strlcpy(apiUrl, apiParam.getValue(), sizeof(apiUrl));
    prefs.putString("api", apiUrl);
  }
  WiFi.setAutoReconnect(true);
  Serial.printf("WiFi OK ip=%s\n", WiFi.localIP().toString().c_str());
  dataQueue = xQueueCreate(1, sizeof(SolarData));
  if (!dataQueue || xTaskCreate(fetchWorker, "solar-http", 8192, nullptr, 1, &fetchTaskHandle) != pdPASS) {
    Serial.println("Falha ao iniciar consulta solar");
    delay(3000);
    ESP.restart();
  }
  requestFetch();
  render();
  lastPoll = millis();
}

void loop() {
  handleTouch();
  SolarData next;
  if (xQueueReceive(dataQueue, &next, 0) == pdTRUE) {
    data = next;
    received = true;
    lastGood = millis();
    Serial.printf("SOLAR ok=%d dia=%.1f mes=%.1f total=%.1f daylight=%d\n",
                  data.ok, data.today, data.month, data.total, data.daylight);
    render();
  }
  if (millis() - lastPoll >= POLL_MS) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    requestFetch();
    render();
  }
  delay(30);
}
