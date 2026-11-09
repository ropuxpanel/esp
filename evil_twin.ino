#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

extern "C" {
#include "user_interface.h"
}

// ============================================
// CONFIG
// ============================================
#define ADMIN_SSID    "CSM Mohasin"
#define ADMIN_PASS    "samiya"
#define MAX_FAKE_APS  30
#define SSID_MAX_LEN  32
#define EEPROM_SIZE   (MAX_FAKE_APS * SSID_MAX_LEN + 1)
#define BEACON_CH     1       // সব beacon একই channel — AP-ও এই channel-এ
#define BEACON_MS     60      // per-SSID interval ms

const char AUDIO_B64[] PROGMEM = "YOUR_BASE64_STRING_HERE";
const size_t AUDIO_B64_LEN = sizeof(AUDIO_B64) - 1;

// ============================================
// GLOBALS
// ============================================
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress netmask(255, 255, 255, 0);

DNSServer  dnsServer;
ESP8266WebServer webServer(80);

char fakeSSIDs[MAX_FAKE_APS][SSID_MAX_LEN] = {
  "Free_WiFi",       "Airport_Free",    "Hotel_Guest",
  "Starbucks_WiFi",  "McD_FreeWiFi",    "Public_Net",
  "Open_Network",    "Guest_Access",    "FreeInternet",
  "WiFi_Free_Zone",  "CafeNet",         "Library_WiFi",
  "City_Free_WiFi",  "Plaza_Guest",     "Market_WiFi",
  "Shop_Free",       "Restaurant_Net",  "Mall_WiFi",
  "Park_WiFi",       "Station_Free",    "Bus_WiFi",
  "Train_Free",      "Terminal_WiFi",   "Lounge_Net",
  "VIP_Free",        "Premium_WiFi",    "Ultra_Free",
  "MegaNet_Free",    "SpeedNet_Open",   "ConnectFree"
};

// প্রতিটা fake SSID-এর জন্য fixed random MAC pool
// setup()-এ একবার generate হবে, loop-এ same MAC reuse
uint8_t fakeMacs[MAX_FAKE_APS][6];

unsigned long beacon_now = 0;
int beacon_idx = 0;

// ============================================
// MAC POOL — setup()-এ একবার generate
// ============================================
void generateMacPool() {
  randomSeed(ESP.getCycleCount());
  for (int i = 0; i < MAX_FAKE_APS; i++) {
    fakeMacs[i][0] = 0x02 | ((random(0, 0x3F) << 2) & 0xFC); // locally administered, unicast
    fakeMacs[i][1] = random(0, 256);
    fakeMacs[i][2] = random(0, 256);
    fakeMacs[i][3] = random(0, 256);
    fakeMacs[i][4] = random(0, 256);
    fakeMacs[i][5] = random(0, 256);
  }
}

// ============================================
// EEPROM — একবার begin, তারপর save/load
// ============================================
void saveSSIDs() {
  EEPROM.write(0, 0xAA);
  for (int i = 0; i < MAX_FAKE_APS; i++)
    for (int j = 0; j < SSID_MAX_LEN; j++)
      EEPROM.write(1 + i * SSID_MAX_LEN + j, fakeSSIDs[i][j]);
  EEPROM.commit();
}

void loadSSIDs() {
  if (EEPROM.read(0) != 0xAA) return;
  for (int i = 0; i < MAX_FAKE_APS; i++) {
    for (int j = 0; j < SSID_MAX_LEN; j++)
      fakeSSIDs[i][j] = EEPROM.read(1 + i * SSID_MAX_LEN + j);
    fakeSSIDs[i][SSID_MAX_LEN - 1] = '\0';
  }
}

// ============================================
// BEACON FRAME INJECTION
// সব SSID → BEACON_CH → DNS wildcard → একই captive portal
// ============================================
void sendBeacon(int idx) {
  const char* ssid   = fakeSSIDs[idx];
  uint8_t*    mac    = fakeMacs[idx];
  uint8_t     ssid_len = strlen(ssid);
  if (ssid_len == 0 || ssid_len >= SSID_MAX_LEN) return;

  wifi_set_channel(BEACON_CH);

  uint8_t pkt[128];
  memset(pkt, 0, sizeof(pkt));

  // Frame control — beacon
  pkt[0] = 0x80; pkt[1] = 0x00;
  // Duration
  pkt[2] = 0x00; pkt[3] = 0x00;
  // Destination — broadcast
  memset(&pkt[4], 0xFF, 6);
  // Source MAC (fake, unique per SSID)
  memcpy(&pkt[10], mac, 6);
  // BSSID = Source MAC
  memcpy(&pkt[16], mac, 6);
  // Sequence control
  pkt[22] = 0x00; pkt[23] = 0x00;

  // Fixed parameters (8 bytes)
  // Timestamp — 8 bytes microseconds
  uint64_t ts = (uint64_t)micros();
  memcpy(&pkt[24], &ts, 8);
  // Beacon interval — 100 TU = 0x0064
  pkt[32] = 0x64; pkt[33] = 0x00;
  // Capability info — ESS + open (no encryption → client connects easier)
  pkt[34] = 0x01; pkt[35] = 0x00;

  // Tagged parameters
  uint8_t off = 36;
  // SSID tag
  pkt[off++] = 0x00;
  pkt[off++] = ssid_len;
  memcpy(&pkt[off], ssid, ssid_len);
  off += ssid_len;
  // Supported rates tag
  pkt[off++] = 0x01;
  pkt[off++] = 0x08;
  uint8_t rates[] = {0x82,0x84,0x8B,0x96,0x24,0x30,0x48,0x6C};
  memcpy(&pkt[off], rates, 8);
  off += 8;
  // DS Parameter (channel)
  pkt[off++] = 0x03;
  pkt[off++] = 0x01;
  pkt[off++] = BEACON_CH;

  wifi_send_pkt_freedom(pkt, off, 0);
}

// ============================================
// CAPTIVE PORTAL — সব SSID connect করলে এখানে আসবে
// ============================================
void sendCaptivePortal() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");

  webServer.sendContent(F("<!DOCTYPE html><html><head>"));
  webServer.sendContent(F("<meta charset='UTF-8'>"));
  webServer.sendContent(F("<meta name='viewport' content='width=device-width,initial-scale=1.0'>"));
  webServer.sendContent(F("<title>Connecting...</title><style>"));
  webServer.sendContent(F("*{margin:0;padding:0;box-sizing:border-box}"));
  webServer.sendContent(F("body{background:#000;color:#0ff;font-family:monospace;"));
  webServer.sendContent(F("min-height:100vh;display:flex;align-items:center;"));
  webServer.sendContent(F("justify-content:center;overflow:hidden}"));
  webServer.sendContent(F(".overlay{position:fixed;inset:0;background:#000;z-index:9999;"));
  webServer.sendContent(F("display:flex;align-items:center;justify-content:center;"));
  webServer.sendContent(F("flex-direction:column;gap:16px;cursor:pointer}"));
  webServer.sendContent(F(".tap{color:#0ff;font-size:1.1em;animation:blink 0.8s infinite}"));
  webServer.sendContent(F(".spinner{width:38px;height:38px;border:3px solid #001a1a;"));
  webServer.sendContent(F("border-top:3px solid #0ff;border-radius:50%;"));
  webServer.sendContent(F("animation:spin 1s linear infinite}"));
  webServer.sendContent(F("@keyframes spin{to{transform:rotate(360deg)}}"));
  webServer.sendContent(F("@keyframes blink{0%,100%{opacity:1}50%{opacity:0.2}}"));
  webServer.sendContent(F("@keyframes bgf{0%{background:#000}15%{background:#001a1a}"));
  webServer.sendContent(F("35%{background:#000}60%{background:#001111}100%{background:#000}}"));
  webServer.sendContent(F(".ab{animation:bgf 0.2s infinite}"));
  webServer.sendContent(F(".glitch{font-size:2.2em;font-weight:bold;color:#0ff;"));
  webServer.sendContent(F("text-align:center;text-shadow:2px 0 #f0f,-2px 0 #0f0;"));
  webServer.sendContent(F("animation:gt 0.25s infinite}"));
  webServer.sendContent(F("@keyframes gt{"));
  webServer.sendContent(F("0%{text-shadow:2px 0 #f0f,-2px 0 #0f0;transform:translate(0)}"));
  webServer.sendContent(F("25%{text-shadow:-3px 0 #f0f,3px 0 #0f0;transform:translate(-2px,1px)}"));
  webServer.sendContent(F("50%{text-shadow:3px 0 #0f0,-3px 0 #f0f;transform:translate(2px,-1px)}"));
  webServer.sendContent(F("75%{text-shadow:-2px 0 #f0f,2px 0 #0f0;transform:translate(1px,0)}"));
  webServer.sendContent(F("100%{text-shadow:2px 0 #f0f,-2px 0 #0f0;transform:translate(0)}}"));
  webServer.sendContent(F(".sl{position:fixed;inset:0;pointer-events:none;z-index:998;"));
  webServer.sendContent(F("background:repeating-linear-gradient(0deg,transparent,transparent 2px,"));
  webServer.sendContent(F("rgba(0,255,255,0.03) 2px,rgba(0,255,255,0.03) 4px)}"));
  webServer.sendContent(F(".card{text-align:center;padding:40px 30px;"));
  webServer.sendContent(F("position:relative;z-index:1;display:none}"));
  webServer.sendContent(F(".sub{color:#0a0;font-size:0.85em;margin-top:14px;"));
  webServer.sendContent(F("animation:blink 1s infinite}"));
  webServer.sendContent(F(".bar{width:200px;height:4px;background:#001;"));
  webServer.sendContent(F("margin:20px auto;overflow:hidden}"));
  webServer.sendContent(F(".bar::after{content:'';display:block;width:50%;height:100%;"));
  webServer.sendContent(F("background:linear-gradient(90deg,transparent,#0ff,transparent);"));
  webServer.sendContent(F("animation:sc 1.5s infinite}"));
  webServer.sendContent(F("@keyframes sc{0%{transform:translateX(-100%)}"));
  webServer.sendContent(F("100%{transform:translateX(300%)}}"));
  webServer.sendContent(F("</style></head><body id='bd'>"));

  webServer.sendContent(F("<div class='overlay' id='ov' onclick='go()'>"));
  webServer.sendContent(F("<div class='spinner'></div>"));
  webServer.sendContent(F("<div class='tap'>Tap to connect...</div>"));
  webServer.sendContent(F("</div>"));
  webServer.sendContent(F("<div class='sl'></div>"));
  webServer.sendContent(F("<div class='card' id='ct'>"));
  webServer.sendContent(F("<div class='glitch'>FREE WiFi</div>"));
  webServer.sendContent(F("<div class='bar'></div>"));
  webServer.sendContent(F("<div class='sub'>CONNECTING TO NETWORK...</div>"));
  webServer.sendContent(F("</div>"));

  // Audio
  webServer.sendContent(F("<audio id='snd' loop preload='auto' src='data:audio/mpeg;base64,"));
  char buf[257];
  for (size_t i = 0; i < AUDIO_B64_LEN; i += 256) {
    size_t sz = (AUDIO_B64_LEN - i < 256) ? (AUDIO_B64_LEN - i) : 256;
    memcpy_P(buf, AUDIO_B64 + i, sz);
    buf[sz] = '\0';
    webServer.sendContent(buf);
  }
  webServer.sendContent(F("'></audio>"));

  webServer.sendContent(F("<script>"));
  webServer.sendContent(F("function go(){"));
  webServer.sendContent(F("document.getElementById('ov').style.display='none';"));
  webServer.sendContent(F("document.getElementById('ct').style.display='block';"));
  webServer.sendContent(F("document.getElementById('bd').classList.add('ab');"));
  webServer.sendContent(F("document.getElementById('snd').play().catch(function(){});"));
  webServer.sendContent(F("}"));
  webServer.sendContent(F("</script></body></html>"));
  webServer.sendContent("");
}

// ============================================
// ADMIN PANEL — chunked, no String heap frag
// ============================================
void handleAdmin() {
  if (webServer.method() == HTTP_POST) {
    for (int i = 0; i < MAX_FAKE_APS; i++) {
      String key = "s" + String(i);
      if (webServer.hasArg(key)) {
        String val = webServer.arg(key);
        val.trim();
        if (val.length() > 0 && val.length() < SSID_MAX_LEN) {
          strncpy(fakeSSIDs[i], val.c_str(), SSID_MAX_LEN - 1);
          fakeSSIDs[i][SSID_MAX_LEN - 1] = '\0';
        }
      }
    }
    saveSSIDs();
  }

  bool saved = (webServer.method() == HTTP_POST);
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");

  webServer.sendContent(F("<!DOCTYPE html><html><head>"));
  webServer.sendContent(F("<meta charset='UTF-8'>"));
  webServer.sendContent(F("<meta name='viewport' content='width=device-width,initial-scale=1'>"));
  webServer.sendContent(F("<title>Admin</title><style>"));
  webServer.sendContent(F("body{font-family:Arial,sans-serif;background:#111;"));
  webServer.sendContent(F("color:#0ff;margin:0;padding:16px}"));
  webServer.sendContent(F(".w{max-width:480px;margin:0 auto}"));
  webServer.sendContent(F("h1{color:#0ff;text-align:center;margin-bottom:4px;font-size:1.3em}"));
  webServer.sendContent(F(".s{text-align:center;color:#0a0;font-size:12px;margin-bottom:18px}"));
  webServer.sendContent(F("label{font-size:11px;color:#088;display:block;margin-top:6px}"));
  webServer.sendContent(F("input{width:100%;padding:7px;margin:2px 0 6px;"));
  webServer.sendContent(F("background:#001a1a;border:1px solid #066;color:#0ff;"));
  webServer.sendContent(F("border-radius:3px;font-size:13px}"));
  webServer.sendContent(F(".btn{width:100%;padding:11px;background:#0ff;color:#000;"));
  webServer.sendContent(F("border:none;border-radius:4px;font-size:14px;"));
  webServer.sendContent(F("font-weight:bold;cursor:pointer;margin-top:8px}"));
  webServer.sendContent(F(".btn:hover{background:#0cc}"));
  webServer.sendContent(F(".ok{background:#001a00;border:1px solid #0f0;color:#0f0;"));
  webServer.sendContent(F("padding:10px;border-radius:4px;text-align:center;margin-bottom:14px}"));
  webServer.sendContent(F("</style></head><body><div class='w'>"));
  webServer.sendContent(F("<h1>&#9760; CSM Mohasin Admin</h1>"));
  webServer.sendContent(F("<div class='s'>Fake AP Manager &mdash; সব AP → একই portal</div>"));

  if (saved)
    webServer.sendContent(F("<div class='ok'>&#10003; Saved!</div>"));

  webServer.sendContent(F("<form method='POST' action='/admin'>"));

  // Input fields — chunked, no String concat loop
  char line[128];
  for (int i = 0; i < MAX_FAKE_APS; i++) {
    snprintf(line, sizeof(line),
      "<label>AP %d</label>"
      "<input type='text' name='s%d' value='%s' maxlength='31'>",
      i + 1, i, fakeSSIDs[i]);
    webServer.sendContent(line);
  }

  webServer.sendContent(F("<button type='submit' class='btn'>&#128190; Save All</button>"));
  webServer.sendContent(F("</form></div></body></html>"));
  webServer.sendContent("");
}

// ============================================
// HANDLERS
// ============================================
void handleRoot()     { sendCaptivePortal(); }
void handleNotFound() { sendCaptivePortal(); }

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // EEPROM একবার init
  EEPROM.begin(EEPROM_SIZE);
  loadSSIDs();

  // MAC pool generate
  generateMacPool();

  // Packet injection unlock
  wifi_set_opmode(STATION_MODE);
  wifi_promiscuous_enable(1);
  delay(50);
  wifi_promiscuous_enable(0);

  // SoftAP — BEACON_CH-এ fixed
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netmask);
  WiFi.softAP(ADMIN_SSID, ADMIN_PASS, BEACON_CH);  // channel lock

  // DNS wildcard — সব domain → 192.168.4.1
  dnsServer.start(DNS_PORT, "*", apIP);

  webServer.on("/", handleRoot);
  webServer.on("/admin", handleAdmin);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  Serial.println(F("[+] Admin AP: " ADMIN_SSID));
  Serial.println(F("[+] Admin panel: http://192.168.4.1/admin"));
  Serial.println(F("[+] MAC pool generated"));
  Serial.printf("[+] Beaconing %d SSIDs on ch%d\n", MAX_FAKE_APS, BEACON_CH);

  beacon_now = millis();
}

// ============================================
// LOOP
// ============================================
void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  if (millis() - beacon_now >= BEACON_MS) {
    sendBeacon(beacon_idx);
    beacon_idx = (beacon_idx + 1) % MAX_FAKE_APS;
    beacon_now = millis();
  }
}
