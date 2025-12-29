#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <esp_mac.h>

// --- Globals ---
#define RGB_PIN 48
#define NUM_LEDS 1
Adafruit_NeoPixel rgb(NUM_LEDS, RGB_PIN, NEO_GRB + NEO_KHZ800);
AsyncWebServer server(80);
Preferences prefs;

NimBLEHIDDevice* pHid;
NimBLEServer* pBleServer;
NimBLEScan* pBleScan;
NimBLECharacteristic* pInput; 
NimBLECharacteristic* pBatteryLevel;

bool scanRequested = false, scanComplete = false;
String lastScanJson = "[]";

// Execution Globals
bool scriptRunning = false;
String currentScript = "";
int scriptLineIndex = 0;
unsigned long nextActionTime = 0;

// TIMING
int defaultDelay = 15;
int keyHoldTime = 20;

// Live Type Globals
bool liveTypeRunning = false;
String liveTypeBuffer = "";
int liveTypeIndex = 0;

// --- KEYMAP & HID LOGIC (Standard US) ---
const uint8_t keymap[128][2] = {
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}, 
    {0,0x2A},{0,0x2B},{0,0x28},{0,0},{0,0},{0,0},{0,0},{0,0}, 
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}, 
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0}, 
    {0,0x2C},{2,0x1E},{2,0x34},{2,0x20},{2,0x21},{2,0x22},{2,0x24},{0,0x34},
    {2,0x26},{2,0x27},{2,0x25},{2,0x2E},{0,0x36},{0,0x2D},{0,0x37},{0,0x38},
    {0,0x27},{0,0x1E},{0,0x1F},{0,0x20},{0,0x21},{0,0x22},{0,0x23},{0,0x24},{0,0x25},{0,0x26},
    {2,0x33},{0,0x33},{2,0x36},{0,0x2E},{2,0x37},{2,0x38},{2,0x1F},
    {2,0x04},{2,0x05},{2,0x06},{2,0x07},{2,0x08},{2,0x09},{2,0x0A},{2,0x0B},{2,0x0C},{2,0x0D},{2,0x0E},{2,0x0F},{2,0x10},{2,0x11},{2,0x12},{2,0x13},{2,0x14},{2,0x15},{2,0x16},{2,0x17},{2,0x18},{2,0x19},{2,0x1A},{2,0x1B},{2,0x1C},{2,0x1D},
    {0,0x2F},{0,0x31},{0,0x30},{2,0x23},{2,0x2D},{0,0x35},
    {0,0x04},{0,0x05},{0,0x06},{0,0x07},{0,0x08},{0,0x09},{0,0x0A},{0,0x0B},{0,0x0C},{0,0x0D},{0,0x0E},{0,0x0F},{0,0x10},{0,0x11},{0,0x12},{0,0x13},{0,0x14},{0,0x15},{0,0x16},{0,0x17},{0,0x18},{0,0x19},{0,0x1A},{0,0x1B},{0,0x1C},{0,0x1D},
    {2,0x2F},{2,0x31},{2,0x30},{2,0x35},{0,0x4C}
};
#define KEY_ENTER 0x28
#define KEY_ESC 0x29
#define KEY_BACKSPACE 0x2A
#define KEY_TAB 0x2B
#define KEY_SPACE 0x2C
#define MOD_CTRL 0x01
#define MOD_SHIFT 0x02
#define MOD_ALT 0x04
#define MOD_GUI 0x08

const uint8_t _hidReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 
    0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

void setRGB(uint8_t r, uint8_t g, uint8_t b) { rgb.setPixelColor(0, rgb.Color(r, g, b)); rgb.show(); }

void sendKeyRaw(uint8_t code, uint8_t mod) {
    uint8_t report[] = {mod, 0, code, 0, 0, 0, 0, 0};
    pInput->setValue(report, sizeof(report)); pInput->notify(); delay(keyHoldTime);
    uint8_t release[] = {0, 0, 0, 0, 0, 0, 0, 0};
    pInput->setValue(release, sizeof(release)); pInput->notify(); delay(keyHoldTime);
}

void sendChar(char c) {
    if (c < 128) {
        uint8_t mod = keymap[c][0];
        uint8_t code = keymap[c][1];
        if (code != 0) sendKeyRaw(code, mod);
    }
}

void typeText(String text) {
    for (int i = 0; i < text.length(); i++) sendChar(text[i]);
}

void processDuckyLine(String line) {
    line.trim();
    if (line.length() == 0 || line.startsWith("REM")) return;
    int spaceIdx = line.indexOf(' ');
    String cmd = (spaceIdx == -1) ? line : line.substring(0, spaceIdx);
    String args = (spaceIdx == -1) ? "" : line.substring(spaceIdx + 1);
    cmd.toUpperCase();

    if (cmd == "STRING") typeText(args);
    else if (cmd == "DELAY") delay(args.toInt());
    else if (cmd == "DEFAULTDELAY" || cmd == "DEFAULT_DELAY") defaultDelay = args.toInt();
    else if (cmd == "ENTER") sendKeyRaw(KEY_ENTER, 0);
    else if (cmd == "GUI" || cmd == "WINDOWS") sendKeyRaw(args.length()>0 ? keymap[args[0]][1] : 0, MOD_GUI);
    else if (cmd == "CTRL" || cmd == "CONTROL") sendKeyRaw(args.length()>0 ? keymap[args[0]][1] : 0, MOD_CTRL);
    else if (cmd == "ALT") sendKeyRaw(0, MOD_ALT);
    else if (cmd == "SHIFT") sendKeyRaw(0, MOD_SHIFT);
    else if (cmd == "TAB") sendKeyRaw(KEY_TAB, 0);
    else if (cmd == "ESC" || cmd == "ESCAPE") sendKeyRaw(KEY_ESC, 0);
    else if (cmd == "BACKSPACE" || cmd == "BKSP") sendKeyRaw(KEY_BACKSPACE, 0);
}

// --- History Saver (Safe Version) ---
void saveHistory(String macAddress) {
    // Note: No LittleFS.begin() here, it's done in setup()
    File file = LittleFS.open("/history.txt", FILE_APPEND);
    if(file) {
        file.printf("%s | %lu s\n", macAddress.c_str(), millis()/1000);
        file.close();
    } else {
        Serial.println("Failed to open history for append");
    }
}

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        setRGB(0, 255, 0); 
        pServer->updateConnParams(desc->conn_handle, 6, 6, 0, 100);
        uint8_t l=100; pBatteryLevel->setValue(&l, 1); pBatteryLevel->notify();
        
        // Save History
        String mac = NimBLEAddress(desc->peer_ota_addr).toString().c_str();
        saveHistory(mac);
    }
    void onDisconnect(NimBLEServer* pServer) override {
        setRGB(255, 0, 0); pServer->getAdvertising()->start();
    }
};

void startAdvertising() {
    NimBLEAdvertising* pAdvertising = pBleServer->getAdvertising();
    pAdvertising->stop(); 
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setAppearance(0x03C1); 
    advData.setCompleteServices(NimBLEUUID("1812"));
    pAdvertising->setAdvertisementData(advData);
    NimBLEAdvertisementData scanData;
    scanData.setName(prefs.getString("name", "ESP32_Pro").c_str());
    pAdvertising->setScanResponseData(scanData);
    pAdvertising->start();
}

void setup() {
    Serial.begin(115200);
    rgb.begin(); rgb.setBrightness(50); setRGB(255, 255, 0); 
    
    // 1. Initialize FileSystem FIRST
    if(!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
        return;
    }
    
    // 2. Ensure History File Exists
    if(!LittleFS.exists("/history.txt")) {
        File f = LittleFS.open("/history.txt", FILE_WRITE);
        if(f) { f.print("--- Connection History ---\n"); f.close(); }
    }
    
    prefs.begin("config", false);
    String dName = prefs.getString("name", "ESP32_Pro");
    String customMac = prefs.getString("mac", "");
    String apSSID = prefs.getString("ap_ssid", "ESP32_Pentest");
    String apPass = prefs.getString("ap_pass", "password123");
    String staSSID = prefs.getString("sta_ssid", "");
    String staPass = prefs.getString("sta_pass", "");
    defaultDelay = prefs.getInt("def_delay", 15);
    keyHoldTime = prefs.getInt("key_hold", 20);

    if (customMac.length() == 17) {
        uint8_t ma[6]; 
        sscanf(customMac.c_str(), "%x:%x:%x:%x:%x:%x", &ma[0],&ma[1],&ma[2],&ma[3],&ma[4],&ma[5]);
        esp_base_mac_addr_set(ma);
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSSID.c_str(), apPass.c_str());
    if(staSSID.length() > 0) WiFi.begin(staSSID.c_str(), staPass.c_str());

    NimBLEDevice::init(dName.c_str());
    NimBLEDevice::setSecurityAuth(true, true, true); 
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); 
    
    pBleServer = NimBLEDevice::createServer();
    pBleServer->setCallbacks(new ServerCallbacks());
    
    pHid = new NimBLEHIDDevice(pBleServer);
    pHid->reportMap((uint8_t*)_hidReportDescriptor, sizeof(_hidReportDescriptor)); 
    pInput = pHid->inputReport(1); 
    pHid->manufacturer()->setValue("Logitech"); pHid->pnp(0x02, 0x046D, 0xC52B, 0x0111); pHid->hidInfo(0x00, 0x01);

    NimBLEService* pBat = pBleServer->createService("180F");
    pBatteryLevel = pBat->createCharacteristic("2A19", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    uint8_t l=100; pBatteryLevel->setValue(&l, 1); pBat->start();
    
    pHid->startServices();
    startAdvertising();

    pBleScan = NimBLEDevice::getScan();
    pBleScan->setActiveScan(true); pBleScan->setInterval(100); pBleScan->setWindow(99);

    setRGB(255, 0, 0); 

    // --- API ROUTES (MUST BE BEFORE serveStatic) ---
    
    server.on("/api/config", HTTP_GET, [dName, apSSID, apPass, staSSID, staPass](AsyncWebServerRequest *request){
        JsonDocument d;
        d["name"] = dName; d["type"] = prefs.getUShort("type", 0x03C1); d["mac"] = prefs.getString("mac", "");
        d["ap_ssid"] = apSSID; d["ap_pass"] = apPass; d["sta_ssid"] = staSSID; d["sta_pass"] = staPass;
        d["def_delay"] = defaultDelay; d["key_hold"] = keyHoldTime; d["ip"] = WiFi.localIP().toString();
        String r; serializeJson(d, r); request->send(200, "application/json", r);
    });

    server.on("/api/save", HTTP_POST, [](AsyncWebServerRequest *r){
        if(r->hasParam("name",true)) prefs.putString("name", r->getParam("name",true)->value());
        if(r->hasParam("type",true)) prefs.putUShort("type", r->getParam("type",true)->value().toInt());
        if(r->hasParam("mac",true)) prefs.putString("mac", r->getParam("mac",true)->value());
        if(r->hasParam("ap_ssid",true)) prefs.putString("ap_ssid", r->getParam("ap_ssid",true)->value());
        if(r->hasParam("ap_pass",true)) prefs.putString("ap_pass", r->getParam("ap_pass",true)->value());
        if(r->hasParam("sta_ssid",true)) prefs.putString("sta_ssid", r->getParam("sta_ssid",true)->value());
        if(r->hasParam("sta_pass",true)) prefs.putString("sta_pass", r->getParam("sta_pass",true)->value());
        if(r->hasParam("def_delay",true)) prefs.putInt("def_delay", r->getParam("def_delay",true)->value().toInt());
        if(r->hasParam("key_hold",true)) prefs.putInt("key_hold", r->getParam("key_hold",true)->value().toInt());
        r->send(200, "text/plain", "Saved."); delay(500); ESP.restart();
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r){
        JsonDocument d; d["busy"] = (scriptRunning || liveTypeRunning || scanRequested);
        String s; serializeJson(d, s); r->send(200, "application/json", s);
    });

    server.on("/api/live/key", HTTP_POST, [](AsyncWebServerRequest *r){
        if(!r->hasParam("cmd", true)) { r->send(400); return; }
        String cmd = r->getParam("cmd", true)->value();
        if(cmd == "enter") sendKeyRaw(KEY_ENTER, 0);
        else if(cmd == "esc") sendKeyRaw(KEY_ESC, 0);
        else if(cmd == "backspace") sendKeyRaw(KEY_BACKSPACE, 0);
        else if(cmd == "tab") sendKeyRaw(KEY_TAB, 0);
        else if(cmd == "ctrl_a") sendKeyRaw(keymap['a'][1], MOD_CTRL);
        else if(cmd == "ctrl_c") sendKeyRaw(keymap['c'][1], MOD_CTRL);
        else if(cmd == "ctrl_v") sendKeyRaw(keymap['v'][1], MOD_CTRL);
        else if(cmd == "ctrl_z") sendKeyRaw(keymap['z'][1], MOD_CTRL);
        else if(cmd == "win_r") sendKeyRaw(keymap['r'][1], MOD_GUI);
        else if(cmd == "win_d") sendKeyRaw(keymap['d'][1], MOD_GUI);
        r->send(200, "text/plain", "OK");
    });

    server.on("/api/ducky/run", HTTP_POST, [](AsyncWebServerRequest *r){
        if(r->hasParam("script", true)) {
            currentScript = r->getParam("script", true)->value();
            scriptLineIndex=0; scriptRunning=true; r->send(200, "text/plain", "Running");
        } else r->send(400);
    });
    server.on("/api/ducky/stop", HTTP_POST, [](AsyncWebServerRequest *r){ scriptRunning=false; liveTypeRunning=false; r->send(200); });
    server.on("/api/live/type", HTTP_POST, [](AsyncWebServerRequest *r){
        if(r->hasParam("text", true)){ liveTypeBuffer=r->getParam("text", true)->value(); liveTypeIndex=0; liveTypeRunning=true; r->send(200); } else r->send(400);
    });

    // --- Files (index.html hidden) ---
    server.on("/api/files/list", HTTP_GET, [](AsyncWebServerRequest *r){
        File root = LittleFS.open("/"); File f = root.openNextFile();
        JsonDocument d; JsonArray a = d.to<JsonArray>();
        while(f){ 
            String fname = String(f.name());
            if(fname != "index.html" && fname != "/index.html") {
                JsonObject o=a.add<JsonObject>(); o["name"]=fname; o["size"]=f.size();
            }
            f=root.openNextFile(); 
        }
        String s; serializeJson(d, s); r->send(200, "application/json", s);
    });
    server.on("/api/files/read", HTTP_GET, [](AsyncWebServerRequest *r){
        String p=r->getParam("path")->value(); if(!p.startsWith("/")) p="/"+p;
        if(LittleFS.exists(p)) r->send(LittleFS, p, "text/plain"); else r->send(404);
    });
    server.on("/api/files/write", HTTP_POST, [](AsyncWebServerRequest *r){
        String p=r->getParam("path",true)->value(); String c=r->getParam("content",true)->value();
        if(!p.startsWith("/")) p="/"+p;
        File f=LittleFS.open(p, FILE_WRITE); if(f){ f.print(c); f.close(); r->send(200); } else r->send(500);
    });
    server.on("/api/files/delete", HTTP_POST, [](AsyncWebServerRequest *r){
        String p=r->getParam("path",true)->value(); if(!p.startsWith("/")) p="/"+p; LittleFS.remove(p); r->send(200);
    });

    server.on("/api/scan/start", HTTP_POST, [](AsyncWebServerRequest *request){
        if (scanRequested) request->send(429, "text/plain", "Busy");
        else { scanRequested = true; scanComplete = false; request->send(200, "text/plain", "OK"); }
    });
    server.on("/api/scan/results", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!scanComplete && scanRequested) request->send(202, "application/json", "[]");
        else request->send(200, "application/json", lastScanJson);
    });

    // --- History Route (Fixed) ---
    server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request){
        // SAFETY: Only read if file exists
        if(LittleFS.exists("/history.txt")) request->send(LittleFS, "/history.txt", "text/plain");
        else request->send(200, "text/plain", "No History.");
    });
    server.on("/api/clear_history", HTTP_POST, [](AsyncWebServerRequest *request){ 
        LittleFS.remove("/history.txt"); 
        // Recreate empty file
        File f = LittleFS.open("/history.txt", FILE_WRITE); f.print("--- Cleared ---\n"); f.close();
        request->send(200, "text/plain", "Cleared"); 
    });

    // --- SERVE STATIC FILES (LAST) ---
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.begin();
}

void loop() {
    if (scriptRunning && millis() > nextActionTime) {
        setRGB(255, 0, 255); 
        int nextNewLine = currentScript.indexOf('\n', scriptLineIndex);
        if (nextNewLine == -1) nextNewLine = currentScript.length();
        if (scriptLineIndex >= currentScript.length()) { scriptRunning = false; setRGB(0, 255, 0); } 
        else {
            processDuckyLine(currentScript.substring(scriptLineIndex, nextNewLine));
            scriptLineIndex = nextNewLine + 1;
            nextActionTime = millis() + defaultDelay; 
        }
    }
    if (liveTypeRunning && millis() > nextActionTime) {
        setRGB(0, 255, 255); 
        if (liveTypeIndex >= liveTypeBuffer.length()) { liveTypeRunning = false; setRGB(0, 255, 0); } 
        else { sendChar(liveTypeBuffer[liveTypeIndex]); liveTypeIndex++; nextActionTime = millis() + keyHoldTime + 2; }
    }
    if (scanRequested) {
        setRGB(0, 0, 255); 
        if(pBleServer->getAdvertising()->isAdvertising()) pBleServer->getAdvertising()->stop();
        pBleScan->clearResults();
        NimBLEScanResults results = pBleScan->start(5, false);
        JsonDocument doc; JsonArray arr = doc.to<JsonArray>();
        for(int i = 0; i < results.getCount(); i++) {
            NimBLEAdvertisedDevice d = results.getDevice(i);
            JsonObject o = arr.add<JsonObject>();
            o["name"] = d.getName().c_str(); o["mac"] = d.getAddress().toString().c_str(); o["rssi"] = d.getRSSI();
        }
        String t; serializeJson(doc, t); lastScanJson = t;
        startAdvertising(); setRGB(255, 0, 0); scanRequested = false; scanComplete = true;
    }
    delay(1);
}