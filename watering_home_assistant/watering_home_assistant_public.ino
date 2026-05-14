#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <PubSubClient.h>

// =========================
// Konfiguráció
// A Wi-Fi, NTP és időjárási lokáció alapadatai.
// A személyes/hálózati adatokat publikus GitHub repóban nem szabad kitölteni.
// =========================

// IDE ÍRD A SAJÁT WIFI ADATAIDAT
const char *STA_SSID = "";              // Saját Wi-Fi hálózat neve. 
const char *STA_PASSWORD = "";          // Saját Wi-Fi jelszó.

const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";
const char *TZ_INFO      = "CET-1CEST,M3.5.0/2,M10.5.0/3";

const float WEATHER_LAT = 0.0f;       // Saját helyszín szélességi koordinátája.
const float WEATHER_LON = 0.0f;       // Saját helyszín hosszúsági koordinátája.

// =========================
// MQTT / Okosotthon integráció
// A Home Assistant / Mosquitto kapcsolat alapbeállításai és a fizikai I/O lábak.
// =========================
const char *MQTT_HOST             = "";   // Home Assistant / Mosquitto broker IP-címe.
const uint16_t MQTT_PORT          = 1883;
const char *MQTT_USER             = "";   // MQTT felhasználónév. 
const char *MQTT_PASSWORD         = "";   // MQTT jelszó. 
const char *MQTT_CLIENT_ID        = "esp32_ontozorendszer";
const char *MQTT_DEVICE_ID        = "esp32_ontozorendszer";
const char *MQTT_BASE_TOPIC       = "home/garden/irrigation";
const char *MQTT_DISCOVERY_PREFIX = "homeassistant";

WebServer server(80);
WiFiClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);

const int SENSOR_ADC_PIN = 39;
const int RELAY_PIN      = 5;

const bool RELAY_ACTIVE_LOW = false;

// =========================
// Talajnedvesség kalibráció
// A száraz és nedves referencia ADC értékekből számol a program százalékos nedvességet.
// =========================
int moistureRawDry = 3294;
int moistureRawWet = 1104;

// =========================
// Állítható stratégiai paraméterek
// Ezeket a felhasználó a webes felületen és részben MQTT-n keresztül is módosíthatja.
// =========================
int   dryThresholdPercent          = 25;

float rainSkipMmThreshold          = 1.5f;
int   rainLookaheadHours           = 6;

float heatBoostTempC               = 30.0f;
int   heatThresholdBoostPercent    = 5;

float highHumidityThresholdPercent = 80.0f;
int   highHumidityReductionPercent = 3;

// =========================
// Egyéb konstansok
// Időzítések, mintaszámok és tárolási méretek a méréshez, öntözéshez, Wi-Fi/MQTT működéshez.
// =========================
const uint8_t  SAMPLES                = 6;
const uint8_t  DRY_COUNT_LIMIT        = 3;
const uint32_t WATER_ON_TIME_MS       = 3000;
const uint32_t SOAK_DELAY_MS          = 7000;
const uint32_t CHECK_INTERVAL_MS      = 1000;
const uint32_t WIFI_CONNECT_MS        = 10000;
const uint32_t HISTORY_SAMPLE_MS      = 60000;
const uint32_t WEATHER_UPDATE_MS      = 30UL * 60UL * 1000UL;
const uint32_t MQTT_RECONNECT_MS      = 5000;
const uint32_t MQTT_PUBLISH_MS        = 60000;
const uint16_t MQTT_BUFFER_SIZE       = 1536;
const int      MOISTURE_HISTORY_SIZE  = 60;
const int      WEATHER_FORECAST_HOURS = 24;

// =========================
// Globális állapot
// A futás közben változó állapotok, mérési eredmények, előzmények és MQTT állapotjelzők.
// =========================
enum IrrigationState {
  IRRIGATION_IDLE,
  IRRIGATION_WATERING,
  IRRIGATION_SOAKING
};

IrrigationState irrigationState = IRRIGATION_IDLE;

uint8_t       dryCounter                   = 0;
unsigned long lastCheckMs                  = 0;
unsigned long stateStartedMs               = 0;
bool          manualMode                   = false;
bool          pumpActive                   = false;
int           lastMoisture                 = 0;
int           lastMoisturePercent          = 0;
unsigned long lastMeasurementMs            = 0;
String        lastAutoWateringAt           = "Még nem volt";
String        lastMeasurementTime          = "N/A";
String        lastTimeSyncDescription      = "Nincs szinkron";

unsigned long lastHistorySampleMs          = 0;
unsigned long lastWeatherAttemptMs         = 0;

float         outsideTemperature           = NAN;
float         outsideHumidity              = NAN;
String        weatherLastUpdate            = "N/A";
String        weatherStatus                = "Még nincs lekérve";

String moistureHistoryLabels[MOISTURE_HISTORY_SIZE];
int    moistureHistoryPercent[MOISTURE_HISTORY_SIZE];
int    moistureHistoryCount = 0;

String forecastLabels[WEATHER_FORECAST_HOURS];
float  forecastTemp[WEATHER_FORECAST_HOURS];
float  forecastHumidity[WEATHER_FORECAST_HOURS];
float  forecastPrecipitation[WEATHER_FORECAST_HOURS];
float  forecastPrecipProbability[WEATHER_FORECAST_HOURS];
int    forecastCount = 0;

int    effectiveDryThresholdPercent        = 25;
bool   rainLockActive                      = false;
float  nextLookaheadPrecipitationSum       = 0.0f;
float  nextLookaheadMaxPrecipProbability   = 0.0f;
String irrigationDecisionReason            = "Normál küszöb";

bool          mqttDiscoveryPublished       = false;
unsigned long lastMqttReconnectAttemptMs   = 0;
unsigned long lastMqttPublishMs            = 0;
bool          mqttPublishRequested         = false;

// =========================
// HTML oldal
// Az ESP32 saját webes kezelőfelülete. PROGMEM-ben van, hogy ne a RAM-ot terhelje.
// =========================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="hu">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Öntözőrendszer</title>
  <style>
    body{
      font-family: Arial, sans-serif;
      margin:0;
      background:#f4f7f3;
      color:#1d2a1d;
    }

    .wrap{
      max-width:1150px;
      margin:0 auto;
      padding:16px;
    }

    h1,h2,h3{
      margin:0 0 12px 0;
    }

    .grid{
      display:grid;
      grid-template-columns:repeat(auto-fit,minmax(260px,1fr));
      gap:14px;
      margin-bottom:16px;
    }

    .card{
      background:#fff;
      border-radius:14px;
      padding:20px;
      box-shadow:0 2px 10px rgba(0,0,0,.08);
      margin:0;
    }

    .chart-card{
      background:#fff;
      border-radius:14px;
      padding:20px;
      box-shadow:0 2px 10px rgba(0,0,0,.08);
      margin:0;
    }

    .wrap > .card + .card,
    .wrap > .card + .chart-card,
    .wrap > .chart-card + .card,
    .wrap > .chart-card + .chart-card{
      margin-top:16px;
    }

    .kv{
      display:flex;
      justify-content:space-between;
      gap:12px;
      margin:7px 0;
      border-bottom:1px solid #eef2ee;
      padding-bottom:7px;
      align-items:flex-start;
    }

    .kv span:first-child{
      color:#516051;
    }

    .kv span:last-child{
      text-align:right;
      word-break:break-word;
      max-width:58%;
    }

    .big{
      font-size:28px;
      font-weight:700;
    }

    .row{
      display:flex;
      gap:10px;
      flex-wrap:wrap;
      margin-top:12px;
    }

    button,input{
      font-size:16px;
      padding:10px 12px;
      border-radius:10px;
      border:1px solid #cfd9cf;
      box-sizing:border-box;
    }

    button{
      cursor:pointer;
      background:#eef5ee;
    }

    canvas{
      width:100%;
      height:240px;
      display:block;
      background:#fff;
      border-radius:10px;
    }

    .muted{
      color:#6a766a;
      font-size:14px;
    }

    .pill{
      display:inline-block;
      padding:4px 10px;
      border-radius:999px;
      background:#e9f3ea;
      font-size:13px;
      margin-top:6px;
    }

    .strategy-settings-card{
      padding:20px;
    }

    .strategy-settings-card h3{
      margin-bottom:18px;
    }

    .strategy-settings-card form{
      display:flex;
      flex-direction:column;
      gap:20px;
    }

    .settings-grid{
      display:grid;
      grid-template-columns:repeat(auto-fit,minmax(260px,1fr));
      column-gap:20px;
      row-gap:20px;
      align-items:start;
    }

    .field{
      display:flex;
      flex-direction:column;
      gap:8px;
    }

    .field input{
      width:100%;
    }

    .field label{
      font-size:14px;
      color:#516051;
    }

    .field .unit{
      font-size:13px;
      color:#6a766a;
    }

    .save-row{
      display:flex;
      justify-content:flex-end;
      margin:0;
      padding-top:4px;
    }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Automata öntözőrendszer</h1>
    <div class="muted">Talajnedvesség helyi szenzorból, külső hőmérséklet és páratartalom online időjárási adatból.</div>

    <div class="grid" style="margin-top:16px;">
      <div class="card">
        <h3>Talajnedvesség</h3>
        <div class="big" id="moisturePercent">-</div>
        <div class="muted">százalék</div>
        <div class="kv"><span>Nyers érték</span><span id="moistureRaw">-</span></div>
        <div class="kv"><span>Utolsó mérés</span><span id="lastMeasurementTime">-</span></div>
      </div>

      <div class="card">
        <h3>Öntözés állapot</h3>
        <div class="kv"><span>Pumpa</span><span id="pumpActive">-</span></div>
        <div class="kv"><span>Mód</span><span id="manualMode">-</span></div>
        <div class="kv"><span>Állapot</span><span id="stateText">-</span></div>
        <div class="kv"><span>Utolsó automata öntözés</span><span id="lastAutoWateringAt">-</span></div>
      </div>

      <div class="card">
        <h3>Külső időjárás</h3>
        <div class="kv"><span>Hőmérséklet</span><span id="outsideTemperature">-</span></div>
        <div class="kv"><span>Páratartalom</span><span id="outsideHumidity">-</span></div>
        <div class="kv"><span>Frissítés</span><span id="weatherLastUpdate">-</span></div>
        <div class="kv"><span>Állapot</span><span id="weatherStatus">-</span></div>
        <div class="pill">Lokáció: Szeged</div>
      </div>

      <div class="card">
        <h3>Hálózat és idő</h3>
        <div class="kv"><span>Hálózati mód</span><span id="networkMode">-</span></div>
        <div class="kv"><span>Eszköz IP</span><span id="staIp">-</span></div>
        <div class="kv"><span>Időszinkron</span><span id="timeSync">-</span></div>
      </div>

      <div class="card">
        <h3>Öntözési stratégia</h3>
        <div class="kv"><span>Bázis küszöb</span><span id="dryThresholdText">-</span></div>
        <div class="kv"><span>Aktív küszöb</span><span id="effectiveDryThresholdText">-</span></div>
        <div class="kv"><span>Eső tiltás</span><span id="rainLockActive">-</span></div>
        <div class="kv"><span>Köv. időszak csapadék</span><span id="nextLookaheadPrecipitationSum">-</span></div>
        <div class="kv"><span>Max eső valószínűség</span><span id="nextLookaheadMaxPrecipProbability">-</span></div>
        <div class="kv"><span>Döntési ok</span><span id="irrigationDecisionReason">-</span></div>
      </div>
    </div>

    <div class="card">
      <h3>Alap vezérlés</h3>
      <form action="/set" method="GET" class="row">
        <input id="thresholdInput" name="t" type="number" min="0" max="100" placeholder="Szárazsági küszöb %">
        <button type="submit">Küszöb mentése</button>
      </form>
      <div class="row">
        <button onclick="location.href='/mode?manual=0'">Automata mód</button>
        <button onclick="location.href='/mode?manual=1'">Kézi mód</button>
      </div>
      <div class="row">
        <button onclick="location.href='/start'">Pumpa indítása (kézi mód)</button>
        <button onclick="location.href='/stop'">Pumpa leállítása (kézi mód)</button>
      </div>
      <div class="muted" style="margin-top:10px;">
        Automata módban a pumpát csak az automatikus logika vezérli. Kézi indítás és leállítás csak kézi módban működik.
      </div>
    </div>

    <div class="card strategy-settings-card">
      <h3>Időjárási szabályok beállításai</h3>
      <form action="/setStrategy" method="GET">
        <div class="settings-grid">
          <div class="field">
            <label for="rainSkipMmThresholdInput">Eső tiltási küszöb</label>
            <input id="rainSkipMmThresholdInput" name="rainmm" type="number" min="0" max="50" step="0.1">
            <div class="unit">mm / következő órákban</div>
          </div>

          <div class="field">
            <label for="rainLookaheadHoursInput">Eső előrenézési idő</label>
            <input id="rainLookaheadHoursInput" name="rainh" type="number" min="1" max="24" step="1">
            <div class="unit">óra</div>
          </div>

          <div class="field">
            <label for="heatBoostTempCInput">Meleg korrekció határa</label>
            <input id="heatBoostTempCInput" name="heattemp" type="number" min="-20" max="60" step="0.1">
            <div class="unit">°C felett</div>
          </div>

          <div class="field">
            <label for="heatThresholdBoostPercentInput">Meleg korrekció mértéke</label>
            <input id="heatThresholdBoostPercentInput" name="heatboost" type="number" min="0" max="50" step="1">
            <div class="unit">+ %</div>
          </div>

          <div class="field">
            <label for="highHumidityThresholdPercentInput">Magas páratartalom határa</label>
            <input id="highHumidityThresholdPercentInput" name="humth" type="number" min="0" max="100" step="1">
            <div class="unit">% fölött</div>
          </div>

          <div class="field">
            <label for="highHumidityReductionPercentInput">Páratartalom korrekció mértéke</label>
            <input id="highHumidityReductionPercentInput" name="humred" type="number" min="0" max="50" step="1">
            <div class="unit">- %</div>
          </div>
        </div>

        <div class="save-row">
          <button type="submit">Időjárási szabályok mentése</button>
        </div>
      </form>
    </div>

    <div class="chart-card">
      <h3>Talajnedvesség előzmény</h3>
      <div class="muted">Az utolsó 60 perc, percenként mintavételezve.</div>
      <canvas id="moistureChart"></canvas>
    </div>

    <div class="chart-card">
      <h3>Külső hőmérséklet előrejelzés - Szeged</h3>
      <div class="muted">A következő 24 óra.</div>
      <canvas id="tempChart"></canvas>
    </div>

    <div class="chart-card">
      <h3>Külső páratartalom előrejelzés - Szeged</h3>
      <div class="muted">A következő 24 óra.</div>
      <canvas id="humidityChart"></canvas>
    </div>

    <div class="chart-card">
      <h3>Külső csapadék előrejelzés - Szeged</h3>
      <div class="muted">A következő 24 óra.</div>
      <canvas id="precipChart"></canvas>
    </div>
  </div>

  <script>
    let lastData = null;

    function $(id){ return document.getElementById(id); }

    function setText(id, value){
      $(id).textContent = value ?? "-";
    }

    function updateInputIfIdle(id, value){
      const el = $(id);
      if (document.activeElement !== el) {
        el.value = value;
      }
    }

    function updateCards(d){
      setText("moisturePercent", d.moisturePercent + " %");
      setText("moistureRaw", d.moistureRaw);
      setText("lastMeasurementTime", d.lastMeasurementTime);

      setText("pumpActive", d.pumpActive ? "BE" : "KI");
      setText("manualMode", d.manualMode ? "Kézi" : "Automata");
      setText("stateText", d.state);
      setText("lastAutoWateringAt", d.lastAutoWateringAt);

      setText("outsideTemperature", d.outsideTemperature === null ? "N/A" : d.outsideTemperature + " °C");
      setText("outsideHumidity", d.outsideHumidity === null ? "N/A" : d.outsideHumidity + " %");
      setText("weatherLastUpdate", d.weatherLastUpdate);
      setText("weatherStatus", d.weatherStatus);

      setText("networkMode", d.networkMode);
      setText("staIp", d.staIp && d.staIp.length ? d.staIp : "-");
      setText("timeSync", d.timeSync);

      setText("dryThresholdText", d.dryThresholdPercent + " %");
      setText("effectiveDryThresholdText", d.effectiveDryThresholdPercent + " %");
      setText("rainLockActive", d.rainLockActive ? "AKTÍV" : "Nincs");
      setText("nextLookaheadPrecipitationSum",
        d.nextLookaheadPrecipitationSum === null ? "N/A" : Number(d.nextLookaheadPrecipitationSum).toFixed(1) + " mm"
      );
      setText("nextLookaheadMaxPrecipProbability",
        d.nextLookaheadMaxPrecipProbability === null ? "N/A" : Number(d.nextLookaheadMaxPrecipProbability).toFixed(0) + " %"
      );
      setText("irrigationDecisionReason", d.irrigationDecisionReason);

      updateInputIfIdle("thresholdInput", d.dryThresholdPercent);
      updateInputIfIdle("rainSkipMmThresholdInput", d.rainSkipMmThreshold);
      updateInputIfIdle("rainLookaheadHoursInput", d.rainLookaheadHours);
      updateInputIfIdle("heatBoostTempCInput", d.heatBoostTempC);
      updateInputIfIdle("heatThresholdBoostPercentInput", d.heatThresholdBoostPercent);
      updateInputIfIdle("highHumidityThresholdPercentInput", d.highHumidityThresholdPercent);
      updateInputIfIdle("highHumidityReductionPercentInput", d.highHumidityReductionPercent);
    }

    function drawChart(canvasId, labels, values, options = {}) {
      const canvas = $(canvasId);
      const ctx = canvas.getContext("2d");
      const dpr = window.devicePixelRatio || 1;
      const cssW = canvas.clientWidth;
      const cssH = canvas.clientHeight;

      canvas.width = Math.max(1, Math.floor(cssW * dpr));
      canvas.height = Math.max(1, Math.floor(cssH * dpr));
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

      ctx.clearRect(0, 0, cssW, cssH);
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, cssW, cssH);

      const ml = 44, mr = 12, mt = 12, mb = 28;
      const pw = cssW - ml - mr;
      const ph = cssH - mt - mb;

      ctx.strokeStyle = "#d8e2d8";
      ctx.lineWidth = 1;
      ctx.strokeRect(ml, mt, pw, ph);

      if (!values || !values.length) {
        ctx.fillStyle = "#758175";
        ctx.font = "14px Arial";
        ctx.fillText("Nincs adat", ml + 12, mt + 22);
        return;
      }

      let min = (options.min !== undefined) ? options.min : Math.min(...values);
      let max = (options.max !== undefined) ? options.max : Math.max(...values);

      if (min === max) {
        min -= 1;
        max += 1;
      }

      if (options.min === undefined || options.max === undefined) {
        const pad = (max - min) * 0.12;
        if (options.min === undefined) min -= pad;
        if (options.max === undefined) max += pad;
      }

      ctx.font = "12px Arial";
      ctx.fillStyle = "#6f7c6f";
      ctx.strokeStyle = "#edf1ed";

      for (let i = 0; i <= 4; i++) {
        const y = mt + (ph / 4) * i;
        ctx.beginPath();
        ctx.moveTo(ml, y);
        ctx.lineTo(ml + pw, y);
        ctx.stroke();

        const val = max - ((max - min) / 4) * i;
        const txt = (Math.round(val * 10) / 10).toString() + (options.suffix || "");
        ctx.fillText(txt, 4, y + 4);
      }

      ctx.strokeStyle = options.color || "#2f7d32";
      ctx.lineWidth = 2;
      ctx.beginPath();

      values.forEach((v, i) => {
        const x = ml + (values.length === 1 ? pw / 2 : (pw * i / (values.length - 1)));
        const y = mt + ph - ((v - min) / (max - min)) * ph;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();

      ctx.fillStyle = options.pointColor || options.color || "#2f7d32";
      values.forEach((v, i) => {
        const x = ml + (values.length === 1 ? pw / 2 : (pw * i / (values.length - 1)));
        const y = mt + ph - ((v - min) / (max - min)) * ph;
        ctx.beginPath();
        ctx.arc(x, y, 2.5, 0, Math.PI * 2);
        ctx.fill();
      });

      const step = Math.max(1, Math.ceil(labels.length / 6));
      ctx.fillStyle = "#6f7c6f";
      ctx.font = "11px Arial";

      for (let i = 0; i < labels.length; i += step) {
        const x = ml + (labels.length === 1 ? pw / 2 : (pw * i / (labels.length - 1)));
        ctx.fillText(labels[i], x - 14, mt + ph + 16);
      }

      if ((labels.length - 1) % step !== 0) {
        const i = labels.length - 1;
        const x = ml + (labels.length === 1 ? pw / 2 : (pw * i / (labels.length - 1)));
        ctx.fillText(labels[i], x - 14, mt + ph + 16);
      }
    }

    function renderCharts(d){
      lastData = d;

      drawChart("moistureChart",
        d.moistureHistoryLabels || [],
        d.moistureHistoryPercent || [],
        { min: 0, max: 100, suffix: "%", color: "#2f7d32" }
      );

      drawChart("tempChart",
        d.forecastLabels || [],
        d.forecastTemp || [],
        { suffix: "°C", color: "#1565c0" }
      );

      drawChart("humidityChart",
        d.forecastLabels || [],
        d.forecastHumidity || [],
        { min: 0, max: 100, suffix: "%", color: "#8e24aa" }
      );

      drawChart("precipChart",
        d.forecastLabels || [],
        d.forecastPrecipitation || [],
        { min: 0, suffix: " mm", color: "#ef6c00" }
      );
    }

    async function loadStatus(){
      try{
        const res = await fetch("/status");
        const data = await res.json();
        updateCards(data);
        renderCharts(data);
      } catch(err){
        console.error(err);
      }
    }

    window.addEventListener("resize", () => {
      if (lastData) renderCharts(lastData);
    });

    loadStatus();
    setInterval(loadStatus, 15000);
  </script>
</body>
</html>
)rawliteral";

// =========================
// Segédfüggvények
// Általános kiszolgáló függvények relévezérléshez, JSON készítéshez és időcímkékhez.
// =========================
// Bekapcsolja a szivattyút vezérlő kimenetet az aktív logikai szintnek megfelelően.
inline void relayOn() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
}

// Kikapcsolja a szivattyút vezérlő kimenetet az aktív logikai szintnek megfelelően.
inline void relayOff() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
}

// JSON szövegbe illeszthető formára alakítja a speciális karaktereket.
String jsonEscape(const String &value) {
  String out = value;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  out.replace("\n", "\\n");
  out.replace("\r", "");
  return out;
}

// Idézőjelek közé tett, JSON-kompatibilis szöveget állít elő.
String jsonQuote(const String &value) {
  return "\"" + jsonEscape(value) + "\"";
}

// Lebegőpontos számot alakít szöveggé megadott tizedesjeggyel.
String floatToStringSafe(float value, uint8_t decimals = 1) {
  char buffer[24];
  dtostrf(value, 0, decimals, buffer);
  return String(buffer);
}

// Érvénytelen float esetén JSON null értéket, egyébként számot ad vissza.
String jsonFloatOrNull(float value, uint8_t decimals = 1) {
  if (isnan(value)) return "null";
  return floatToStringSafe(value, decimals);
}

// String tömbből JSON tömböt készít a webes grafikonokhoz.
String buildStringArrayJson(String *arr, int count) {
  String out = "[";
  for (int i = 0; i < count; i++) {
    if (i) out += ",";
    out += jsonQuote(arr[i]);
  }
  out += "]";
  return out;
}

// Egész szám tömbből JSON tömböt készít.
String buildIntArrayJson(int *arr, int count) {
  String out = "[";
  for (int i = 0; i < count; i++) {
    if (i) out += ",";
    out += String(arr[i]);
  }
  out += "]";
  return out;
}

// Float tömbből JSON tömböt készít megadott pontossággal.
String buildFloatArrayJson(float *arr, int count, uint8_t decimals = 1) {
  String out = "[";
  for (int i = 0; i < count; i++) {
    if (i) out += ",";
    out += floatToStringSafe(arr[i], decimals);
  }
  out += "]";
  return out;
}

// Ellenőrzi, hogy sikerült-e valós NTP időt szinkronizálni.
bool isTimeSynced() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

// Rövid HH:MM időcímkét készít grafikonhoz; NTP hiányában millis() alapján becsül.
String getShortTimeLabel() {
  if (isTimeSynced()) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buffer[8];
    strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
    return String(buffer);
  }

  unsigned long totalMinutes = millis() / 60000UL;
  unsigned int hh = (totalMinutes / 60UL) % 24;
  unsigned int mm = totalMinutes % 60UL;
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%02u:%02u", hh, mm);
  return String(buffer);
}

// ISO időbélyegből csak az óra:perc részt emeli ki.
String isoToHourMinute(const String &iso) {
  int t = iso.indexOf('T');
  if (t >= 0 && iso.length() >= t + 6) {
    return iso.substring(t + 1, t + 6);
  }
  return iso;
}

// Összefűzi az öntözési döntés több szöveges indokát.
void appendReason(String &base, const String &part) {
  if (base.length() > 0) base += " | ";
  base += part;
}

// =========================
// MQTT segédfüggvények
// MQTT topicok, Home Assistant discovery üzenetek és állapotpublikálás kezelése.
// =========================
// Az eszköz online/offline állapotának MQTT topicját adja vissza.
String mqttAvailabilityTopic() {
  return String(MQTT_BASE_TOPIC) + "/availability";
}

// Állapotértékek MQTT topicját állítja össze.
String mqttStateTopic(const char *suffix) {
  return String(MQTT_BASE_TOPIC) + "/state/" + suffix;
}

// Vezérlőparancsok MQTT topicját állítja össze.
String mqttCommandTopic(const char *suffix) {
  return String(MQTT_BASE_TOPIC) + "/cmd/" + suffix;
}

// Home Assistant MQTT discovery konfigurációs topicot készít.
String mqttDiscoveryTopic(const char *component, const char *objectId) {
  return String(MQTT_DISCOVERY_PREFIX) + "/" + component + "/" + MQTT_DEVICE_ID + "_" + objectId + "/config";
}

// Közös Home Assistant eszközleíró JSON részletet állít elő.
String mqttDeviceJson() {
  String json = "\"device\":{";
  json += "\"identifiers\":[\"" + String(MQTT_DEVICE_ID) + "\"],";
  json += "\"name\":\"Okos öntözőrendszer\",";
  json += "\"manufacturer\":\"Saját projekt\",";
  json += "\"model\":\"ESP32 öntözőrendszer\",";
  json += "\"sw_version\":\"1.0-mqtt\"},";
  json += "\"availability_topic\":\"" + mqttAvailabilityTopic() + "\",";
  json += "\"payload_available\":\"online\",";
  json += "\"payload_not_available\":\"offline\",";
  return json;
}

// MQTT üzenetet küld, ha a kliens kapcsolódva van.
bool mqttPublishRaw(const String &topic, const String &payload, bool retain = true) {
  if (!mqttClient.connected()) return false;
  return mqttClient.publish(topic.c_str(), payload.c_str(), retain);
}

// Jelzi, hogy állapotváltozás miatt friss MQTT publikálás szükséges.
void requestMqttPublish() {
  mqttPublishRequested = true;
}

// Home Assistant sensor entitás discovery üzenetét készíti és publikálja.
void publishDiscoverySensor(const char *objectId, const char *name, const char *stateSuffix,
                           const char *unit = nullptr, const char *deviceClass = nullptr,
                           const char *stateClass = nullptr, const char *icon = nullptr) {
  String payload = "{";
  payload += "\"name\":\"" + String(name) + "\",";
  payload += "\"object_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += "\"unique_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += mqttDeviceJson();
  payload += "\"state_topic\":\"" + mqttStateTopic(stateSuffix) + "\"";

  if (unit && strlen(unit)) payload += ",\"unit_of_measurement\":\"" + String(unit) + "\"";
  if (deviceClass && strlen(deviceClass)) payload += ",\"device_class\":\"" + String(deviceClass) + "\"";
  if (stateClass && strlen(stateClass)) payload += ",\"state_class\":\"" + String(stateClass) + "\"";
  if (icon && strlen(icon)) payload += ",\"icon\":\"" + String(icon) + "\"";

  payload += "}";
  mqttPublishRaw(mqttDiscoveryTopic("sensor", objectId), payload, true);
}

// Home Assistant binary_sensor entitás discovery üzenetét készíti és publikálja.
void publishDiscoveryBinarySensor(const char *objectId, const char *name, const char *stateSuffix,
                                  const char *deviceClass = nullptr, const char *icon = nullptr) {
  String payload = "{";
  payload += "\"name\":\"" + String(name) + "\",";
  payload += "\"object_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += "\"unique_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += mqttDeviceJson();
  payload += "\"state_topic\":\"" + mqttStateTopic(stateSuffix) + "\",";
  payload += "\"payload_on\":\"ON\",";
  payload += "\"payload_off\":\"OFF\"";
  if (deviceClass && strlen(deviceClass)) payload += ",\"device_class\":\"" + String(deviceClass) + "\"";
  if (icon && strlen(icon)) payload += ",\"icon\":\"" + String(icon) + "\"";
  payload += "}";
  mqttPublishRaw(mqttDiscoveryTopic("binary_sensor", objectId), payload, true);
}

// Home Assistant switch entitás discovery üzenetét készíti és publikálja.
void publishDiscoverySwitch(const char *objectId, const char *name, const char *stateSuffix,
                           const char *commandSuffix, const char *icon = nullptr) {
  String payload = "{";
  payload += "\"name\":\"" + String(name) + "\",";
  payload += "\"object_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += "\"unique_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += mqttDeviceJson();
  payload += "\"state_topic\":\"" + mqttStateTopic(stateSuffix) + "\",";
  payload += "\"command_topic\":\"" + mqttCommandTopic(commandSuffix) + "\",";
  payload += "\"payload_on\":\"ON\",";
  payload += "\"payload_off\":\"OFF\"";
  if (icon && strlen(icon)) payload += ",\"icon\":\"" + String(icon) + "\"";
  payload += "}";
  mqttPublishRaw(mqttDiscoveryTopic("switch", objectId), payload, true);
}

// Home Assistant number entitás discovery üzenetét készíti és publikálja.
void publishDiscoveryNumber(const char *objectId, const char *name, const char *stateSuffix,
                           const char *commandSuffix, int minValue, int maxValue, int step,
                           const char *unit = nullptr, const char *icon = nullptr) {
  String payload = "{";
  payload += "\"name\":\"" + String(name) + "\",";
  payload += "\"object_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += "\"unique_id\":\"" + String(MQTT_DEVICE_ID) + "_" + String(objectId) + "\",";
  payload += mqttDeviceJson();
  payload += "\"state_topic\":\"" + mqttStateTopic(stateSuffix) + "\",";
  payload += "\"command_topic\":\"" + mqttCommandTopic(commandSuffix) + "\",";
  payload += "\"min\":" + String(minValue) + ",";
  payload += "\"max\":" + String(maxValue) + ",";
  payload += "\"step\":" + String(step) + ",";
  payload += "\"mode\":\"box\"";
  if (unit && strlen(unit)) payload += ",\"unit_of_measurement\":\"" + String(unit) + "\"";
  if (icon && strlen(icon)) payload += ",\"icon\":\"" + String(icon) + "\"";
  payload += "}";
  mqttPublishRaw(mqttDiscoveryTopic("number", objectId), payload, true);
}

// Létrehozza az összes Home Assistant entitást MQTT discovery segítségével.
void publishHomeAssistantDiscovery() {
  publishDiscoverySensor("moisture_percent", "Talajnedvesség", "moisture_percent", "%", nullptr, "measurement", "mdi:water-percent");
  publishDiscoverySensor("moisture_raw", "Talajnedvesség nyers", "moisture_raw", nullptr, nullptr, nullptr, "mdi:chart-bell-curve");
  publishDiscoverySensor("outside_temperature", "Külső hőmérséklet", "outside_temperature", "°C", "temperature", "measurement", "mdi:thermometer");
  publishDiscoverySensor("outside_humidity", "Külső páratartalom", "outside_humidity", "%", "humidity", "measurement", "mdi:water-percent");
  publishDiscoverySensor("dry_threshold_percent", "Szárazsági küszöb", "dry_threshold_percent", "%", nullptr, nullptr, "mdi:tune");
  publishDiscoverySensor("effective_threshold_percent", "Aktív küszöb", "effective_threshold_percent", "%", nullptr, nullptr, "mdi:tune-variant");
  publishDiscoverySensor("next_rain_mm", "Várható csapadék", "next_rain_mm", "mm", nullptr, "measurement", "mdi:weather-rainy");
  publishDiscoverySensor("network_mode", "Hálózati mód", "network_mode", nullptr, nullptr, nullptr, "mdi:wifi");
  publishDiscoveryBinarySensor("rain_lock", "Eső tiltás", "rain_lock", nullptr, "mdi:weather-pouring");
  publishDiscoverySwitch("pump", "Pumpa kézi vezérlés", "pump", "pump", "mdi:water-pump");
  publishDiscoverySwitch("manual_mode", "Kézi üzemmód", "manual_mode", "manual_mode", "mdi:gesture-tap-button");
  publishDiscoveryNumber("threshold_number", "Szárazsági küszöb beállítás", "dry_threshold_percent", "dry_threshold_percent", 0, 100, 1, "%", "mdi:tune");
  mqttDiscoveryPublished = true;
}

// Elküldi az aktuális mérési, időjárási és vezérlési állapotokat MQTT-n.
void publishMqttStates() {
  if (!mqttClient.connected()) return;

  mqttPublishRaw(mqttAvailabilityTopic(), "online", true);
  mqttPublishRaw(mqttStateTopic("moisture_percent"), String(lastMoisturePercent), true);
  mqttPublishRaw(mqttStateTopic("moisture_raw"), String(lastMoisture), true);
  mqttPublishRaw(mqttStateTopic("dry_threshold_percent"), String(dryThresholdPercent), true);
  mqttPublishRaw(mqttStateTopic("effective_threshold_percent"), String(effectiveDryThresholdPercent), true);
  mqttPublishRaw(mqttStateTopic("next_rain_mm"), floatToStringSafe(nextLookaheadPrecipitationSum, 1), true);
  mqttPublishRaw(mqttStateTopic("rain_lock"), rainLockActive ? "ON" : "OFF", true);
  mqttPublishRaw(mqttStateTopic("pump"), pumpActive ? "ON" : "OFF", true);
  mqttPublishRaw(mqttStateTopic("manual_mode"), manualMode ? "ON" : "OFF", true);
  mqttPublishRaw(mqttStateTopic("network_mode"), getNetworkModeDescription(), true);

  if (!isnan(outsideTemperature)) {
    mqttPublishRaw(mqttStateTopic("outside_temperature"), floatToStringSafe(outsideTemperature, 1), true);
  }

  if (!isnan(outsideHumidity)) {
    mqttPublishRaw(mqttStateTopic("outside_humidity"), floatToStringSafe(outsideHumidity, 0), true);
  }

  String summary = "{";
  summary += "\"moisture_percent\":" + String(lastMoisturePercent) + ",";
  summary += "\"moisture_raw\":" + String(lastMoisture) + ",";
  summary += "\"pump_active\":" + String(pumpActive ? "true" : "false") + ",";
  summary += "\"manual_mode\":" + String(manualMode ? "true" : "false") + ",";
  summary += "\"dry_threshold_percent\":" + String(dryThresholdPercent) + ",";
  summary += "\"effective_threshold_percent\":" + String(effectiveDryThresholdPercent) + ",";
  summary += "\"rain_lock_active\":" + String(rainLockActive ? "true" : "false") + ",";
  summary += "\"next_rain_mm\":" + floatToStringSafe(nextLookaheadPrecipitationSum, 1) + ",";
  summary += "\"decision_reason\":" + jsonQuote(irrigationDecisionReason) + ",";
  summary += "\"last_measurement_time\":" + jsonQuote(lastMeasurementTime) + ",";
  summary += "\"last_auto_watering_at\":" + jsonQuote(lastAutoWateringAt);
  summary += "}";
  mqttPublishRaw(mqttStateTopic("summary"), summary, true);

  mqttPublishRequested = false;
  lastMqttPublishMs = millis();
}

// Az aktuális stratégiai beállításokat EEPROM-ba menti.
void saveSettings();
// EEPROM-ból betölti a beállításokat, hibás adat esetén alapértékeket használ.
void loadSettings();

// Feldolgozza a Home Assistant felől érkező MQTT vezérlőparancsokat.
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String topicStr(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (topicStr == mqttCommandTopic("manual_mode")) {
    if (msg.equalsIgnoreCase("ON")) {
      manualMode = true;
      stopPumpHardware();
      resetAutoLogic();
      Serial.println("MQTT: kézi mód BE");
    } else if (msg.equalsIgnoreCase("OFF")) {
      manualMode = false;
      stopPumpHardware();
      resetAutoLogic();
      Serial.println("MQTT: automata mód BE");
    }
    requestMqttPublish();
    return;
  }

  if (topicStr == mqttCommandTopic("pump")) {
    if (!manualMode) {
      Serial.println("MQTT: pumpa parancs ignorálva, mert automata mód aktív.");
      requestMqttPublish();
      return;
    }

    if (msg.equalsIgnoreCase("ON")) {
      startPumpHardware();
      Serial.println("MQTT: pumpa BE (kézi mód)");
    } else if (msg.equalsIgnoreCase("OFF")) {
      stopPumpHardware();
      Serial.println("MQTT: pumpa KI (kézi mód)");
    }

    requestMqttPublish();
    return;
  }

  if (topicStr == mqttCommandTopic("dry_threshold_percent")) {
    int v = msg.toInt();
    if (v >= 0 && v <= 100) {
      dryThresholdPercent = v;
      saveSettings();
      updateIrrigationStrategy();
      Serial.print("MQTT: új szárazsági küszöb: ");
      Serial.println(dryThresholdPercent);
      requestMqttPublish();
    }
    return;
  }
}

// Kapcsolódik az MQTT brokerhez, feliratkozik a parancs topicokra és publikálja az állapotokat.
bool mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String clientId = String(MQTT_CLIENT_ID) + "_" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);
  bool ok = false;

  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD, mqttAvailabilityTopic().c_str(), 0, true, "offline");
  } else {
    ok = mqttClient.connect(clientId.c_str(), mqttAvailabilityTopic().c_str(), 0, true, "offline");
  }

  if (!ok) {
    Serial.print("MQTT kapcsolat sikertelen, rc=");
    Serial.println(mqttClient.state());
    return false;
  }

  Serial.println("MQTT kapcsolódva.");
  mqttClient.publish(mqttAvailabilityTopic().c_str(), "online", true);
  mqttClient.subscribe(mqttCommandTopic("pump").c_str());
  mqttClient.subscribe(mqttCommandTopic("manual_mode").c_str());
  mqttClient.subscribe(mqttCommandTopic("dry_threshold_percent").c_str());

  publishHomeAssistantDiscovery();
  publishMqttStates();
  return true;
}

// Időzítve próbál újracsatlakozni, ha az MQTT kapcsolat megszakadt.
void handleMqttConnection() {
  if (mqttClient.connected()) return;

  unsigned long nowMs = millis();
  if (nowMs - lastMqttReconnectAttemptMs < MQTT_RECONNECT_MS) return;
  lastMqttReconnectAttemptMs = nowMs;

  mqttConnect();
}

// Esemény vagy időzítés alapján elküldi az MQTT állapotfrissítést.
void publishMqttIfDue() {
  if (!mqttClient.connected()) return;

  unsigned long nowMs = millis();
  if (mqttPublishRequested || lastMqttPublishMs == 0 || nowMs - lastMqttPublishMs >= MQTT_PUBLISH_MS) {
    publishMqttStates();
  }
}

// =========================
// Perzisztencia
// A felhasználói beállítások EEPROM-ba mentése és visszatöltése újraindítás után.
// =========================
const uint32_t SETTINGS_MAGIC = 0xA55A1234;

typedef struct StoredSettings {
  uint32_t magic;
  uint8_t  dryThresholdPercent;
  float    rainSkipMmThreshold;
  uint8_t  rainLookaheadHours;
  float    heatBoostTempC;
  uint8_t  heatThresholdBoostPercent;
  float    highHumidityThresholdPercent;
  uint8_t  highHumidityReductionPercent;
} StoredSettings;

// Ellenőrzi, hogy az EEPROM-ból olvasott beállítások érvényes tartományban vannak-e.
bool settingsAreValid(const StoredSettings &s);
void saveSettings();
void loadSettings();

bool settingsAreValid(const StoredSettings &s) {
  if (s.magic != SETTINGS_MAGIC) return false;
  if (s.dryThresholdPercent > 100) return false;
  if (s.rainSkipMmThreshold < 0.0f || s.rainSkipMmThreshold > 50.0f) return false;
  if (s.rainLookaheadHours < 1 || s.rainLookaheadHours > WEATHER_FORECAST_HOURS) return false;
  if (s.heatBoostTempC < -20.0f || s.heatBoostTempC > 60.0f) return false;
  if (s.heatThresholdBoostPercent > 50) return false;
  if (s.highHumidityThresholdPercent < 0.0f || s.highHumidityThresholdPercent > 100.0f) return false;
  if (s.highHumidityReductionPercent > 50) return false;
  return true;
}

void saveSettings() {
  StoredSettings s;
  s.magic                        = SETTINGS_MAGIC;
  s.dryThresholdPercent          = (uint8_t)dryThresholdPercent;
  s.rainSkipMmThreshold          = rainSkipMmThreshold;
  s.rainLookaheadHours           = (uint8_t)rainLookaheadHours;
  s.heatBoostTempC               = heatBoostTempC;
  s.heatThresholdBoostPercent    = (uint8_t)heatThresholdBoostPercent;
  s.highHumidityThresholdPercent = highHumidityThresholdPercent;
  s.highHumidityReductionPercent = (uint8_t)highHumidityReductionPercent;

  EEPROM.put(0, s);
  EEPROM.commit();
  Serial.println("Beallitasok elmentve.");
  requestMqttPublish();
}

void loadSettings() {
  StoredSettings s;
  EEPROM.get(0, s);

  if (!settingsAreValid(s)) {
    dryThresholdPercent          = 25;
    rainSkipMmThreshold          = 1.5f;
    rainLookaheadHours           = 6;
    heatBoostTempC               = 30.0f;
    heatThresholdBoostPercent    = 5;
    highHumidityThresholdPercent = 80.0f;
    highHumidityReductionPercent = 3;
    saveSettings();
    return;
  }

  dryThresholdPercent          = s.dryThresholdPercent;
  rainSkipMmThreshold          = s.rainSkipMmThreshold;
  rainLookaheadHours           = s.rainLookaheadHours;
  heatBoostTempC               = s.heatBoostTempC;
  heatThresholdBoostPercent    = s.heatThresholdBoostPercent;
  highHumidityThresholdPercent = s.highHumidityThresholdPercent;
  highHumidityReductionPercent = s.highHumidityReductionPercent;
}

// =========================
// Időkezelés
// NTP alapú időszinkron és ember által olvasható időbélyegek előállítása.
// =========================
// Helyi idő szerinti dátum-idő szöveget ad vissza.
String getFormattedTime() {
  if (!isTimeSynced()) {
    return "Nincs pontos idő";
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

// Wi-Fi kapcsolat esetén NTP időszinkronizálást végez.
void syncTimeIfPossible() {
  if (WiFi.status() != WL_CONNECTED) {
    lastTimeSyncDescription = "Nincs Wi-Fi kapcsolat, NTP kihagyva";
    return;
  }

  Serial.println("NTP időszinkron indítása...");
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);

  const unsigned long startMs = millis();
  while (!isTimeSynced() && millis() - startMs < 10000) {
    delay(100);
  }

  if (isTimeSynced()) {
    lastTimeSyncDescription = "Szinkron: " + getFormattedTime();
    Serial.println("Idő szinkronizálva: " + getFormattedTime());
  } else {
    lastTimeSyncDescription = "NTP nem sikerült";
    Serial.println("NTP időszinkron nem sikerült.");
  }
}

// =========================
// Szenzorolvasás
// A kapacitív talajnedvesség-érzékelő ADC értékének olvasása és százalékosítása.
// =========================
// Több ADC minta átlagolásával beolvassa a talajnedvesség nyers értékét.
int readMoisture() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < SAMPLES; i++) {
    sum += analogRead(SENSOR_ADC_PIN);
  }
  return sum / SAMPLES;
}

// A nyers ADC értéket kalibráció alapján 0-100% közötti nedvességértékké alakítja.
int rawToPercent(int raw) {
  if (moistureRawDry == moistureRawWet) return 0;

  int percent;
  if (moistureRawDry > moistureRawWet) {
    percent = map(raw, moistureRawDry, moistureRawWet, 0, 100);
  } else {
    percent = map(raw, moistureRawDry, moistureRawWet, 100, 0);
  }

  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  return percent;
}

// Frissíti az utolsó nyers és százalékos talajnedvesség-mérést.
void updateLastMeasurement() {
  lastMoisture = readMoisture();
  lastMoisturePercent = rawToPercent(lastMoisture);
  lastMeasurementMs = millis();
  lastMeasurementTime = getFormattedTime();
}

// Új talajnedvesség előzményértéket ment, telített lista esetén eltolással.
void appendMoistureHistory(const String &label, int percent) {
  if (moistureHistoryCount < MOISTURE_HISTORY_SIZE) {
    moistureHistoryLabels[moistureHistoryCount] = label;
    moistureHistoryPercent[moistureHistoryCount] = percent;
    moistureHistoryCount++;
    return;
  }

  for (int i = 1; i < MOISTURE_HISTORY_SIZE; i++) {
    moistureHistoryLabels[i - 1] = moistureHistoryLabels[i];
    moistureHistoryPercent[i - 1] = moistureHistoryPercent[i];
  }

  moistureHistoryLabels[MOISTURE_HISTORY_SIZE - 1] = label;
  moistureHistoryPercent[MOISTURE_HISTORY_SIZE - 1] = percent;
}

// Időzítés alapján rögzíti a talajnedvesség előzményadatot.
void recordMoistureHistoryIfDue() {
  unsigned long nowMs = millis();
  if (lastHistorySampleMs == 0 || nowMs - lastHistorySampleMs >= HISTORY_SAMPLE_MS) {
    lastHistorySampleMs = nowMs;
    appendMoistureHistory(getShortTimeLabel(), lastMoisturePercent);
  }
}

// =========================
// Wi-Fi kezelés
// Station módú csatlakozás az otthoni hálózathoz és automatikus újracsatlakozás.
// =========================
unsigned long lastWifiRetryMs = 0;

// ESP32-t Station módba állítja és csatlakozik a beállított Wi-Fi hálózathoz.
void connectStation() {
  if (strlen(STA_SSID) == 0) {
    Serial.println("STA Wi-Fi nincs beállítva.");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASSWORD);

  Serial.print("Csatlakozás az otthoni Wi-Fi-re: ");
  Serial.println(STA_SSID);

  const unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("STA kapcsolódva, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("STA kapcsolat nem sikerült.");
  }
}

// Megszakadt Wi-Fi kapcsolat esetén időzítve újracsatlakozást indít.
void wifiReconnectIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiRetryMs < 10000) return;

  lastWifiRetryMs = now;
  Serial.println("Wi-Fi megszakadt, újracsatlakozás...");

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASSWORD);
}

// A Wi-Fi alrendszer inicializálása.
void setupWifi() {
  connectStation();
}

// Rövid szöveges leírást ad a hálózati állapotról.
String getNetworkModeDescription() {
  return WiFi.status() == WL_CONNECTED ? "STA" : "STA (nincs kapcsolat)";
}

// =========================
// Öntözési stratégia számítás
// A bázis szárazsági küszöb módosítása időjárási adatok alapján.
// =========================
// Kiszámítja az aktív öntözési küszöböt és az eső miatti tiltást.
void updateIrrigationStrategy() {
  effectiveDryThresholdPercent = dryThresholdPercent;
  nextLookaheadPrecipitationSum = 0.0f;
  nextLookaheadMaxPrecipProbability = 0.0f;
  rainLockActive = false;

  int hoursToCheck = forecastCount;
  if (hoursToCheck > rainLookaheadHours) {
    hoursToCheck = rainLookaheadHours;
  }

  for (int i = 0; i < hoursToCheck; i++) {
    nextLookaheadPrecipitationSum += forecastPrecipitation[i];
    if (forecastPrecipProbability[i] > nextLookaheadMaxPrecipProbability) {
      nextLookaheadMaxPrecipProbability = forecastPrecipProbability[i];
    }
  }

  String reason = "";

  if (!isnan(outsideTemperature) && outsideTemperature > heatBoostTempC) {
    effectiveDryThresholdPercent += heatThresholdBoostPercent;
    appendReason(reason, String("Meleg korrekció: +") + String(heatThresholdBoostPercent) + "%");
  }

  if (!isnan(outsideHumidity) && outsideHumidity >= highHumidityThresholdPercent) {
    effectiveDryThresholdPercent -= highHumidityReductionPercent;
    appendReason(reason, String("Magas páratartalom: -") + String(highHumidityReductionPercent) + "%");
  }

  if (effectiveDryThresholdPercent < 0)   effectiveDryThresholdPercent = 0;
  if (effectiveDryThresholdPercent > 100) effectiveDryThresholdPercent = 100;

  if (nextLookaheadPrecipitationSum >= rainSkipMmThreshold) {
    rainLockActive = true;
    appendReason(reason, "Várható eső: öntözés tiltva");
  }

  if (reason.length() == 0) {
    reason = "Normál küszöb";
  }

  irrigationDecisionReason = reason;
}

// =========================
// Öntözés állapotgép
// Nem blokkoló automatikus öntözési ciklus: készenlét, öntözés, áztatási várakozás.
// =========================
// Az öntözési állapotot felhasználóbarát szöveggé alakítja.
const char *irrigationStateToText(IrrigationState state) {
  switch (state) {
    case IRRIGATION_IDLE:     return "Készenlét";
    case IRRIGATION_WATERING: return "Öntözés folyamatban";
    case IRRIGATION_SOAKING:  return "Áztatási várakozás";
    default:                  return "Ismeretlen";
  }
}

// Leállítja a szivattyút és frissíti a pumpaállapot változót.
void stopPumpHardware() {
  relayOff();
  pumpActive = false;
}

// Elindítja a szivattyút és frissíti a pumpaállapot változót.
void startPumpHardware() {
  relayOn();
  pumpActive = true;
}

// Automatikus öntözési ciklust indít és rögzíti az indítás időpontját.
void startAutoWateringCycle() {
  irrigationState = IRRIGATION_WATERING;
  stateStartedMs = millis();
  startPumpHardware();
  lastAutoWateringAt = getFormattedTime();
  Serial.println("Automata öntözés indítva.");
  requestMqttPublish();
}

// Befejezi az automatikus öntözési ciklust és készenlétbe áll.
void finishAutoCycle() {
  irrigationState = IRRIGATION_IDLE;
  stateStartedMs = 0;
  stopPumpHardware();
  Serial.println("Öntözési ciklus befejezve.");
  requestMqttPublish();
}

// Alaphelyzetbe állítja az automatikus öntözési logikát.
void resetAutoLogic() {
  dryCounter = 0;
  irrigationState = IRRIGATION_IDLE;
  stateStartedMs = 0;
}

// Kezeli az öntözés és áztatás időzített állapotváltásait.
void updateIrrigationStateMachine() {
  if (manualMode) {
    return;
  }

  const unsigned long nowMs = millis();

  switch (irrigationState) {
    case IRRIGATION_IDLE:
      break;

    case IRRIGATION_WATERING:
      if (nowMs - stateStartedMs >= WATER_ON_TIME_MS) {
        stopPumpHardware();
        irrigationState = IRRIGATION_SOAKING;
        stateStartedMs = nowMs;
        Serial.println("Öntözés vége, áztatási szakasz indul.");
      }
      break;

    case IRRIGATION_SOAKING:
      if (nowMs - stateStartedMs >= SOAK_DELAY_MS) {
        finishAutoCycle();
      }
      break;
  }
}

// Időzítve mér, dönt az öntözésről és szükség esetén indítja az automatikus ciklust.
void checkMoistureIfDue() {
  if (manualMode || irrigationState != IRRIGATION_IDLE) {
    return;
  }

  const unsigned long nowMs = millis();
  if (nowMs - lastCheckMs < CHECK_INTERVAL_MS) {
    return;
  }
  lastCheckMs = nowMs;

  updateLastMeasurement();
  recordMoistureHistoryIfDue();
  updateIrrigationStrategy();

  Serial.print("Nedvesség nyers érték: ");
  Serial.print(lastMoisture);
  Serial.print(" | ");
  Serial.print(lastMoisturePercent);
  Serial.println(" %");

  Serial.print("Bázis küszöb: ");
  Serial.print(dryThresholdPercent);
  Serial.print(" %, aktív küszöb: ");
  Serial.print(effectiveDryThresholdPercent);
  Serial.println(" %");

  Serial.print("Köv. időszak csapadék: ");
  Serial.print(nextLookaheadPrecipitationSum, 1);
  Serial.println(" mm");

  if (rainLockActive) {
    dryCounter = 0;
    stopPumpHardware();
    Serial.println("Automata öntözés tiltva a várható csapadék miatt.");
    requestMqttPublish();
    return;
  }

  if (lastMoisturePercent <= effectiveDryThresholdPercent) {
    dryCounter++;
    Serial.print("Száraz mérés számláló: ");
    Serial.println(dryCounter);

    if (dryCounter >= DRY_COUNT_LIMIT) {
      dryCounter = 0;
      startAutoWateringCycle();
    }
  } else {
    dryCounter = 0;
    stopPumpHardware();
    requestMqttPublish();
  }
}

// =========================
// Időjárás lekérés és feldolgozás
// Open-Meteo előrejelzés lekérése, egyszerű JSON feldolgozása és adatok eltárolása.
// =========================
// Összeállítja az Open-Meteo API lekérdezési URL-jét.
String buildWeatherUrl() {
  String url = "https://api.open-meteo.com/v1/forecast?";
  url += "latitude=" + String(WEATHER_LAT, 4);
  url += "&longitude=" + String(WEATHER_LON, 4);
  url += "&current=temperature_2m,relative_humidity_2m";
  url += "&hourly=temperature_2m,relative_humidity_2m,precipitation,precipitation_probability";
  url += "&forecast_hours=24";
  url += "&timezone=Europe%2FBudapest";
  return url;
}

// Egyszerű szöveges JSON-érték kiemelése a válaszból.
bool extractStringValueFrom(const String &src, int startPos, const String &key, String &out) {
  int keyPos = src.indexOf(key, startPos);
  if (keyPos < 0) return false;

  int colon = src.indexOf(':', keyPos + key.length());
  if (colon < 0) return false;

  int q1 = src.indexOf('"', colon + 1);
  if (q1 < 0) return false;

  int q2 = src.indexOf('"', q1 + 1);
  if (q2 < 0) return false;

  out = src.substring(q1 + 1, q2);
  return true;
}

// Egyszerű lebegőpontos JSON-érték kiemelése a válaszból.
bool extractFloatValueFrom(const String &src, int startPos, const String &key, float &out) {
  int keyPos = src.indexOf(key, startPos);
  if (keyPos < 0) return false;

  int colon = src.indexOf(':', keyPos + key.length());
  if (colon < 0) return false;

  int i = colon + 1;
  while (i < src.length() && (src[i] == ' ' || src[i] == '\n' || src[i] == '\r' || src[i] == '\t')) i++;

  int end = i;
  while (end < src.length()) {
    char c = src[end];
    bool valid = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
    if (!valid) break;
    end++;
  }

  if (end <= i) return false;
  out = src.substring(i, end).toFloat();
  return true;
}

// JSON tömb tartalmát emeli ki szövegként.
bool extractArrayContentFrom(const String &src, int startPos, const String &key, String &out) {
  int keyPos = src.indexOf(key, startPos);
  if (keyPos < 0) return false;

  int b1 = src.indexOf('[', keyPos + key.length());
  if (b1 < 0) return false;

  int b2 = src.indexOf(']', b1 + 1);
  if (b2 < 0) return false;

  out = src.substring(b1 + 1, b2);
  return true;
}

// Időbélyeg string tömböt dolgoz fel.
int parseQuotedStringArray(const String &src, String *out, int maxCount) {
  int count = 0;
  int pos = 0;

  while (count < maxCount) {
    int q1 = src.indexOf('"', pos);
    if (q1 < 0) break;
    int q2 = src.indexOf('"', q1 + 1);
    if (q2 < 0) break;

    out[count++] = src.substring(q1 + 1, q2);
    pos = q2 + 1;
  }

  return count;
}

// Számokat tartalmazó tömböt dolgoz fel.
int parseFloatArray(const String &src, float *out, int maxCount) {
  int count = 0;
  int start = 0;

  while (count < maxCount && start < src.length()) {
    while (start < src.length() && (src[start] == ' ' || src[start] == ',' || src[start] == '\n' || src[start] == '\r' || src[start] == '\t')) {
      start++;
    }
    if (start >= src.length()) break;

    int end = start;
    while (end < src.length() && src[end] != ',') {
      end++;
    }

    String token = src.substring(start, end);
    token.trim();

    if (token.length() > 0) {
      out[count++] = token.toFloat();
    }

    start = end + 1;
  }

  return count;
}

// Feldolgozza az időjárási API válaszát és frissíti az előrejelzési tömböket.
bool parseWeatherPayload(const String &payload) {
  int currentPos = payload.indexOf("\"current\":");
  int hourlyPos  = payload.indexOf("\"hourly\":");

  if (currentPos < 0 || hourlyPos < 0) {
    return false;
  }

  float nowTemp = NAN;
  float nowHum  = NAN;
  String currentTimeIso;

  if (!extractStringValueFrom(payload, currentPos, "\"time\"", currentTimeIso)) return false;
  if (!extractFloatValueFrom(payload, currentPos, "\"temperature_2m\"", nowTemp)) return false;
  if (!extractFloatValueFrom(payload, currentPos, "\"relative_humidity_2m\"", nowHum)) return false;

  String timeArrayRaw, tempArrayRaw, humArrayRaw, precipArrayRaw, precipProbArrayRaw;
  if (!extractArrayContentFrom(payload, hourlyPos, "\"time\"", timeArrayRaw)) return false;
  if (!extractArrayContentFrom(payload, hourlyPos, "\"temperature_2m\"", tempArrayRaw)) return false;
  if (!extractArrayContentFrom(payload, hourlyPos, "\"relative_humidity_2m\"", humArrayRaw)) return false;
  if (!extractArrayContentFrom(payload, hourlyPos, "\"precipitation\"", precipArrayRaw)) return false;
  if (!extractArrayContentFrom(payload, hourlyPos, "\"precipitation_probability\"", precipProbArrayRaw)) return false;

  String rawTimes[WEATHER_FORECAST_HOURS];
  float rawTemps[WEATHER_FORECAST_HOURS];
  float rawHums[WEATHER_FORECAST_HOURS];
  float rawPrecips[WEATHER_FORECAST_HOURS];
  float rawPrecipProbs[WEATHER_FORECAST_HOURS];

  int timeCount       = parseQuotedStringArray(timeArrayRaw, rawTimes, WEATHER_FORECAST_HOURS);
  int tempCount       = parseFloatArray(tempArrayRaw, rawTemps, WEATHER_FORECAST_HOURS);
  int humCount        = parseFloatArray(humArrayRaw, rawHums, WEATHER_FORECAST_HOURS);
  int precipCount     = parseFloatArray(precipArrayRaw, rawPrecips, WEATHER_FORECAST_HOURS);
  int precipProbCount = parseFloatArray(precipProbArrayRaw, rawPrecipProbs, WEATHER_FORECAST_HOURS);

  forecastCount = timeCount;
  if (tempCount       < forecastCount) forecastCount = tempCount;
  if (humCount        < forecastCount) forecastCount = humCount;
  if (precipCount     < forecastCount) forecastCount = precipCount;
  if (precipProbCount < forecastCount) forecastCount = precipProbCount;

  for (int i = 0; i < forecastCount; i++) {
    forecastLabels[i]            = isoToHourMinute(rawTimes[i]);
    forecastTemp[i]              = rawTemps[i];
    forecastHumidity[i]          = rawHums[i];
    forecastPrecipitation[i]     = rawPrecips[i];
    forecastPrecipProbability[i] = rawPrecipProbs[i];
  }

  outsideTemperature = nowTemp;
  outsideHumidity    = nowHum;
  weatherLastUpdate  = getFormattedTime();
  weatherStatus      = "Friss adat";

  updateIrrigationStrategy();
  requestMqttPublish();
  return true;
}

// HTTPS kapcsolaton keresztül lekéri az aktuális időjárás-előrejelzést.
bool fetchWeatherNow() {
  if (WiFi.status() != WL_CONNECTED) {
    weatherStatus = "Nincs internetes kapcsolat";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);

  String url = buildWeatherUrl();

  Serial.println("Időjárás lekérés indul...");
  Serial.println(url);

  if (!https.begin(client, url)) {
    weatherStatus = "HTTPS kapcsolat hiba";
    return false;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    weatherStatus = "HTTP hiba: " + String(httpCode);
    Serial.println(weatherStatus);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  bool ok = parseWeatherPayload(payload);
  if (!ok) {
    weatherStatus = "JSON feldolgozási hiba";
    Serial.println(weatherStatus);
    return false;
  }

  Serial.print("Időjárás frissítve. Hőmérséklet: ");
  Serial.print(outsideTemperature, 1);
  Serial.print(" C, páratartalom: ");
  Serial.print(outsideHumidity, 0);
  Serial.println(" %");

  return true;
}

// Időzítés alapján frissíti az időjárási adatokat.
void updateWeatherIfDue() {
  unsigned long nowMs = millis();

  if (lastWeatherAttemptMs == 0 || nowMs - lastWeatherAttemptMs >= WEATHER_UPDATE_MS) {
    lastWeatherAttemptMs = nowMs;
    fetchWeatherNow();
  }
}

// =========================
// Web
// HTTP végpontok: főoldal, állapot JSON, beállítások és kézi vezérlés.
// =========================
// Kiszolgálja a fő HTML kezelőfelületet.
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

// JSON válaszban visszaadja a rendszer aktuális állapotát a webes felületnek.
void handleStatus() {
  String json;
  json.reserve(11000);

  json += "{";
  json += "\"moistureRaw\":" + String(lastMoisture) + ",";
  json += "\"moisturePercent\":" + String(lastMoisturePercent) + ",";

  json += "\"dryThresholdPercent\":" + String(dryThresholdPercent) + ",";
  json += "\"rainSkipMmThreshold\":" + jsonFloatOrNull(rainSkipMmThreshold, 1) + ",";
  json += "\"rainLookaheadHours\":" + String(rainLookaheadHours) + ",";
  json += "\"heatBoostTempC\":" + jsonFloatOrNull(heatBoostTempC, 1) + ",";
  json += "\"heatThresholdBoostPercent\":" + String(heatThresholdBoostPercent) + ",";
  json += "\"highHumidityThresholdPercent\":" + jsonFloatOrNull(highHumidityThresholdPercent, 0) + ",";
  json += "\"highHumidityReductionPercent\":" + String(highHumidityReductionPercent) + ",";

  json += "\"effectiveDryThresholdPercent\":" + String(effectiveDryThresholdPercent) + ",";
  json += "\"rainLockActive\":" + String(rainLockActive ? "true" : "false") + ",";
  json += "\"nextLookaheadPrecipitationSum\":" + jsonFloatOrNull(nextLookaheadPrecipitationSum, 1) + ",";
  json += "\"nextLookaheadMaxPrecipProbability\":" + jsonFloatOrNull(nextLookaheadMaxPrecipProbability, 0) + ",";
  json += "\"irrigationDecisionReason\":" + jsonQuote(irrigationDecisionReason) + ",";

  json += "\"pumpActive\":" + String(pumpActive ? "true" : "false") + ",";
  json += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
  json += "\"state\":" + jsonQuote(String(irrigationStateToText(irrigationState))) + ",";
  json += "\"lastMeasurementTime\":" + jsonQuote(lastMeasurementTime) + ",";
  json += "\"lastAutoWateringAt\":" + jsonQuote(lastAutoWateringAt) + ",";
  json += "\"timeSync\":" + jsonQuote(lastTimeSyncDescription) + ",";
  json += "\"networkMode\":" + jsonQuote(getNetworkModeDescription()) + ",";
  json += "\"staIp\":" + jsonQuote(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + ",";

  json += "\"outsideTemperature\":" + jsonFloatOrNull(outsideTemperature, 1) + ",";
  json += "\"outsideHumidity\":" + jsonFloatOrNull(outsideHumidity, 0) + ",";
  json += "\"weatherLastUpdate\":" + jsonQuote(weatherLastUpdate) + ",";
  json += "\"weatherStatus\":" + jsonQuote(weatherStatus) + ",";

  json += "\"moistureHistoryLabels\":" + buildStringArrayJson(moistureHistoryLabels, moistureHistoryCount) + ",";
  json += "\"moistureHistoryPercent\":" + buildIntArrayJson(moistureHistoryPercent, moistureHistoryCount) + ",";
  json += "\"forecastLabels\":" + buildStringArrayJson(forecastLabels, forecastCount) + ",";
  json += "\"forecastTemp\":" + buildFloatArrayJson(forecastTemp, forecastCount, 1) + ",";
  json += "\"forecastHumidity\":" + buildFloatArrayJson(forecastHumidity, forecastCount, 0) + ",";
  json += "\"forecastPrecipitation\":" + buildFloatArrayJson(forecastPrecipitation, forecastCount, 1) + ",";
  json += "\"forecastPrecipProbability\":" + buildFloatArrayJson(forecastPrecipProbability, forecastCount, 0);
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

// Feldolgozza a szárazsági küszöb módosítását a webes felületről.
void handleSet() {
  if (server.hasArg("t")) {
    int newVal = server.arg("t").toInt();
    if (newVal >= 0 && newVal <= 100) {
      dryThresholdPercent = newVal;
      saveSettings();
      updateIrrigationStrategy();
    }
  }
  requestMqttPublish();
  server.sendHeader("Location", "/");
  server.send(303);
}

// Feldolgozza az időjárás-alapú stratégiai paraméterek módosítását.
void handleSetStrategy() {
  bool changed = false;

  if (server.hasArg("rainmm")) {
    float v = server.arg("rainmm").toFloat();
    if (v >= 0.0f && v <= 50.0f) {
      rainSkipMmThreshold = v;
      changed = true;
    }
  }

  if (server.hasArg("rainh")) {
    int v = server.arg("rainh").toInt();
    if (v >= 1 && v <= WEATHER_FORECAST_HOURS) {
      rainLookaheadHours = v;
      changed = true;
    }
  }

  if (server.hasArg("heattemp")) {
    float v = server.arg("heattemp").toFloat();
    if (v >= -20.0f && v <= 60.0f) {
      heatBoostTempC = v;
      changed = true;
    }
  }

  if (server.hasArg("heatboost")) {
    int v = server.arg("heatboost").toInt();
    if (v >= 0 && v <= 50) {
      heatThresholdBoostPercent = v;
      changed = true;
    }
  }

  if (server.hasArg("humth")) {
    float v = server.arg("humth").toFloat();
    if (v >= 0.0f && v <= 100.0f) {
      highHumidityThresholdPercent = v;
      changed = true;
    }
  }

  if (server.hasArg("humred")) {
    int v = server.arg("humred").toInt();
    if (v >= 0 && v <= 50) {
      highHumidityReductionPercent = v;
      changed = true;
    }
  }

  if (changed) {
    saveSettings();
    updateIrrigationStrategy();
  }

  requestMqttPublish();
  server.sendHeader("Location", "/");
  server.send(303);
}

// Kézi és automata üzemmód közötti váltást kezel.
void handleMode() {
  if (!server.hasArg("manual")) {
    server.send(400, "text/plain", "Hianyzo manual parameter");
    return;
  }

  String v = server.arg("manual");

  if (v == "1") {
    manualMode = true;
    stopPumpHardware();
    resetAutoLogic();
    Serial.println("Web: kézi mód BE");
  } else {
    manualMode = false;
    stopPumpHardware();
    resetAutoLogic();
    Serial.println("Web: automata mód BE");
  }

  requestMqttPublish();
  server.sendHeader("Location", "/");
  server.send(303);
}

// Kézi módban elindítja a pumpát webes parancsra.
void handleStart() {
  if (!manualMode) {
    Serial.println("Web: pumpa indítás ignorálva, mert automata mód aktív.");
    requestMqttPublish();
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  startPumpHardware();
  Serial.println("Web: pumpa BE (kézi mód)");

  requestMqttPublish();
  server.sendHeader("Location", "/");
  server.send(303);
}

// Kézi módban leállítja a pumpát webes parancsra.
void handleStop() {
  if (!manualMode) {
    Serial.println("Web: pumpa leállítás ignorálva, mert automata mód aktív.");
    requestMqttPublish();
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  stopPumpHardware();
  Serial.println("Web: pumpa KI (kézi mód)");
  requestMqttPublish();

  server.sendHeader("Location", "/");
  server.send(303);
}

// Regisztrálja a HTTP végpontokat és elindítja a web szervert.
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  server.on("/setStrategy", handleSetStrategy);
  server.on("/mode", handleMode);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.begin();
  Serial.println("HTTP szerver elindult.");
}

// =========================
// Setup / Loop
// Inicializálás és a fő programciklus. A loop nem blokkoló jelleggel kezeli a feladatokat.
// =========================
// Egyszer lefutó inicializálás: hardver, Wi-Fi, idő, web, MQTT, első mérés.
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_PIN, OUTPUT);
  stopPumpHardware();

  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_ADC_PIN, ADC_11db);

  EEPROM.begin(128);
  loadSettings();

  setupWifi();
  syncTimeIfPossible();

  updateLastMeasurement();
  appendMoistureHistory(getShortTimeLabel(), lastMoisturePercent);

  setupWebServer();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);

  fetchWeatherNow();
  updateIrrigationStrategy();
  requestMqttPublish();
}

// Folyamatos fő ciklus: web, Wi-Fi, MQTT, mérés, időjárás és öntözési állapotgép kezelése.
void loop() {
  server.handleClient();

  wifiReconnectIfNeeded();
  handleMqttConnection();
  mqttClient.loop();

  updateIrrigationStateMachine();
  checkMoistureIfDue();
  updateWeatherIfDue();
  publishMqttIfDue();
}
