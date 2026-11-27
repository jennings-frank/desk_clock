#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <TM1637Display.h>
#include <IRremote.hpp> // Install "IRremote" by Armin Joachimsmeyer (v4.x)
#include <DHT.h>

// --- Wi-Fi Credentials ---
const char* ssid     = "<SSID>";
const char* password = "<PASS>";


// --- Pin Definitions ---
#define IR_PIN     3
#define CLK_PIN    4
#define DIO_PIN    5
#define BUZZER_PIN 6
#define DHT_PIN    0
#define DHT_TYPE   DHT11

// --- IR Remote Command Bytes (Standard 21-Key Mini Remote) ---
#define IR_CMD_POWER      0x45  // CH-   : Toggle Display ON/OFF
#define IR_CMD_DND        0x47  // CH+   : Toggle Manual DND (Dims + Mutes)
#define IR_CMD_BRIGHTNESS 0x46  // CH    : Cycle Brightness
#define IR_CMD_CHIME      0x44  // |<<   : Test Chime
#define IR_CMD_VOL_DOWN   0x07  // VOL-  : Toggle Buzzer Sound ON/OFF
#define IR_CMD_VOL_UP     0x15  // VOL+  : Toggle Buzzer Sound ON/OFF
#define IR_CMD_EQ         0x09  // EQ    : Cycle Time -> Indoor -> Outdoor Stats

// --- IR Remote Number Keys (0 - 9 for Meditation Countdown Timer) ---
#define IR_CMD_0          0x16  // 0     : Cancel Timer & Return to Clock
#define IR_CMD_1          0x0C  // 1     : 1-Min Timer
#define IR_CMD_2          0x18  // 2     : 2-Min Timer
#define IR_CMD_3          0x5E  // 3     : 3-Min Timer
#define IR_CMD_4          0x08  // 4     : 4-Min Timer
#define IR_CMD_5          0x1C  // 5     : 5-Min Timer
#define IR_CMD_6          0x5A  // 6     : 6-Min Timer
#define IR_CMD_7          0x42  // 7     : 7-Min Timer
#define IR_CMD_8          0x52  // 8     : 8-Min Timer
#define IR_CMD_9          0x4A  // 9     : 9-Min Timer

// --- Timezone Configuration (IST = UTC+5:30) ---
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

const char* ntpServer1 = "time.google.com";
const char* ntpServer2 = "time.cloudflare.com";
const char* ntpServer3 = "in.pool.ntp.org";

// --- Add lat/long for Open-Meteo Endpoints ---
const char* urlAQI     = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=<LAT>&longitude=<LONG>current=us_aqi";
const char* urlWeather = "https://api.open-meteo.com/v1/forecast?latitude=<LAT>&longitude=<LONG>&current=apparent_temperature,uv_index,wind_gusts_10m&hourly=precipitation_probability&timezone=Asia%2FKolkata&forecast_hours=12";

// --- Brightness Settings (0 = dimmest, 7 = brightest) ---
const uint8_t DAY_BRIGHTNESS   = 5;
const uint8_t NIGHT_BRIGHTNESS = 1;

TM1637Display display(CLK_PIN, DIO_PIN);
DHT dht(DHT_PIN, DHT_TYPE);
Preferences prefs;

// --- Thread Synchronization for Background Weather Task ---
portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool triggerWeatherRefresh = true;

// --- State Variables ---
int lastChimeHour = -1;
int lastAlarmDay  = -1;
int lastDisconnectReason = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastDhtRead = 0;

// --- Countdown Timer State ---
bool timerActive = false;
unsigned long timerEndMs = 0;

// --- Sensor & Perungudi Weather Cache ---
int currentTempC     = -99;
int currentHum       = -99;
volatile int perungudiAQI   = -1;
volatile int perungudiFeels = -99;
volatile int perungudiUV    = -1;
volatile int perungudiGusts = -1;
volatile int perungudiRain  = -1;

// --- Remote Control & DND States ---
bool displayEnabled = true;
bool buzzerEnabled  = true;
bool isDND          = false;
int manualDndOverride = -1; // -1 = Auto schedule, 0 = Forced OFF, 1 = Forced ON
int lastAutoDndState = -1;
uint8_t currentDayBrightness = DAY_BRIGHTNESS;
unsigned long osdEndTime = 0;
int displayMode = 0; // 0:Time, 1:InTemp, 2:InHum, 3:AQI, 4:FeelsLike, 5:UV, 6:Gusts, 7:Rain%

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
  }
}

void silenceBuzzer() {
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
}

// Disabled so remote presses & stray IR noise never make unintended beeps
void playClickBeep() {
}

void playHourlyChime() {
  if (!buzzerEnabled || isDND) return;
  tone(BUZZER_PIN, 2093, 140);
  delay(170);
  tone(BUZZER_PIN, 2637, 260);
  delay(280);
  silenceBuzzer();
}

// --- Soothing Meditation Timer Completion Chime ---
void playTimerDoneTone() {
  // Change to `if (!buzzerEnabled || isDND) return;` if you want DND to mute the timer too
  if (!buzzerEnabled) return;

  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 1760, 180); // A6
    delay(210);
    tone(BUZZER_PIN, 2217, 220); // C#7
    delay(250);
    tone(BUZZER_PIN, 2637, 380); // E7
    delay(550);
  }
  silenceBuzzer();
}

// --- Full-Volume 6:00 AM Morning Alarm (Cannot be stopped via remote) ---
void play6AMAlarm() {
  if (!buzzerEnabled || isDND) return;
  int melody[]    = { 2093, 2489, 2794, 3136, 2794, 3136 };
  int durations[] = {  140,  140,  140,  280,  140,  420 };

  for (int repeat = 0; repeat < 3; repeat++) {
    for (int i = 0; i < 6; i++) {
      tone(BUZZER_PIN, melody[i], durations[i]);
      delay(durations[i] + 35);
    }
    delay(250);
  }
  silenceBuzzer();
}

void showOSD(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3, unsigned long durationMs) {
  uint8_t segs[] = { s0, s1, s2, s3 };
  display.setBrightness(isDND ? NIGHT_BRIGHTNESS : currentDayBrightness, true);
  display.setSegments(segs);
  osdEndTime = millis() + durationMs;
}

void startCountdownTimer(int minutes) {
  if (minutes < 1 || minutes > 9) return;
  displayEnabled = true;
  osdEndTime = 0;
  timerActive = true;
  timerEndMs = millis() + ((unsigned long)minutes * 60000UL);
  Serial.printf("Meditation Timer Started: %d min(s)\n", minutes);
}

void cancelCountdownTimer() {
  if (!timerActive) return;
  timerActive = false;
  timerEndMs = 0;
  displayEnabled = true;
  // Briefly show "00:00" as confirmation before returning to clock
  display.setBrightness(isDND ? NIGHT_BRIGHTNESS : currentDayBrightness, true);
  display.showNumberDecEx(0, 0x40, true);
  osdEndTime = millis() + 700;
  Serial.println("Meditation Timer Cancelled");
}

// --- Low-Noise / Low-Current Wi-Fi Connection Routine ---
void connectWiFiHardened() {
  display.clear();

  WiFi.disconnect(true, true);
  delay(200);

  WiFi.onEvent(WiFiEvent);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setMinSecurity(WIFI_AUTH_WPA_WPA2_PSK);
  WiFi.setAutoReconnect(true);

  WiFi.begin(ssid, password);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  WiFi.setTxPower(WIFI_POWER_15dBm);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    attempts++;

    if (attempts == 32) {
      WiFi.disconnect(false, true);
      delay(300);
      WiFi.begin(ssid, password);
    }

    if (attempts % 8 == 0) {
      uint8_t dot[] = { 0x00, 0x00, 0x00, 0x80 };
      display.setSegments(dot);
      delay(40);
      display.clear();
    }

    if (attempts >= 64) {
      WiFi.disconnect(true, true);
      delay(500);
      ESP.restart();
    }
  }
}

// --- Lightweight JSON Float-to-Rounded-Int Extractor ---
int extractJsonRoundedInt(const String& payload, const char* key, int fallback) {
  int idx = payload.lastIndexOf(key);
  if (idx == -1) return fallback;
  idx += strlen(key);
  while (idx < (int)payload.length() && (payload[idx] == ':' || payload[idx] == ' ' || payload[idx] == '[')) {
    idx++;
  }
  if (idx >= (int)payload.length()) return fallback;
  char c = payload[idx];
  if (!isDigit(c) && c != '-') return fallback;
  return (int)round(payload.substring(idx).toFloat());
}

// --- Lightweight JSON Array Peak Value Extractor (for 12-hour rain forecast) ---
int extractMaxJsonArrayInt(const String& payload, const char* key) {
  int idx = payload.lastIndexOf(key);
  if (idx == -1) return -1;
  int startBracket = payload.indexOf('[', idx);
  int endBracket   = payload.indexOf(']', startBracket);
  if (startBracket == -1 || endBracket == -1) return -1;

  String arr = payload.substring(startBracket + 1, endBracket);
  int maxVal = 0;
  int pos = 0;
  while (pos < (int)arr.length()) {
    int comma = arr.indexOf(',', pos);
    String token = (comma == -1) ? arr.substring(pos) : arr.substring(pos, comma);
    token.trim();
    if (token.length() > 0 && isDigit(token[0])) {
      int v = token.toInt();
      if (v > maxVal) maxVal = v;
    }
    if (comma == -1) break;
    pos = comma + 1;
  }
  return maxVal;
}

// --- Non-Blocking HTTPS Background Worker (Separate Scoped Clients) ---
void performWeatherFetch() {
  if (WiFi.status() != WL_CONNECTED) return;

  // 1. Fetch US AQI in its own clean TLS scope
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(6000);

    if (http.begin(client, urlAQI)) {
      int code = http.GET();
      if (code == 200) {
        String body = http.getString();
        int val = extractJsonRoundedInt(body, "\"us_aqi\"", -1);
        if (val >= 0) {
          portENTER_CRITICAL(&weatherMux);
          perungudiAQI = val;
          portEXIT_CRITICAL(&weatherMux);
          Serial.printf("[BG] Perungudi AQI: %d\n", val);
        }
      } else {
        Serial.printf("[BG] AQI HTTP Error: %d\n", code);
      }
      http.end();
    }
  }

  vTaskDelay(pdMS_TO_TICKS(150));

  // 2. Fetch Feels-Like, UV Index, Wind Gusts & 12h Peak Rain Chance
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(6000);

    if (http.begin(client, urlWeather)) {
      int code = http.GET();
      if (code == 200) {
        String body = http.getString();
        int f = extractJsonRoundedInt(body, "\"apparent_temperature\"", -99);
        int u = extractJsonRoundedInt(body, "\"uv_index\"", -1);
        int g = extractJsonRoundedInt(body, "\"wind_gusts_10m\"", -1);
        int r = extractMaxJsonArrayInt(body, "\"precipitation_probability\"");

        portENTER_CRITICAL(&weatherMux);
        if (f != -99) perungudiFeels = f;
        if (u >= 0)   perungudiUV    = u;
        if (g >= 0)   perungudiGusts = g;
        if (r >= 0)   perungudiRain  = r;
        portEXIT_CRITICAL(&weatherMux);

        Serial.printf("[BG] Weather -> Feels:%dC, UV:%d, Gusts:%dkm/h, Rain:%d%%\n", f, u, g, r);
      } else {
        Serial.printf("[BG] Weather HTTP Error: %d\n", code);
      }
      http.end();
    }
  }
}

void weatherTask(void* pvParameters) {
  unsigned long lastSuccessMs = 0;

  for (;;) {
    bool hasMissingData = (perungudiAQI < 0 || perungudiFeels == -99);
    unsigned long intervalMs = hasMissingData ? 15000UL : 1200000UL;

    if (triggerWeatherRefresh || (millis() - lastSuccessMs >= intervalMs)) {
      triggerWeatherRefresh = false;
      performWeatherFetch();
      lastSuccessMs = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// --- Helper to Render "<PrefixChar> <2-or-3-digit number>" ---
void renderPrefixedStat(uint8_t prefixSeg, int val, int minVal, int maxVal, bool allowNegative) {
  if ((allowNegative && val == -99) || (!allowNegative && val < 0)) {
    uint8_t err[] = { prefixSeg, 0x00, 0x40, 0x40 }; // "X --"
    display.setSegments(err);
    return;
  }

  int v = constrain(val, minVal, maxVal);
  uint8_t segs[4];
  segs[0] = prefixSeg;

  if (v < 0) {
    segs[1] = 0x40; // '-'
    int absV = abs(v);
    segs[2] = (absV >= 10) ? display.encodeDigit(absV / 10) : 0x00;
    segs[3] = display.encodeDigit(absV % 10);
  } else if (v >= 100) {
    segs[1] = display.encodeDigit(v / 100);
    segs[2] = display.encodeDigit((v / 10) % 10);
    segs[3] = display.encodeDigit(v % 10);
  } else {
    segs[1] = 0x00;
    segs[2] = (v >= 10) ? display.encodeDigit(v / 10) : 0x00;
    segs[3] = display.encodeDigit(v % 10);
  }
  display.setSegments(segs);
}

// --- Render Indoor Temperature (e.g., "28°C") ---
void renderTemperature() {
  if (currentTempC == -99) {
    uint8_t err[] = { 0x40, 0x40, 0x63, 0x39 }; // "--°C"
    display.setSegments(err);
    return;
  }
  uint8_t segs[4];
  int t = constrain(currentTempC, -9, 99);
  if (t < 0) {
    segs[0] = 0x40;
    segs[1] = display.encodeDigit(abs(t));
  } else {
    segs[0] = (t >= 10) ? display.encodeDigit(t / 10) : 0x00;
    segs[1] = display.encodeDigit(t % 10);
  }
  segs[2] = 0x63; // '°'
  segs[3] = 0x39; // 'C'
  display.setSegments(segs);
}

// --- Render Indoor Humidity (e.g., "65 H") ---
void renderHumidity() {
  if (currentHum == -99) {
    uint8_t err[] = { 0x40, 0x40, 0x00, 0x76 }; // "-- H"
    display.setSegments(err);
    return;
  }
  uint8_t segs[4];
  int h = constrain(currentHum, 0, 99);
  segs[0] = (h >= 10) ? display.encodeDigit(h / 10) : 0x00;
  segs[1] = display.encodeDigit(h % 10);
  segs[2] = 0x00;
  segs[3] = 0x76; // 'H'
  display.setSegments(segs);
}

void readDHTSensor() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    currentHum   = (int)round(h);
    currentTempC = (int)round(t);
  }
}

void handleIR() {
  if (!IrReceiver.decode()) return;

  // Only accept valid NEC commands from standard remote address 0x00 and ignore repeats/noise
  if (IrReceiver.decodedIRData.protocol == NEC &&
      IrReceiver.decodedIRData.address == 0x00 &&
      !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {

    uint16_t cmd = IrReceiver.decodedIRData.command;
    Serial.print("IR Command Received: 0x");
    Serial.println(cmd, HEX);

    switch (cmd) {
      case IR_CMD_0:
        cancelCountdownTimer();
        break;

      case IR_CMD_1: startCountdownTimer(1); break;
      case IR_CMD_2: startCountdownTimer(2); break;
      case IR_CMD_3: startCountdownTimer(3); break;
      case IR_CMD_4: startCountdownTimer(4); break;
      case IR_CMD_5: startCountdownTimer(5); break;
      case IR_CMD_6: startCountdownTimer(6); break;
      case IR_CMD_7: startCountdownTimer(7); break;
      case IR_CMD_8: startCountdownTimer(8); break;
      case IR_CMD_9: startCountdownTimer(9); break;

      case IR_CMD_POWER:
        displayEnabled = !displayEnabled;
        playClickBeep();
        if (!displayEnabled) {
          display.clear();
          osdEndTime = 0;
        } else {
          showOSD(0x00, 0x3F, 0x54, 0x00, 800); // "On"
        }
        break;

      case IR_CMD_DND: {
        bool currentDnd = (manualDndOverride != -1) ? (manualDndOverride == 1) : (lastAutoDndState == 1);
        manualDndOverride = currentDnd ? 0 : 1;
        isDND = (manualDndOverride == 1);
        prefs.putInt("dnd", manualDndOverride);

        playClickBeep();
        if (manualDndOverride == 1) {
          silenceBuzzer();
          showOSD(0x5E, 0x54, 0x5E, display.encodeDigit(1), 1200); // "dnd1"
        } else {
          showOSD(0x5E, 0x54, 0x5E, display.encodeDigit(0), 1200); // "dnd0"
        }
        break;
      }

      case IR_CMD_BRIGHTNESS:
        displayEnabled = true;
        currentDayBrightness += 2;
        if (currentDayBrightness > 7) currentDayBrightness = 1;
        prefs.putUChar("bright", currentDayBrightness);
        playClickBeep();
        showOSD(0x7C, 0x50, 0x00, display.encodeDigit(currentDayBrightness), 1200); // "br X"
        break;

      case IR_CMD_CHIME:
        playHourlyChime(); // Strictly respects buzzerEnabled && !isDND
        break;

      case IR_CMD_VOL_DOWN:
      case IR_CMD_VOL_UP:
        buzzerEnabled = !buzzerEnabled;
        prefs.putBool("sound", buzzerEnabled);
        if (buzzerEnabled) {
          playClickBeep();
          showOSD(0x6D, 0x54, 0x5E, display.encodeDigit(1), 1200); // "Snd1"
        } else {
          silenceBuzzer();
          showOSD(0x6D, 0x54, 0x5E, display.encodeDigit(0), 1200); // "Snd0"
        }
        break;

      case IR_CMD_EQ:
        displayEnabled = true;
        displayMode = (displayMode + 1) % 8;
        playClickBeep();
        osdEndTime = 0;
        if (displayMode >= 3 && (perungudiAQI < 0 || perungudiFeels == -99)) {
          triggerWeatherRefresh = true;
        }
        break;
    }
  }

  IrReceiver.resume();
}

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  silenceBuzzer();

  pinMode(8, OUTPUT);
  digitalWrite(8, HIGH); // Turn OFF active-LOW blue LED on Super Mini
  Serial.begin(115200);

  // Restore saved user settings across reboots
  prefs.begin("clock", false);
  buzzerEnabled        = prefs.getBool("sound", true);
  manualDndOverride    = prefs.getInt("dnd", -1);
  currentDayBrightness = prefs.getUChar("bright", DAY_BRIGHTNESS);
  if (manualDndOverride == 1) isDND = true;

  display.setBrightness(isDND ? NIGHT_BRIGHTNESS : currentDayBrightness);

  // Initialize DHT11 & IR Receiver
  dht.begin();
  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK);

  // Stage 1: Quiet / Low-Current Wi-Fi Connection
  connectWiFiHardened();

  // Stage 2: Wi-Fi Connected -> Request NTP immediately
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);

  uint8_t ntpText[] = { 0x54, 0x78, 0x73, 0x00 }; // ntP
  display.setSegments(ntpText);

  int ntpPolls = 0;
  while (time(nullptr) < 1700000000) {
    delay(250);
    ntpPolls++;

    if (ntpPolls % 4 == 0) {
      uint8_t retryText[] = { 0x54, 0x78, 0x73, display.encodeDigit((ntpPolls / 4) % 10) };
      display.setSegments(retryText);
    }

    if (ntpPolls % 20 == 0) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
    }

    if (ntpPolls >= 100) {
      ESP.restart();
    }
  }

  // Evaluate DND state at startup
  struct tm bootTime;
  if (getLocalTime(&bootTime, 200)) {
    bool autoDnd = (bootTime.tm_hour >= 23 || bootTime.tm_hour < 6);
    lastAutoDndState = autoDnd ? 1 : 0;
    isDND = (manualDndOverride != -1) ? (manualDndOverride == 1) : autoDnd;
  }

  // Initial DHT11 reading + start background HTTPS weather task (12KB stack for TLS)
  readDHTSensor();
  xTaskCreate(
    weatherTask,
    "WeatherTask",
    12288,
    NULL,
    1,
    NULL
  );

  // Startup remains 100% silent
}

void loop() {
  handleIR();

  // --- Check if Active Meditation Timer Has Finished ---
  if (timerActive && (long)(millis() - timerEndMs) >= 0) {
    timerActive = false;
    timerEndMs = 0;

    if (displayEnabled) {
      display.setBrightness(isDND ? NIGHT_BRIGHTNESS : currentDayBrightness, true);
      display.showNumberDecEx(0, 0x40, true); // Flash 00:00 while chiming
    }
    playTimerDoneTone();
  }

  // Refresh DHT11 every 3 seconds
  if (millis() - lastDhtRead >= 3000) {
    lastDhtRead = millis();
    readDHTSensor();
  }

  if (millis() - lastDisplayUpdate >= 200) {
    lastDisplayUpdate = millis();

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) {
      if (displayEnabled && !timerActive) {
        uint8_t err[] = { 0x79, 0x50, 0x50, 0x00 };
        display.setSegments(err);
      }
      return;
    }

    int hour24 = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;
    int second = timeinfo.tm_sec;
    int dayOfYear = timeinfo.tm_yday;

    bool autoDnd = (hour24 >= 23 || hour24 < 6);
    if (lastAutoDndState != -1 && autoDnd != (bool)lastAutoDndState) {
      manualDndOverride = -1;
      prefs.putInt("dnd", -1);
    }
    lastAutoDndState = autoDnd ? 1 : 0;

    isDND = (manualDndOverride != -1) ? (manualDndOverride == 1) : autoDnd;

    if (millis() >= osdEndTime) {
      if (!displayEnabled) {
        display.clear();
      } else {
        display.setBrightness(isDND ? NIGHT_BRIGHTNESS : currentDayBrightness);

        if (timerActive) {
          // --- Render Active Countdown Timer (MM:SS) ---
          long remainingMs = (long)(timerEndMs - millis());
          if (remainingMs < 0) remainingMs = 0;

          int totalSeconds = (remainingMs + 999) / 1000; // Ceiling so 5:00 displays cleanly at start
          int remMin = totalSeconds / 60;
          int remSec = totalSeconds % 60;

          int timerDigits = (remMin * 100) + remSec;
          uint8_t colonMask = ((remainingMs / 500) % 2 == 0) ? 0x40 : 0x00;
          display.showNumberDecEx(timerDigits, colonMask, true);
        } else {
          // --- Normal Clock / Sensor Display Modes ---
          portENTER_CRITICAL(&weatherMux);
          int aqi   = perungudiAQI;
          int feels = perungudiFeels;
          int uv    = perungudiUV;
          int gusts = perungudiGusts;
          int rain  = perungudiRain;
          portEXIT_CRITICAL(&weatherMux);

          switch (displayMode) {
            case 1:
              renderTemperature(); // Indoor Temp (e.g., "29°C")
              break;
            case 2:
              renderHumidity();    // Indoor Humidity (e.g., "68 H")
              break;
            case 3:
              renderPrefixedStat(0x77, aqi, 0, 999, false);     // 'A' : US AQI
              break;
            case 4:
              renderPrefixedStat(0x5C, feels, -19, 99, true);   // 'o' : Outdoor Feels-Like °C
              break;
            case 5:
              renderPrefixedStat(0x3E, uv, 0, 20, false);       // 'U' : UV Index
              break;
            case 6:
              renderPrefixedStat(0x3D, gusts, 0, 199, false);   // 'G' : Wind Gusts (km/h)
              break;
            case 7:
              renderPrefixedStat(0x50, rain, 0, 100, false);    // 'r' : Peak Rain Chance %
              break;
            default: {
              int displayHour = hour24 % 12;
              if (displayHour == 0) displayHour = 12;

              int displayTime = (displayHour * 100) + minute;
              uint8_t colonMask = (second % 2 == 0) ? 0x40 : 0x00;
              display.showNumberDecEx(displayTime, colonMask, false);
              break;
            }
          }
        }
      }
    }

    // --- 6:00 AM Short Alarm ---
    if (buzzerEnabled && !isDND && hour24 == 6 && minute == 0 && second == 0 && dayOfYear != lastAlarmDay) {
      lastAlarmDay = dayOfYear;
      lastChimeHour = 6;
      play6AMAlarm();
    }

    // --- Hourly Chime ---
    if (buzzerEnabled && !isDND && hour24 != 6 && minute == 0 && second == 0 && hour24 != lastChimeHour) {
      lastChimeHour = hour24;
      playHourlyChime();
    }
  }

  // --- Silent Background Wi-Fi Reconnect Check (every 60s) ---
  if (millis() - lastWifiCheck > 60000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false, true);
      delay(100);
      WiFi.begin(ssid, password);
    }
  }
}
