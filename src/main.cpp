#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// --- Config ---
#define SCALE_NAME      "FitTrack"
#define AP_SSID         "OpenTrackFit"
#define AP_PASSWORD     "12345678"
#define AP_TIMEOUT_MS   300000
#define WIFI_LOST_MS    60000

// --- BLE ---
static BLEUUID SVC_FFB0("0000ffb0-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFB2("0000ffb2-0000-1000-8000-00805f9b34fb");
static BLEUUID CHR_FFB3("0000ffb3-0000-1000-8000-00805f9b34fb");

#define PKT_LIVE   0xCE
#define PKT_STABLE 0xCA

// --- State ---
enum Mode { MODE_AP, MODE_STA, MODE_BLE_ONLY };

BLEScan* pBLEScan = nullptr;
BLEAdvertisedDevice* pScaleDevice = nullptr;
BLEClient* pClient = nullptr;
volatile bool connected = false;
bool doConnect = false;
bool scanning = false;

WebServer server(80);
Preferences prefs;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Mode currentMode = MODE_BLE_ONLY;
unsigned long apStartTime = 0;
unsigned long wifiLostTime = 0;

volatile float finalWeight = 0;
char finalWeightTime[20] = "";
volatile bool doForward = false;

// --- Preferences helpers ---

String getPref(const char* ns, const char* key) {
    prefs.begin(ns, true);
    String v = prefs.getString(key, "");
    prefs.end();
    return v;
}

void setPref(const char* ns, const char* key, const String& val) {
    prefs.begin(ns, false);
    prefs.putString(key, val);
    prefs.end();
}

uint16_t getPrefInt(const char* ns, const char* key, uint16_t def) {
    prefs.begin(ns, true);
    uint16_t v = prefs.getUShort(key, def);
    prefs.end();
    return v;
}

void setPrefInt(const char* ns, const char* key, uint16_t val) {
    prefs.begin(ns, false);
    prefs.putUShort(key, val);
    prefs.end();
}

// --- MQTT & HTTP forwarding ---

void forwardWeight(float weight, const char* time) {
    String json = "{\"weight\":" + String(weight, 1) + ",\"time\":\"" + String(time) + "\"}";

    // MQTT
    String broker = getPref("mqtt", "broker");
    if (!broker.isEmpty()) {
        String topic = getPref("mqtt", "topic");
        if (topic.isEmpty()) topic = "opentrackfit/weight";
        uint16_t port = getPrefInt("mqtt", "port", 1883);
        String user = getPref("mqtt", "user");
        String pass = getPref("mqtt", "pass");

        mqttClient.setServer(broker.c_str(), port);
        bool ok;
        if (!user.isEmpty()) {
            ok = mqttClient.connect("OpenTrackFit", user.c_str(), pass.c_str());
        } else {
            ok = mqttClient.connect("OpenTrackFit");
        }
        if (ok) {
            mqttClient.publish(topic.c_str(), json.c_str());
            Serial.printf("MQTT published to %s\n", topic.c_str());
            mqttClient.disconnect();
        } else {
            Serial.printf("MQTT connect failed (rc=%d)\n", mqttClient.state());
        }
    }

    // HTTP Webhook
    String webhook = getPref("http", "webhook");
    if (!webhook.isEmpty() && WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(webhook);
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(json);
        Serial.printf("HTTP POST %s -> %d\n", webhook.c_str(), code);
        http.end();
    }
}

// --- HTML ---

const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenTrackFit Setup</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:30px 20px;background:#111;color:#eee}
  h1{color:#4CAF50;margin-bottom:5px;font-size:clamp(22px,3.5vw,32px)}
  .home{color:#888;font-size:clamp(13px,1.5vw,15px);text-decoration:none;display:block;margin-bottom:20px}
  .card{background:#1a1a1a;border-radius:10px;padding:clamp(16px,3vw,28px);margin-bottom:18px}
  .card h2{color:#ccc;font-size:clamp(14px,2vw,17px);margin:0 0 14px 0;padding-bottom:8px;border-bottom:1px solid #333}
  input,select{width:100%;padding:clamp(10px,1.5vw,14px);margin:6px 0;border-radius:6px;border:1px solid #444;background:#222;color:#eee;font-size:clamp(14px,1.5vw,16px)}
  label{display:block;margin-top:10px;color:#999;font-size:clamp(12px,1.5vw,14px)}
  button{width:100%;padding:clamp(11px,1.5vw,15px);margin-top:14px;background:#4CAF50;color:#fff;border:none;border-radius:6px;font-size:clamp(15px,1.5vw,18px);cursor:pointer}
  button:hover{background:#45a049}
  .info{color:#888;font-size:clamp(11px,1.2vw,14px);margin-top:6px}
  .msg{padding:10px;border-radius:6px;margin-top:10px;font-size:clamp(13px,1.5vw,15px);display:none}
  .msg.ok{display:block;background:#1b3a1b;color:#4CAF50}
  .msg.err{display:block;background:#3a1b1b;color:#f44336}
</style>
</head><body>
<h1>OpenTrackFit</h1>
<a class="home" href="/">Zurueck zur Startseite</a>

<div class="card">
  <h2>WLAN</h2>
  <form action="/save/wifi" method="POST">
    <label>SSID</label>
    <select name="ssid" id="ssid" style="display:none"></select>
    <input name="ssid_manual" id="ssid_manual" placeholder="WLAN Name">
    <div class="info" id="scan-status">Scanne WLANs...</div>
    <label>Passwort</label>
    <input name="pass" type="password" placeholder="WLAN Passwort">
    <button type="submit">WLAN speichern & verbinden</button>
  </form>
</div>

<div class="card">
  <h2>MQTT</h2>
  <form action="/save/mqtt" method="POST">
    <label>Broker</label>
    <input name="broker" id="mqtt_broker" placeholder="z.B. 192.168.178.50">
    <label>Port</label>
    <input name="port" id="mqtt_port" placeholder="1883" type="number">
    <label>Topic</label>
    <input name="topic" id="mqtt_topic" placeholder="opentrackfit/weight">
    <label>Benutzer (optional)</label>
    <input name="user" id="mqtt_user" placeholder="">
    <label>Passwort (optional)</label>
    <input name="pass" id="mqtt_pass" type="password" placeholder="">
    <button type="submit">MQTT speichern</button>
    <div class="msg" id="mqtt-msg"></div>
  </form>
</div>

<div class="card">
  <h2>HTTP Webhook</h2>
  <form action="/save/http" method="POST">
    <label>POST URL</label>
    <input name="webhook" id="http_webhook" placeholder="https://example.com/webhook">
    <button type="submit">Webhook speichern</button>
    <div class="msg" id="http-msg"></div>
  </form>
</div>

<div class="card">
  <h2>REST API</h2>
  <p style="color:#999;font-size:14px;margin:0">Messdaten per API abrufen:</p>
  <a href="/api/last-weight-data" target="_blank" style="display:block;margin-top:10px;color:#4CAF50;font-size:15px;word-break:break-all">/api/last-weight-data</a>
  <p style="color:#666;font-size:12px;margin:6px 0 0">Dokumentation: <a href="/api/docs" target="_blank" style="color:#888">/api/docs</a></p>
</div>

<p class="info" style="text-align:center;margin-top:20px">AP schaltet sich nach 5 Min. automatisch ab.</p>
<script>
fetch('/api/scan').then(r=>r.json()).then(d=>{
  var sel=document.getElementById('ssid');
  var inp=document.getElementById('ssid_manual');
  var st=document.getElementById('scan-status');
  if(d.length>0){
    sel.style.display='';inp.style.display='none';inp.name='';sel.name='ssid';
    d.forEach(function(n){
      var o=document.createElement('option');
      o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';
      sel.appendChild(o);
    });
    var o=document.createElement('option');
    o.value='__manual__';o.textContent='Manuell eingeben...';
    sel.appendChild(o);
    sel.onchange=function(){
      if(sel.value==='__manual__'){
        inp.style.display='';inp.name='ssid';inp.required=true;sel.name='';
      }else{
        inp.style.display='none';inp.name='';inp.required=false;sel.name='ssid';
      }
    };
    st.textContent=d.length+' Netzwerke gefunden';
  }else{
    st.textContent='Keine Netzwerke gefunden';inp.required=true;
  }
}).catch(()=>{
  document.getElementById('scan-status').textContent='Scan fehlgeschlagen';
  document.getElementById('ssid_manual').required=true;
});
fetch('/api/settings').then(r=>r.json()).then(d=>{
  if(d.mqtt_broker) document.getElementById('mqtt_broker').value=d.mqtt_broker;
  if(d.mqtt_port) document.getElementById('mqtt_port').value=d.mqtt_port;
  if(d.mqtt_topic) document.getElementById('mqtt_topic').value=d.mqtt_topic;
  if(d.mqtt_user) document.getElementById('mqtt_user').value=d.mqtt_user;
  if(d.http_webhook) document.getElementById('http_webhook').value=d.http_webhook;
}).catch(()=>{});
document.querySelectorAll('form[action="/save/mqtt"],form[action="/save/http"]').forEach(function(f){
  f.onsubmit=function(e){
    e.preventDefault();
    var fd=new FormData(f);
    var id=f.action.includes('mqtt')?'mqtt-msg':'http-msg';
    fetch(f.action,{method:'POST',body:new URLSearchParams(fd)})
      .then(r=>r.json()).then(d=>{
        var m=document.getElementById(id);
        m.textContent=d.message;
        m.className='msg '+(d.ok?'ok':'err');
      }).catch(()=>{});
  };
});
</script>
</body></html>
)rawliteral";

const char WEIGHT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenTrackFit</title>
<style>
  body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:60px 20px;background:#111;color:#eee;text-align:center}
  h1{color:#4CAF50;margin-bottom:0;font-size:clamp(24px,4vw,36px)}
  .weight{font-size:clamp(64px,12vw,120px);font-weight:bold;margin:50px 0 10px;color:#4CAF50}
  .unit{font-size:clamp(24px,4vw,36px);color:#888}
  .status{font-size:clamp(14px,2vw,18px);color:#888;margin-top:20px}
  .settings{margin-top:50px}
  .settings a{color:#888;font-size:clamp(13px,1.5vw,16px)}
</style>
</head><body>
<h1>OpenTrackFit</h1>
<div class="weight"><span id="weight">--.-</span> <span class="unit">kg</span></div>
<div class="status" id="status">Noch keine Messung</div>
<div class="status" id="time"></div>
<div class="settings"><a href="/setup">Netzwerkeinstellungen</a></div>
<script>
function load(){
  fetch('/api/last-weight-data').then(r=>r.json()).then(d=>{
    if(d.weight>0){
      document.getElementById('weight').textContent=d.weight.toFixed(1);
      document.getElementById('status').textContent='Letzte Messung';
      if(d.time) document.getElementById('time').textContent=d.time;
    }
  }).catch(()=>{});
}
load();
setInterval(load,5000);
</script>
</body></html>
)rawliteral";

// --- WiFi ---

bool connectWiFi() {
    String ssid = getPref("wifi", "ssid");
    String pass = getPref("wifi", "pass");
    if (ssid.isEmpty()) return false;

    Serial.printf("Connecting to WiFi: %s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");
        return true;
    }
    return false;
}

void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    currentMode = MODE_AP;
    apStartTime = millis();
}

void stopAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    currentMode = MODE_BLE_ONLY;
}

// --- Web Handlers ---

void handleRoot() {
    server.send(200, "text/html", currentMode == MODE_AP ? CONFIG_PAGE : WEIGHT_PAGE);
}

void handleSetup() {
    server.send(200, "text/html", CONFIG_PAGE);
}

void handleSaveWifi() {
    String ssid = server.arg("ssid");
    if (ssid == "__manual__" || ssid.isEmpty()) ssid = server.arg("ssid_manual");
    String pass = server.arg("pass");

    if (ssid.isEmpty()) {
        server.send(200, "text/html",
            "<html><head><meta http-equiv='refresh' content='3;url=/setup'></head>"
            "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#f44336'>SSID fehlt</h2>"
            "<p style='color:#888'>Weiterleitung in 3s...</p></body></html>");
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Testing WiFi: %s\n", ssid.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        setPref("wifi", "ssid", ssid);
        setPref("wifi", "pass", pass);
        Serial.printf("WiFi OK. IP: %s\n", ip.c_str());
        server.send(200, "text/html",
            "<html><head><meta http-equiv='refresh' content='5;url=http://" + ip + "/'></head>"
            "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#4CAF50'>Verbunden!</h2>"
            "<p>WLAN: " + ssid + "</p>"
            "<p>IP: <strong>" + ip + "</strong></p>"
            "<p>mDNS: <strong>http://opentrackfit.local</strong></p>"
            "<p style='color:#888;margin-top:20px'>Neustart in 5s...</p></body></html>");
        delay(5000);
        ESP.restart();
    } else {
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        Serial.println("WiFi test failed.");
        server.send(200, "text/html",
            "<html><head><meta http-equiv='refresh' content='5;url=/setup'></head>"
            "<body style='font-family:sans-serif;text-align:center;background:#111;color:#eee;padding:40px'>"
            "<h2 style='color:#f44336'>WLAN Verbindung fehlgeschlagen</h2>"
            "<p>WLAN: " + ssid + "</p>"
            "<p>Bitte SSID und Passwort pruefen.</p>"
            "<p style='color:#888;margin-top:20px'>Weiterleitung in 5s...</p></body></html>");
    }
}

void handleSaveMqtt() {
    setPref("mqtt", "broker", server.arg("broker"));
    String port = server.arg("port");
    setPrefInt("mqtt", "port", port.isEmpty() ? 1883 : port.toInt());
    setPref("mqtt", "topic", server.arg("topic"));
    setPref("mqtt", "user", server.arg("user"));
    setPref("mqtt", "pass", server.arg("pass"));
    Serial.println("MQTT settings saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"MQTT Einstellungen gespeichert.\"}");
}

void handleSaveHttp() {
    setPref("http", "webhook", server.arg("webhook"));
    Serial.println("HTTP webhook saved.");
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Webhook gespeichert.\"}");
}

void handleApiScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

void handleApiWeight() {
    server.send(200, "application/json",
        "{\"weight\":" + String(finalWeight, 1) + ",\"time\":\"" + String(finalWeightTime) + "\"}");
}

void handleApiSettings() {
    String json = "{";
    json += "\"mqtt_broker\":\"" + getPref("mqtt", "broker") + "\"";
    json += ",\"mqtt_port\":" + String(getPrefInt("mqtt", "port", 1883));
    json += ",\"mqtt_topic\":\"" + getPref("mqtt", "topic") + "\"";
    json += ",\"mqtt_user\":\"" + getPref("mqtt", "user") + "\"";
    json += ",\"http_webhook\":\"" + getPref("http", "webhook") + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleApiDocs() {
    server.send(200, "application/json",
        "{"
        "\"name\":\"OpenTrackFit API\","
        "\"version\":\"1.0\","
        "\"endpoints\":{"
          "\"GET /api/last-weight-data\":{\"description\":\"Letztes Messergebnis\",\"example\":{\"weight\":102.7,\"time\":\"22.03.2026 15:34\"},\"fields\":{\"weight\":\"Gewicht in kg (float, 0.0 wenn noch keine Messung)\",\"time\":\"Zeitpunkt der Messung (dd.mm.yyyy HH:MM, leer wenn kein NTP)\"}},"
          "\"GET /api/docs\":{\"description\":\"Diese API-Dokumentation\"}"
        "}"
        "}");
}

void setupWebServer() {
    server.on("/", handleRoot);
    server.on("/setup", handleSetup);
    server.on("/save/wifi", HTTP_POST, handleSaveWifi);
    server.on("/save/mqtt", HTTP_POST, handleSaveMqtt);
    server.on("/save/http", HTTP_POST, handleSaveHttp);
    server.on("/api/scan", handleApiScan);
    server.on("/api/last-weight-data", handleApiWeight);
    server.on("/api/settings", handleApiSettings);
    server.on("/api/docs", handleApiDocs);
    server.begin();
}

// --- BLE ---

class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* client) override {}
    void onDisconnect(BLEClient* client) override {
        connected = false;
        Serial.println("\n>> Scale disconnected. Rescanning...");
    }
};

void parseScaleData(uint8_t* data, size_t length) {
    if (length < 8 || data[0] != 0xAC || data[1] != 0x02) return;

    uint8_t type = data[6];

    uint8_t sum = 0;
    for (int i = 2; i <= 6; i++) sum += data[i];
    if (sum != data[7]) return;

    if (type != PKT_LIVE && type != PKT_STABLE) return;

    uint16_t raw = (data[2] << 8) | data[3];
    float weight = raw / 10.0f;

    if (type == PKT_STABLE) {
        finalWeight = weight;
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            strftime(finalWeightTime, sizeof(finalWeightTime), "%d.%m.%Y %H:%M", &ti);
        } else {
            strcpy(finalWeightTime, "");
        }
        Serial.printf(">>> FINAL WEIGHT: %.1f kg <<<\n", weight);
        doForward = true;
    } else {
        Serial.printf("  Measuring: %.1f kg\n", weight);
    }
}

void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    parseScaleData(pData, length);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override {
        if (device.haveName() && device.getName() == SCALE_NAME) {
            Serial.println("FitTrack found!");
            if (pScaleDevice) delete pScaleDevice;
            pScaleDevice = new BLEAdvertisedDevice(device);
            doConnect = true;
            pBLEScan->stop();
        }
    }
};

bool connectToScale() {
    Serial.printf("Connecting to %s...\n", pScaleDevice->getAddress().toString().c_str());

    if (pClient) { delete pClient; pClient = nullptr; }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());

    if (!pClient->connect(pScaleDevice)) {
        Serial.println("Connection failed!");
        return false;
    }

    BLERemoteService* pService = pClient->getService(SVC_FFB0);
    if (!pService) {
        Serial.println("Service 0xFFB0 not found!");
        pClient->disconnect();
        return false;
    }

    BLERemoteCharacteristic* pFFB2 = pService->getCharacteristic(CHR_FFB2);
    if (pFFB2 && pFFB2->canNotify()) pFFB2->registerForNotify(notifyCallback);

    BLERemoteCharacteristic* pFFB3 = pService->getCharacteristic(CHR_FFB3);
    if (pFFB3 && pFFB3->canNotify()) pFFB3->registerForNotify(notifyCallback);

    Serial.println("Connected. Waiting for measurement...");
    connected = true;
    return true;
}

void setupBLE() {
    BLEDevice::init("OpenTrackFit");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

// --- Main ---

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== OpenTrackFit ===");

    setupBLE();

    if (connectWiFi()) {
        currentMode = MODE_STA;
        MDNS.begin("opentrackfit");
        setupWebServer();
        Serial.println("-----------------------------");
        Serial.println("Mode:  LAN (Station)");
        Serial.printf("IP:    %s\n", WiFi.localIP().toString().c_str());
        Serial.println("mDNS:  http://opentrackfit.local");
        Serial.println("-----------------------------");
    } else {
        startAP();
        setupWebServer();
        Serial.println("-----------------------------");
        Serial.println("Mode:  AP (Access Point)");
        Serial.printf("SSID:  %s\n", AP_SSID);
        Serial.printf("Pass:  %s\n", AP_PASSWORD);
        Serial.printf("IP:    %s\n", WiFi.softAPIP().toString().c_str());
        Serial.println("-----------------------------");
    }
}

void loop() {
    server.handleClient();

    // AP timeout
    if (currentMode == MODE_AP && (millis() - apStartTime > AP_TIMEOUT_MS)) {
        stopAP();
        if (connectWiFi()) {
            currentMode = MODE_STA;
            MDNS.begin("opentrackfit");
            Serial.printf("WiFi reconnected. IP: %s\n", WiFi.localIP().toString().c_str());
        }
    }

    // WiFi watchdog
    if (currentMode == MODE_STA) {
        if (WiFi.status() != WL_CONNECTED) {
            if (wifiLostTime == 0) {
                wifiLostTime = millis();
                Serial.println("WiFi lost. Waiting 60s...");
            } else if (millis() - wifiLostTime > WIFI_LOST_MS) {
                Serial.println("WiFi lost for 60s. AP fallback.");
                wifiLostTime = 0;
                startAP();
            }
        } else {
            wifiLostTime = 0;
        }
    }

    // Forward weight (deferred from BLE callback to main loop)
    if (doForward) {
        doForward = false;
        forwardWeight(finalWeight, finalWeightTime);
    }

    // BLE connect
    if (doConnect && !connected) {
        connectToScale();
        doConnect = false;
    }

    // BLE non-blocking scan
    if (!connected && !doConnect && !scanning) {
        pBLEScan->start(5, [](BLEScanResults) { scanning = false; }, false);
        scanning = true;
    }

    delay(10);
}
