// Monitor de saude do servidor numa CYD (ESP32-2432S028R, 320x240 landscape).
//
// Puxa http://<host>:9881/status.json (servido por grafana/exporters/
// tft_dashboard_api.py) e desenha nativamente. Nao renderiza imagem do Grafana
// de proposito: 320x240 e pequeno demais para um painel do Grafana continuar
// legivel, e o render headless custaria Chromium a cada refresh num host que
// vive com ~150MB livres.
//
// WiFi e URL da API sao configurados por portal captivo no primeiro boot
// (AP "CYD-Monitor"), entao nao ha credencial no codigo.
//
// Visual: HUD escuro com acento ciano. Os numeros grandes usam a fonte 7
// (7-segmentos) desenhada sobre "888" apagado, do jeito que um instrumento real
// se comporta -- os segmentos nao acesos continuam visiveis em tom escuro.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiManager.h>

// ---------------------------------------------------------------------------
// Configuracao
// ---------------------------------------------------------------------------
static const char *AP_NAME = "CYD-Monitor";
static const char *DEFAULT_API = "http://192.168.3.17:9881/status.json";
static const uint32_t HTTP_TIMEOUT_MS = 6000;
// Depois de quantos ms sem uma leitura boa a tela passa a se declarar velha.
static const uint32_t STALE_AFTER_MS = 30000;

// LED RGB embutido da CYD -- ativo em LOW. Sem isto ele fica aceso de forma
// aleatoria no boot e ilumina a mesa inteira.
static const int PIN_LED_R = 4;
static const int PIN_LED_G = 16;
static const int PIN_LED_B = 17;

#define SPARK_MAX 60

// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
WiFiManager wm;
char apiUrl[128] = {0};

struct Stats {
  bool ok = false;
  uint32_t uptime = 0;
  float cpu = NAN;
  int cores = 0;
  float load[3] = {NAN, NAN, NAN};
  float temp = NAN;
  float memUsed = NAN, memTotal = NAN;
  float swapUsed = NAN, swapTotal = NAN;
  // 0 = raiz, 1 = backup
  char diskName[2][12] = {{0}, {0}};
  float diskUsed[2] = {NAN, NAN};
  float diskTotal[2] = {NAN, NAN};
  int diskCount = 0;
  uint8_t spark[SPARK_MAX];
  int sparkCount = 0;
};

Stats stats;
uint32_t lastPollMs = 0;
uint32_t lastGoodMs = 0;

// ---------------------------------------------------------------------------
// Paleta -- dark azulado com acento ciano
// ---------------------------------------------------------------------------
uint16_t C_BG, C_PANEL, C_LINE, C_TEXT, C_DIM, C_OK, C_WARN, C_CRIT;
uint16_t C_ACCENT, C_BRACKET, C_GHOST, C_SPARKFILL;

static void initPalette() {
  C_BG        = tft.color565(5, 7, 13);      // quase preto, viés azul
  C_PANEL     = tft.color565(12, 17, 28);
  C_LINE      = tft.color565(30, 42, 60);
  C_TEXT      = tft.color565(200, 214, 229);
  C_DIM       = tft.color565(85, 103, 125);
  C_ACCENT    = tft.color565(0, 229, 255);   // ciano
  C_BRACKET   = tft.color565(0, 82, 102);    // ciano rebaixado p/ cantoneiras
  C_GHOST     = tft.color565(18, 26, 38);    // segmento apagado / LED apagado
  C_SPARKFILL = tft.color565(10, 46, 60);
  C_OK        = tft.color565(0, 224, 138);
  C_WARN      = tft.color565(255, 179, 0);
  C_CRIT      = tft.color565(255, 59, 92);
}

// Verde -> ambar -> vermelho conforme a fracao de uso.
static uint16_t levelColor(float pct) {
  if (isnan(pct)) return C_DIM;
  if (pct >= 90.0f) return C_CRIT;
  if (pct >= 75.0f) return C_WARN;
  return C_OK;
}

// ---------------------------------------------------------------------------
// Geometria (320x240)
// ---------------------------------------------------------------------------
static const int W = 320, H = 240;
static const int HDR_H = 24;

// Dois cards de HUD lado a lado com os numeros grandes.
static const int CARD_Y = 28, CARD_H = 74;          // 28..102
static const int CARD_W = 154;
static const int CARD_L_X = 4, CARD_R_X = 162;
static const int HERO_Y = 46;                        // fonte 7 (48px) 46..94

// Sprite dos numeros grandes (evita piscada ao repintar).
static const int HERO_SPR_W = 150, HERO_SPR_H = 50;
static const int HERO_NUM_R = 112;                   // borda direita do numero
static const int HERO_UNIT_X = 116;

static const int SPARK_X = 8, SPARK_Y = 106;
static const int SPARK_W = 304, SPARK_H = 30;        // 106..136

static const int ROWS_Y = 143, ROW_H = 18;           // 143,161,179,197 (fim 213)
static const int LBL_X = 8;
static const int BAR_X = 48, BAR_SEGS = 14, BAR_SEGW = 7, BAR_GAP = 2, BAR_H = 10;
static const int COL_TEXT_R = 262;                   // "usado/total"
static const int FOOT_DIV = 217, FOOT_Y = 222;

TFT_eSprite heroSpr = TFT_eSprite(&tft);
TFT_eSprite sparkSpr = TFT_eSprite(&tft);

// ---------------------------------------------------------------------------
// Helpers de desenho
// ---------------------------------------------------------------------------

// Texto com espacamento extra entre letras -- o TFT_eSPI nao faz isso nativo,
// e e o que da o ar "tech" aos titulos em caixa alta.
static void drawSpaced(const char *s, int x, int y, int extra, uint8_t font,
                       uint16_t fg, uint16_t bg) {
  tft.setTextFont(font);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(fg, bg);
  char buf[2] = {0, 0};
  int cx = x;
  for (const char *p = s; *p; p++) {
    buf[0] = *p;
    tft.drawString(buf, cx, y);
    cx += tft.textWidth(buf) + extra;
  }
}

// Cantoneiras em L nos quatro cantos -- moldura de HUD sem fechar a caixa.
static void drawBrackets(int x, int y, int w, int h, int len, uint16_t c) {
  tft.drawFastHLine(x, y, len, c);
  tft.drawFastVLine(x, y, len, c);
  tft.drawFastHLine(x + w - len, y, len, c);
  tft.drawFastVLine(x + w - 1, y, len, c);
  tft.drawFastHLine(x, y + h - 1, len, c);
  tft.drawFastVLine(x, y + h - len, len, c);
  tft.drawFastHLine(x + w - len, y + h - 1, len, c);
  tft.drawFastVLine(x + w - 1, y + h - len, len, c);
}

// Barra de LEDs: segmentos acesos na cor do nivel, apagados em C_GHOST.
static void drawSegBar(int x, int y, float pct, uint16_t col) {
  int lit = 0;
  if (!isnan(pct)) {
    lit = (int)((pct / 100.0f) * BAR_SEGS + 0.5f);
    if (pct > 0 && lit < 1) lit = 1;      // 1% ainda acende um segmento
    if (lit > BAR_SEGS) lit = BAR_SEGS;
  }
  for (int i = 0; i < BAR_SEGS; i++) {
    tft.fillRect(x + i * (BAR_SEGW + BAR_GAP), y, BAR_SEGW, BAR_H,
                 i < lit ? col : C_GHOST);
  }
}

static void drawValueRight(const String &s, int right, int y, uint16_t color,
                           uint8_t font, int padWidth) {
  tft.setTextFont(font);
  tft.setTextColor(color, C_BG);
  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(padWidth);
  tft.drawString(s, right, y);
  tft.setTextPadding(0);
}

static void drawValueLeft(const String &s, int x, int y, uint16_t color,
                          uint8_t font, int padWidth) {
  tft.setTextFont(font);
  tft.setTextColor(color, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(padWidth);
  tft.drawString(s, x, y);
  tft.setTextPadding(0);
}

static String fmtUptime(uint32_t s) {
  uint32_t d = s / 86400;
  uint32_t h = (s % 86400) / 3600;
  uint32_t m = (s % 3600) / 60;
  char buf[24];
  if (d > 0) snprintf(buf, sizeof(buf), "%lud %luh", (unsigned long)d, (unsigned long)h);
  else if (h > 0) snprintf(buf, sizeof(buf), "%luh %lum", (unsigned long)h, (unsigned long)m);
  else snprintf(buf, sizeof(buf), "%lum", (unsigned long)m);
  return String(buf);
}

// Uma casa decimal so enquanto o numero e pequeno: "746.1/1876.7G" nao cabe na
// coluna, "746/1877G" cabe.
static String fmtCompact(float v) {
  if (isnan(v)) return "--";
  char buf[16];
  snprintf(buf, sizeof(buf), v >= 100.0f ? "%.0f" : "%.1f", v);
  return String(buf);
}

static String fmt1(float v) {
  if (isnan(v)) return "--";
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", v);
  return String(buf);
}

// ---------------------------------------------------------------------------
// Tela de auto-teste: confirma driver, rotacao, ordem RGB e inversao.
// ---------------------------------------------------------------------------
#if SHOW_BOOT_SELFTEST
static void bootSelfTest() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("AUTO-TESTE DE TELA", 8, 6);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("1) O fundo esta PRETO?  nao -> inversao", 8, 176);
  tft.drawString("2) Cada barra bate com o nome nela?", 8, 194);
  tft.drawString("   nao -> ordem RGB trocada", 8, 212);

  struct { const char *name; uint16_t color; uint16_t label; } bars[] = {
      {"VERMELHO", TFT_RED, TFT_WHITE},
      {"VERDE", TFT_GREEN, TFT_BLACK},
      {"AZUL", TFT_BLUE, TFT_WHITE},
      {"BRANCO", TFT_WHITE, TFT_BLACK},
  };
  const int bw = 76, bx0 = 8, by = 34, bh = 130;
  for (int i = 0; i < 4; i++) {
    int x = bx0 + i * (bw + 2);
    tft.fillRect(x, by, bw, bh, bars[i].color);
    tft.setTextColor(bars[i].label, bars[i].color);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(bars[i].name, x + bw / 2, by + bh / 2);
  }
  tft.setTextDatum(TL_DATUM);
  delay(5000);
}
#endif

// ---------------------------------------------------------------------------
// Layout estatico -- desenhado uma vez; o loop so repinta os valores.
// ---------------------------------------------------------------------------
static const char *ROW_LABELS[4] = {"RAM", "SWAP", "/", "BKP"};

static void drawStaticChrome() {
  tft.fillScreen(C_BG);

  // --- cabecalho: barra de acento + titulo espacado ---
  tft.fillRect(0, 0, W, HDR_H, C_PANEL);
  tft.fillRect(0, 4, 4, HDR_H - 8, C_ACCENT);
  drawSpaced("SERVIDOR", 12, 4, 2, 2, C_TEXT, C_PANEL);
  tft.drawFastHLine(0, HDR_H, W, C_LINE);
  tft.drawFastHLine(0, HDR_H + 1, 120, C_ACCENT);  // realce parcial

  // --- cards dos numeros grandes ---
  drawBrackets(CARD_L_X, CARD_Y, CARD_W, CARD_H, 11, C_BRACKET);
  drawBrackets(CARD_R_X, CARD_Y, CARD_W, CARD_H, 11, C_BRACKET);
  drawSpaced("CPU", CARD_L_X + 10, CARD_Y + 6, 2, 1, C_DIM, C_BG);
  drawSpaced("TEMP", CARD_R_X + 10, CARD_Y + 6, 2, 1, C_DIM, C_BG);

  // --- rotulos das linhas de barra ---
  for (int i = 0; i < 4; i++) {
    drawSpaced(ROW_LABELS[i], LBL_X, ROWS_Y + i * ROW_H + 1, 1, 2, C_DIM, C_BG);
  }

  // --- rodape ---
  tft.drawFastHLine(0, FOOT_DIV, W, C_LINE);
  drawSpaced("LOAD", LBL_X, FOOT_Y, 1, 2, C_DIM, C_BG);
}

// Numero grande em 7 segmentos sobre "888" apagado. O valor e desenhado com
// fundo transparente para que os segmentos nao acesos do proprio digito
// continuem visiveis -- e isso que faz parecer instrumento de verdade.
static void drawHero(int sx, const String &val, const char *unit, uint16_t col) {
  heroSpr.fillSprite(C_BG);

  heroSpr.setTextDatum(TR_DATUM);
  heroSpr.setTextFont(7);
  heroSpr.setTextColor(C_GHOST, C_BG);
  heroSpr.drawString("888", HERO_NUM_R, 0);

  heroSpr.setTextColor(col);  // argumento unico = fundo transparente
  heroSpr.drawString(val, HERO_NUM_R, 0);

  heroSpr.setTextDatum(TL_DATUM);
  heroSpr.setTextFont(4);
  heroSpr.setTextColor(col, C_BG);
  heroSpr.drawString(unit, HERO_UNIT_X, 22);

  heroSpr.pushSprite(sx, HERO_Y);
}

static void drawSparkline() {
  sparkSpr.fillSprite(C_BG);
  const int w = SPARK_W, h = SPARK_H;

  // Grade de fundo: 25/50/75% na horizontal, marcas a cada 10 colunas.
  for (int f = 1; f <= 3; f++) {
    int gy = h - (h * f) / 4;
    for (int x = 0; x < w; x += 5) sparkSpr.drawPixel(x, gy, C_LINE);
  }

  if (stats.sparkCount >= 2) {
    float step = (float)w / (float)stats.sparkCount;
    for (int i = 0; i < stats.sparkCount; i++) {
      int v = stats.spark[i];
      int bh = (int)((v / 100.0f) * h + 0.5f);
      if (bh < 2) bh = 2;
      if (bh > h) bh = h;
      int bx = (int)(i * step);
      int bw = (int)step;
      if (bw < 1) bw = 1;
      // Area preenchida em tom frio + topo aceso na cor do nivel: le como
      // grafico de area com linha, mas custa dois fillRect por coluna.
      sparkSpr.fillRect(bx, h - bh, bw, bh, C_SPARKFILL);
      sparkSpr.fillRect(bx, h - bh, bw, 2, levelColor((float)v));
    }
  }

  sparkSpr.drawRect(0, 0, w, h, C_LINE);
  sparkSpr.pushSprite(SPARK_X, SPARK_Y);
}

static void drawDynamic() {
  // --- CPU e TEMP ---
  // A fonte 7 tem SOMENTE "1234567890:-." -- sem '%' e sem 'C'. Por isso a
  // unidade vai separada, na fonte 4.
  String cpuStr = isnan(stats.cpu) ? "-" : String((int)(stats.cpu + 0.5f));
  drawHero(CARD_L_X + 2, cpuStr, "%", levelColor(stats.cpu));

  // Acima de 85C este N4000 ja esta em throttle termico.
  uint16_t tCol = C_DIM;
  if (!isnan(stats.temp)) {
    if (stats.temp >= 85.0f) tCol = C_CRIT;
    else if (stats.temp >= 75.0f) tCol = C_WARN;
    else tCol = C_OK;
  }
  String tStr = isnan(stats.temp) ? "-" : String((int)(stats.temp + 0.5f));
  drawHero(CARD_R_X + 2, tStr, "C", tCol);

  drawSparkline();

  // --- Quatro linhas de barra ---
  struct RowData { float used, total; };
  RowData rows[4] = {
      {stats.memUsed, stats.memTotal},
      {stats.swapUsed, stats.swapTotal},
      {stats.diskUsed[0], stats.diskTotal[0]},
      {stats.diskUsed[1], stats.diskTotal[1]},
  };

  for (int i = 0; i < 4; i++) {
    int y = ROWS_Y + i * ROW_H;
    float used = rows[i].used, total = rows[i].total;
    float pct = (isnan(used) || isnan(total) || total <= 0) ? NAN : (used / total * 100.0f);
    uint16_t col = levelColor(pct);

    drawSegBar(BAR_X, y + 3, pct, col);

    String txt = (isnan(used) || isnan(total))
                     ? "--"
                     : fmtCompact(used) + "/" + fmtCompact(total) + "G";
    drawValueRight(txt, COL_TEXT_R, y, C_TEXT, 2, 84);
    String pctTxt = isnan(pct) ? "--" : String((int)(pct + 0.5f)) + "%";
    drawValueRight(pctTxt, W - 8, y, col, 2, 44);
  }

  // --- Rodape: load average + nucleos ---
  // Load acima do numero de cores significa fila de execucao -- aqui sao 2.
  uint16_t loadCol = C_OK;
  if (!isnan(stats.load[0]) && stats.cores > 0) {
    float ratio = stats.load[0] / (float)stats.cores;
    if (ratio >= 2.0f) loadCol = C_CRIT;
    else if (ratio >= 1.0f) loadCol = C_WARN;
  }
  String loadTxt = fmt1(stats.load[0]) + "  " + fmt1(stats.load[1]) + "  " + fmt1(stats.load[2]);
  drawValueLeft(loadTxt, 54, FOOT_Y, loadCol, 2, 140);

  String coresTxt = stats.cores > 0 ? String(stats.cores) + " CORES" : "";
  drawValueRight(coresTxt, W - 8, FOOT_Y, C_DIM, 2, 80);
}

static void drawHeaderStatus() {
  bool stale = (millis() - lastGoodMs) > STALE_AFTER_MS;
  bool wifiUp = WiFi.status() == WL_CONNECTED;

  uint16_t dot = C_OK;
  const char *msg = "";
  if (!wifiUp) { dot = C_CRIT; msg = "SEM WIFI"; }
  else if (stale) { dot = C_WARN; msg = "SEM DADOS"; }

  tft.setTextFont(2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor((stale || !wifiUp) ? dot : C_DIM, C_PANEL);
  tft.setTextPadding(150);
  String right = String(msg);
  if (!right.length() && stats.ok) right = "up " + fmtUptime(stats.uptime);
  tft.drawString(right, W - 22, 4);
  tft.setTextPadding(0);

  // LED de status: nucleo aceso + anel escuro, para nao virar um ponto chapado.
  tft.fillCircle(W - 11, 12, 5, C_GHOST);
  tft.fillCircle(W - 11, 12, 3, dot);
  tft.setTextDatum(TL_DATUM);
}

static void drawMessage(const char *line1, const char *line2) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString(line1, W / 2, H / 2 - 16);
  if (line2) {
    tft.setTextFont(2);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(line2, W / 2, H / 2 + 16);
  }
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------------
// Rede
// ---------------------------------------------------------------------------
static bool fetchStats() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(apiUrl)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d em %s\n", code, apiUrl);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("JSON invalido: %s\n", err.c_str());
    return false;
  }

  Stats s;
  s.ok = doc["ok"] | false;
  s.uptime = doc["up"] | 0;
  s.cpu = doc["cpu"].isNull() ? NAN : doc["cpu"].as<float>();
  s.cores = doc["cores"] | 0;
  s.temp = doc["temp"].isNull() ? NAN : doc["temp"].as<float>();
  for (int i = 0; i < 3; i++) {
    JsonVariant v = doc["load"][i];
    s.load[i] = v.isNull() ? NAN : v.as<float>();
  }
  s.memUsed = doc["mem"][0].isNull() ? NAN : doc["mem"][0].as<float>();
  s.memTotal = doc["mem"][1].isNull() ? NAN : doc["mem"][1].as<float>();
  s.swapUsed = doc["swap"][0].isNull() ? NAN : doc["swap"][0].as<float>();
  s.swapTotal = doc["swap"][1].isNull() ? NAN : doc["swap"][1].as<float>();

  JsonArray disks = doc["disk"].as<JsonArray>();
  int di = 0;
  for (JsonVariant d : disks) {
    if (di >= 2) break;
    strlcpy(s.diskName[di], d[0] | "", sizeof(s.diskName[di]));
    s.diskUsed[di] = d[1].isNull() ? NAN : d[1].as<float>();
    s.diskTotal[di] = d[2].isNull() ? NAN : d[2].as<float>();
    di++;
  }
  s.diskCount = di;

  JsonArray sp = doc["spark"].as<JsonArray>();
  int si = 0;
  for (JsonVariant v : sp) {
    if (si >= SPARK_MAX) break;
    int iv = v | 0;
    s.spark[si++] = (uint8_t)constrain(iv, 0, 100);
  }
  s.sparkCount = si;

  stats = s;
  lastGoodMs = millis();
  return true;
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nCYD server monitor");

  // LED RGB apagado (ativo em LOW).
  pinMode(PIN_LED_R, OUTPUT); digitalWrite(PIN_LED_R, HIGH);
  pinMode(PIN_LED_G, OUTPUT); digitalWrite(PIN_LED_G, HIGH);
  pinMode(PIN_LED_B, OUTPUT); digitalWrite(PIN_LED_B, HIGH);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);  // landscape 320x240
  initPalette();

#if SHOW_BOOT_SELFTEST
  bootSelfTest();
#endif

  heroSpr.setColorDepth(16);
  heroSpr.createSprite(HERO_SPR_W, HERO_SPR_H);
  sparkSpr.setColorDepth(16);
  sparkSpr.createSprite(SPARK_W, SPARK_H);

  // Confere no serial que "888" na fonte 7 cabe a esquerda de HERO_NUM_R --
  // se estourar, o digito mais a esquerda sai cortado na borda do sprite.
  heroSpr.setTextFont(7);
  Serial.printf("fonte7 \"888\"=%dpx alt=%dpx | sobra a esquerda=%dpx\n",
                heroSpr.textWidth("888"), heroSpr.fontHeight(),
                HERO_NUM_R - heroSpr.textWidth("888"));
  Serial.printf("heap livre=%u bytes\n", (unsigned)ESP.getFreeHeap());

  drawMessage("Conectando ao WiFi", AP_NAME);

  strlcpy(apiUrl, DEFAULT_API, sizeof(apiUrl));
  WiFiManagerParameter apiParam("api", "URL do status.json", apiUrl, sizeof(apiUrl) - 1);
  wm.addParameter(&apiParam);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  // O AP as vezes recusa a associacao na primeira tentativa ("Association
  // refused temporarily", com um comeback time estourado -- bug conhecido do
  // ESP32). Sem retry, um tropeco desses jogava a placa no portal de
  // configuracao por 3 minutos mesmo com a credencial certa gravada.
  wm.setConnectRetries(3);

  if (!wm.autoConnect(AP_NAME)) {
    // Sem WiFi apos o portal expirar: reinicia em vez de ficar preso no AP,
    // para que uma queda temporaria do roteador se resolva sozinha.
    drawMessage("WiFi falhou", "reiniciando...");
    delay(3000);
    ESP.restart();
  }
  strlcpy(apiUrl, apiParam.getValue(), sizeof(apiUrl));
  if (strlen(apiUrl) == 0) strlcpy(apiUrl, DEFAULT_API, sizeof(apiUrl));

  Serial.printf("WiFi OK  ip=%s  api=%s\n", WiFi.localIP().toString().c_str(), apiUrl);

  drawStaticChrome();
  lastGoodMs = millis();
  if (fetchStats()) drawDynamic();
  drawHeaderStatus();
  lastPollMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    drawHeaderStatus();
    delay(2000);
    return;
  }

  if (millis() - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = millis();
    if (fetchStats()) drawDynamic();
    drawHeaderStatus();
  }
  delay(50);
}
