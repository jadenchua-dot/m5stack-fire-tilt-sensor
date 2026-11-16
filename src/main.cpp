// M5Stack Fire - tilt sensor with alarm, LED bars, web dashboard and phone push.
//
// How tilt is measured: the IMU's accelerometer gives the direction of gravity,
// smoothed into a steady "which way is down" vector. Tilt = angle between that
// vector and the one saved when you pressed ZERO, so it works in any mounting
// orientation. (The gyro measures spin rate, not angle - it's only logged.)
//
// Buttons:  A = ZERO (current position becomes "level")
//           B = mute / unmute the beeper
//           C = cycle alarm limit (5 / 10 / 15 / 20 / 30 degrees)

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "config.h"
#include "web_page.h"

// ---------------------------------------------------------------- hardware
static constexpr int LED_PIN = 15;    // M5Stack Fire side LED bars
static constexpr int LED_COUNT = 10;
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
M5Canvas canvas(&M5.Display);
WebServer server(80);
Preferences prefs;

// ---------------------------------------------------------------- state
struct Vec3 { float x, y, z; };
static inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static inline Vec3 norm(const Vec3& v) { float m = sqrtf(dot(v, v)); return m > 1e-6f ? Vec3{v.x / m, v.y / m, v.z / m} : Vec3{0, 0, 1}; }

Vec3 grav = {0, 0, 1};     // fused gravity direction (body frame)
Vec3 accLP = {0, 0, 1};    // smoothed raw accelerometer, used for screen rotation
int screenRot = ROT_TOP_UP;
Vec3 rawAcc, rawGyro;      // last raw readings (for the serial log)
uint32_t imuSamples = 0;
Vec3 ref = {0, 0, 1};      // "level" reference
Vec3 refU, refV;           // two axes perpendicular to ref, for X/Y tilt
bool imuStarted = false;

float tiltDeg = 0, tiltX = 0, tiltY = 0;
float limitDeg = DEFAULT_LIMIT_DEG;
bool alarmOn = false, warnOn = false, muted = false;
uint32_t overSince = 0, lastBeep = 0;

String ipText = "Wi-Fi: starting...";
String ipShort = "";       // shorter version for portrait
bool apMode = false;

struct Event { uint32_t t; bool alarm; float deg; };
static constexpr int MAX_EVENTS = 12;
Event events[MAX_EVENTS];
int eventCount = 0;

struct Notice { bool alarm; float deg; };
QueueHandle_t notifyQueue;

// ---------------------------------------------------------------- helpers
void buildRefAxes() {
  // pick a body axis not parallel to ref, project it onto the plane
  Vec3 a = fabsf(ref.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  float d = dot(a, ref);
  refU = norm({a.x - d * ref.x, a.y - d * ref.y, a.z - d * ref.z});
  refV = cross(ref, refU);
}

void saveSettings() {
  prefs.putFloat("rx", ref.x); prefs.putFloat("ry", ref.y); prefs.putFloat("rz", ref.z);
  prefs.putFloat("lim", limitDeg);
  prefs.putBool("mute", muted);
}

void loadSettings() {
  ref = norm({prefs.getFloat("rx", 0), prefs.getFloat("ry", 0), prefs.getFloat("rz", 1)});
  limitDeg = prefs.getFloat("lim", DEFAULT_LIMIT_DEG);
  muted = prefs.getBool("mute", false);
  buildRefAxes();
}

void zeroHere() {
  ref = grav;
  buildRefAxes();
  alarmOn = false; overSince = 0;
  saveSettings();
  M5.Speaker.tone(2600, 60);
  Serial.printf("Zeroed. ref = %.3f %.3f %.3f\n", ref.x, ref.y, ref.z);
}

void setLimit(float d) {
  limitDeg = constrain(d, 1.0f, 60.0f);
  saveSettings();
}

void addEvent(bool alarm) {
  if (eventCount == MAX_EVENTS) { memmove(events, events + 1, sizeof(Event) * (MAX_EVENTS - 1)); eventCount--; }
  events[eventCount++] = {millis(), alarm, tiltDeg};
  Notice n{alarm, tiltDeg};
  xQueueSend(notifyQueue, &n, 0);
}

// ---------------------------------------------------------------- IMU fusion
void updateImu() {
  static uint32_t lastUs = micros();
  if (!M5.Imu.update()) return;
  auto d = M5.Imu.getImuData();

  uint32_t nowUs = micros();
  float dt = (nowUs - lastUs) * 1e-6f;
  lastUs = nowUs;
  if (dt <= 0 || dt > 0.5f) dt = 0.05f;

  Vec3 acc = {d.accel.x, d.accel.y, d.accel.z};
  rawAcc = acc;
  rawGyro = {d.gyro.x, d.gyro.y, d.gyro.z};
  imuSamples++;
  float accMag = sqrtf(dot(acc, acc));
  Vec3 accN = norm(acc);

  if (!imuStarted) { grav = accN; accLP = accN; imuStarted = true; return; }
  accLP = {accLP.x + (accN.x - accLP.x) * 0.1f,
           accLP.y + (accN.y - accLP.y) * 0.1f,
           accLP.z + (accN.z - accLP.z) * 0.1f};

  // Tilt comes straight from gravity (accelerometer), smoothed over time.
  // No gyro integration: the loop only samples ~20 Hz (screen redraws), and
  // integrating the gyro that slowly made the reading wander after handling.
  // While being shaken (|a| far from 1 g) we smooth harder.
  float tau = (accMag > 0.9f && accMag < 1.1f) ? 0.25f : 1.0f;   // seconds
  float a = 1.0f - expf(-dt / tau);
  grav = norm({grav.x + (accN.x - grav.x) * a,
               grav.y + (accN.y - grav.y) * a,
               grav.z + (accN.z - grav.z) * a});

  float c = constrain(dot(grav, ref), -1.0f, 1.0f);
  tiltDeg = acosf(c) * 57.29578f;
  tiltX = atan2f(dot(grav, refU), c) * 57.29578f;
  tiltY = atan2f(dot(grav, refV), c) * 57.29578f;
}

// ---------------------------------------------------------------- alarm logic
void updateAlarm() {
  uint32_t now = millis();
  warnOn = !alarmOn && tiltDeg > limitDeg * WARN_FRACTION;

  if (!alarmOn) {
    if (tiltDeg > limitDeg) {
      if (overSince == 0) overSince = now ? now : 1;
      if (now - overSince >= ALARM_DELAY_MS) {
        alarmOn = true; warnOn = false;
        lastBeep = 0;
        addEvent(true);
        Serial.printf("ALARM: tilt %.1f > %.1f\n", tiltDeg, limitDeg);
      }
    } else {
      overSince = 0;
    }
  } else if (tiltDeg < limitDeg - HYSTERESIS_DEG) {
    alarmOn = false; overSince = 0;
    addEvent(false);
    if (!muted) { M5.Speaker.tone(1200, 80); }
    Serial.printf("Cleared: tilt %.1f\n", tiltDeg);
  }

  if (alarmOn && !muted && now - lastBeep >= BEEP_INTERVAL_MS) {
    M5.Speaker.tone(2200, 250);
    lastBeep = now;
  }
}

// ---------------------------------------------------------------- LEDs
void updateLeds() {
  static uint32_t last = 0;
  if (millis() - last < 50) return;
  last = millis();
  uint32_t col;
  if (alarmOn)      col = ((millis() / 250) % 2) ? leds.Color(255, 0, 0) : 0;
  else if (warnOn)  col = leds.Color(255, 140, 0);
  else              col = leds.Color(0, 200, 40);
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, col);
  leds.show();
}

// ---------------------------------------------------------------- screen
void applyRotation(int rot) {
  screenRot = rot;
  M5.Display.setRotation(rot);
  if (canvas.width() != M5.Display.width()) {          // portrait <-> landscape
    canvas.deleteSprite();
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
      canvas.setColorDepth(8);
      canvas.createSprite(M5.Display.width(), M5.Display.height());
    }
  }
}

// Pick the screen rotation from which edge of the device points up.
// Ignored while lying flat; needs ~0.4 s of steady holding before it flips.
void updateRotation() {
#if AUTO_ROTATE
  static int pending = -1;
  static uint32_t pendingSince = 0;
  static uint32_t lastLog = 0;

  float inPlane = sqrtf(accLP.x * accLP.x + accLP.y * accLP.y);
  if (inPlane < 0.55f) { pending = -1; return; }      // too flat to tell

  // On the Fire the IMU sits rotated 90 deg: +X points to the screen's top
  // edge and +Y to its left edge (checked with photos of the real device).
  // ang = direction of "up" in the IMU's X/Y plane.
  float ang = atan2f(accLP.x, accLP.y) * 57.29578f;
  int want;
  float centre;
  if (ang >= -45 && ang < 45)       { want = ROT_LEFT_UP;   centre = 0; }    // +Y up
  else if (ang >= 45 && ang < 135)  { want = ROT_TOP_UP;    centre = 90; }   // +X up
  else if (ang >= -135 && ang < -45){ want = ROT_BOTTOM_UP; centre = -90; }  // -X up
  else                              { want = ROT_RIGHT_UP;  centre = 180; }  // -Y up
  float off = fabsf(ang - centre);
  if (off > 180) off = 360 - off;

  if (millis() - lastLog > 1000) {
    lastLog = millis();
    Serial.printf("orient: acc %+.2f %+.2f %+.2f  up-angle %+.0f  rot %d\n",
                  accLP.x, accLP.y, accLP.z, ang, screenRot);
  }

  // only switch when clearly in the new quadrant (avoids flicker at 45 deg)
  if (want == screenRot || off > 30) { pending = -1; return; }
  if (want != pending) { pending = want; pendingSince = millis(); return; }
  if (millis() - pendingSince > 400) { applyRotation(want); pending = -1; }
#endif
}

void drawDegree(int x, int y, int r, uint16_t col) {
  canvas.drawCircle(x, y, r, col);
  canvas.drawCircle(x, y, r - 1, col);
}

void drawScreen() {
  static uint32_t last = 0;
  if (millis() - last < 50) return;   // ~20 fps
  last = millis();
  const int W = canvas.width(), H = canvas.height();
  const bool portrait = H > W;
  const bool upsideDown = screenRot == ROT_BOTTOM_UP;   // buttons now at the top
  const int y0 = upsideDown ? 22 : 0;                   // room for button labels

  bool blink = (millis() / 400) % 2;
  uint16_t bg, fg = TFT_WHITE;
  const char* title;
  const char* sub;
  if (alarmOn)     { bg = blink ? canvas.color565(210, 25, 25) : canvas.color565(140, 0, 0); title = "TILTED!"; sub = "Straighten it"; }
  else if (warnOn) { bg = canvas.color565(245, 165, 0); fg = TFT_BLACK; title = "CAREFUL"; sub = "Near the limit"; }
  else             { bg = canvas.color565(25, 145, 60); title = "LEVEL"; sub = "Tilt is OK"; }

  canvas.fillScreen(bg);
  canvas.setTextColor(fg);

  // title
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.drawString(title, 12, y0 + 10);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString(sub, 14, y0 + 46);

  // big angle
  char buf[48];
  snprintf(buf, sizeof(buf), "%.1f", tiltDeg);
  canvas.setFont(&fonts::Font7);
  canvas.drawString(buf, 12, y0 + 78);
  int w = canvas.textWidth(buf);
  drawDegree(12 + w + 10, y0 + 84, 6, fg);

  // bubble level: right side in landscape, under the number in portrait
  const int R = portrait ? 56 : 60;
  const int cx = portrait ? W / 2 : 248;
  const int cy = portrait ? 200 : y0 + 104;
  float pxPerDeg = (R / 2.0f) / limitDeg;     // limit ring at half radius
  canvas.drawCircle(cx, cy, R, fg);
  canvas.drawCircle(cx, cy, R / 2, fg);
  canvas.drawFastHLine(cx - R, cy, 2 * R, fg);
  canvas.drawFastVLine(cx, cy - R, 2 * R, fg);
  float bx = tiltX * pxPerDeg, by = tiltY * pxPerDeg;
  float m = sqrtf(bx * bx + by * by);
  if (m > R - 9) { bx *= (R - 9) / m; by *= (R - 9) / m; }
  canvas.fillCircle(cx + (int)bx, cy + (int)by, 9, fg);
  canvas.fillCircle(cx + (int)bx, cy + (int)by, 5, bg);

  // details
  canvas.setFont(&fonts::FreeSans9pt7b);
  if (portrait) {
    snprintf(buf, sizeof(buf), "X %+.1f   Y %+.1f", tiltX, tiltY);
    canvas.drawString(buf, 12, 264);
    snprintf(buf, sizeof(buf), "Limit %.1f%s", limitDeg, muted ? "   (muted)" : "");
    canvas.drawString(buf, 12, 284);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(ipShort, 12, 304);
  } else {
    snprintf(buf, sizeof(buf), "X %+.1f   Y %+.1f   Limit %.1f", tiltX, tiltY, limitDeg);
    canvas.drawString(buf, 12, y0 + 172);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(ipText, 12, y0 + 196);

    // button labels next to the physical A / B / C buttons
    const char* mid = muted ? "UNMUTE" : "MUTE";
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    if (upsideDown) {   // buttons along the top, A now on the right
      canvas.setTextDatum(top_center);
      canvas.drawString("LIMIT", 65, 3);
      canvas.drawString(mid, 160, 3);
      canvas.drawString("ZERO", 255, 3);
    } else {
      canvas.setTextDatum(bottom_center);
      canvas.drawString("ZERO", 65, 238);
      canvas.drawString(mid, 160, 238);
      canvas.drawString("LIMIT", 255, 238);
    }
  }

  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- buttons
void handleButtons() {
  if (M5.BtnA.wasClicked()) zeroHere();
  if (M5.BtnB.wasClicked()) {
    muted = !muted; saveSettings();
    if (!muted) M5.Speaker.tone(2000, 60);
  }
  if (M5.BtnC.wasClicked()) {
    static const float steps[] = {5, 10, 15, 20, 30};
    float next = steps[0];
    for (float s : steps) if (s > limitDeg + 0.01f) { next = s; break; }
    setLimit(next);
    if (!muted) M5.Speaker.tone(2400, 40);
  }
}

// ---------------------------------------------------------------- web
void sendStatus() {
  String j;
  j.reserve(900);
  j += "{\"tilt\":"; j += String(tiltDeg, 2);
  j += ",\"x\":"; j += String(tiltX, 2);
  j += ",\"y\":"; j += String(tiltY, 2);
  j += ",\"limit\":"; j += String(limitDeg, 1);
  j += ",\"alarm\":"; j += alarmOn ? "true" : "false";
  j += ",\"warn\":"; j += warnOn ? "true" : "false";
  j += ",\"muted\":"; j += muted ? "true" : "false";
  j += ",\"ip\":\""; j += apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); j += "\"";
  j += ",\"now\":"; j += millis();
  j += ",\"events\":[";
  for (int i = 0; i < eventCount; i++) {
    if (i) j += ",";
    j += "{\"t\":"; j += events[i].t;
    j += ",\"a\":"; j += events[i].alarm ? "true" : "false";
    j += ",\"deg\":"; j += String(events[i].deg, 1); j += "}";
  }
  j += "]}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/zero", HTTP_POST, [] { zeroHere(); sendStatus(); });
  server.on("/api/limit", HTTP_POST, [] {
    if (server.hasArg("deg")) setLimit(server.arg("deg").toFloat());
    sendStatus();
  });
  server.on("/api/mute", HTTP_POST, [] {
    muted = server.arg("on") == "1"; saveSettings(); sendStatus();
  });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void setupWifi() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("Connecting to Wi-Fi...", 160, 110);
  M5.Display.drawString(WIFI_SSID, 160, 135);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setAutoReconnect(true);
  // some routers reject the first attempt from an ESP32, so retry a few times
  for (int attempt = 1; attempt <= 4 && WiFi.status() != WL_CONNECTED; attempt++) {
    WiFi.disconnect(true);
    delay(300);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      wl_status_t s = WiFi.status();
      if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) break;
      delay(200);
    }
    Serial.printf("Wi-Fi attempt %d -> status %d\n", attempt, (int)WiFi.status());
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    ipText = "http://" + WiFi.localIP().toString() + "   (" DEVICE_HOSTNAME ".local)";
    ipShort = "http://" + WiFi.localIP().toString();
  } else {
    Serial.printf("Wi-Fi connect to \"%s\" failed, status=%d. Scanning...\n", WIFI_SSID, (int)WiFi.status());
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
      Serial.printf("  [%s] rssi=%d ch=%d enc=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);            // hotspot now, keep retrying home Wi-Fi (see wifiWatchdog)
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    apMode = true;
    ipText = "Hotspot " AP_SSID " / " AP_PASSWORD "  ->  http://" + WiFi.softAPIP().toString();
    ipShort = "Hotspot " AP_SSID "  " + WiFi.softAPIP().toString();
  }
  MDNS.begin(DEVICE_HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  Serial.println(ipText);
}

// In hotspot mode, keep trying the home Wi-Fi every 20 s; switch over once it works.
void wifiWatchdog() {
  static uint32_t lastTry = 0;
  if (!apMode) return;
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ipText = "http://" + WiFi.localIP().toString() + "   (" DEVICE_HOSTNAME ".local)";
    ipShort = "http://" + WiFi.localIP().toString();
    Serial.println("Wi-Fi joined later: " + ipText);
    return;
  }
  if (millis() - lastTry > 20000) {
    lastTry = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Retrying home Wi-Fi...");
  }
}

// push notifications run on their own task so HTTPS never stalls the screen
void notifyTask(void*) {
  Notice n;
  for (;;) {
    if (xQueueReceive(notifyQueue, &n, portMAX_DELAY) != pdTRUE) continue;
    if (strlen(NTFY_TOPIC) == 0 || apMode || WiFi.status() != WL_CONNECTED) continue;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String("https://ntfy.sh/") + NTFY_TOPIC)) continue;
    char body[96];
    if (n.alarm) {
      snprintf(body, sizeof(body), "Tilted %.1f deg (limit %.1f). Please straighten it.", n.deg, limitDeg);
      http.addHeader("Title", "Tilt alarm!");
      http.addHeader("Priority", "high");
      http.addHeader("Tags", "warning");
    } else {
      snprintf(body, sizeof(body), "Back within limit (%.1f deg).", n.deg);
      http.addHeader("Title", "Tilt OK again");
      http.addHeader("Tags", "white_check_mark");
    }
    int code = http.POST((uint8_t*)body, strlen(body));
    Serial.printf("ntfy -> %d\n", code);
    http.end();
  }
}

// ---------------------------------------------------------------- main
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(ROT_TOP_UP);
  M5.Speaker.setVolume(SPEAKER_VOLUME);

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear(); leds.show();

  prefs.begin("tilt", false);
  loadSettings();

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    canvas.setColorDepth(8);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
  }

  if (!M5.Imu.isEnabled()) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("IMU not found!", 20, 100, &fonts::FreeSansBold12pt7b);
    Serial.println("IMU not found");
    while (true) delay(1000);
  }

  notifyQueue = xQueueCreate(8, sizeof(Notice));
  xTaskCreatePinnedToCore(notifyTask, "notify", 8192, nullptr, 1, nullptr, 0);

  setupWifi();
  setupWeb();
}

void loop() {
  M5.update();
  updateImu();
  handleButtons();
  updateAlarm();
  updateLeds();
  updateRotation();
  drawScreen();
  server.handleClient();
  wifiWatchdog();

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 1000) {
    lastLog = millis();
    Serial.printf("tilt %.1f  x %+.1f  y %+.1f  %s | acc %+.2f %+.2f %+.2f |%.2f| gyro %+.1f %+.1f %+.1f | %u Hz\n",
                  tiltDeg, tiltX, tiltY, alarmOn ? "ALARM" : (warnOn ? "warn" : "ok"),
                  rawAcc.x, rawAcc.y, rawAcc.z, sqrtf(dot(rawAcc, rawAcc)),
                  rawGyro.x, rawGyro.y, rawGyro.z, imuSamples);
    imuSamples = 0;
  }
}
