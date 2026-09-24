#include "DianaSetup.h"
#include "DianaConfig.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>

DianaSetup Setup;

static WebServer* server = nullptr;
static DNSServer* dns = nullptr;

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DIANA setup</title><style>
body{background:#00080d;color:#00d4ff;font-family:system-ui,sans-serif;margin:0;padding:18px}
h1{font-weight:300;letter-spacing:.3em;margin:0 0 2px}small{color:#6c8}.id{color:#6a7;letter-spacing:.2em;font-size:12px}
form{max-width:420px}label{display:block;margin:14px 0 4px;color:#9ad}
input,select,textarea{width:100%;box-sizing:border-box;padding:10px;background:#001018;color:#fff;border:1px solid #005a70;border-radius:6px;font-size:16px;font-family:inherit}
button{margin-top:18px;width:100%;padding:12px;background:#00d4ff;color:#000;border:0;border-radius:6px;font-size:17px;font-weight:600}
.row{display:flex;gap:10px}.row>div{flex:1}.note{color:#7a8;font-size:12px;margin-top:6px}
.ok{padding:14px;border:1px solid #00d4ff;border-radius:8px;margin-top:20px}
</style></head><body><h1>DIANA</h1><div class="id">%ID% // CARDPUTER SETUP</div>)HTML";

static const char PAGE_FORM[] PROGMEM = R"HTML(
<form method="POST" action="/save">
<label>WiFi network</label><select name="ssid_sel" onchange="document.getElementById('ssid').value=this.value">%SCAN%</select>
<label>or type SSID</label><input id="ssid" name="ssid" value="%SSID%" placeholder="network name">
<label>WiFi password</label><input name="pass" type="password" placeholder="%PASS%">
<label>Gemini API key(s) <small>one per line; rotates on quota</small></label><textarea name="key" rows="3" placeholder="%KEY%"></textarea>
<label>Your name (optional)</label><input name="name" value="%NAME%">
<div class="row"><div><label>Voice replies</label><select name="voice"><option value="1" %V1%>on</option><option value="0" %V0%>off</option></select></div>
<div><label>Voice</label><select name="tvoice">%VOICES%</select></div></div>
<div class="row"><div><label>Chat model</label><input name="model" value="%MODEL%" placeholder="gemini-3.5-flash-lite"></div>
<div><label>Timezone (POSIX, optional)</label><input name="tz" value="%TZ%" placeholder="auto"></div></div>
<button type="submit">Save &amp; reboot</button>
<div class="note">Saved to /diana/config.json on the SD card and to internal flash.</div>
</form></body></html>)HTML";

static const char* VOICES[] = {"Leda", "Kore", "Aoede", "Zephyr", "Puck", "Charon", "Fenrir", "Orus", "Callirrhoe", "Autonoe", "Despina", "Erinome", "Laomedeia", "Sulafat", "Vindemiatrix", "Achernar"};

static String htmlEscape(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

void DianaSetup::handleRoot() {
    String page = FPSTR(PAGE_HEAD); page.replace("%ID%", DIANA_ID);
    String form = FPSTR(PAGE_FORM);
    String first = Config.wifi.empty() ? "" : Config.wifi[0].ssid;
    String firstPass = Config.wifi.empty() ? "" : Config.wifi[0].pass;
    form.replace("%SCAN%", _scanOptions);
    form.replace("%SSID%", htmlEscape(first));
    // Secrets are never sent back to the browser: blank = keep what is stored.
    form.replace("%PASS%", firstPass.length() ? "(unchanged - leave blank to keep)" : "");
    form.replace("%KEY%", Config.apiKeyCount() ? String(Config.apiKeyCount()) + " key(s) saved - paste here to add more" : "AIza...");
    form.replace("%NAME%", htmlEscape(Config.userName));
    form.replace("%MODEL%", htmlEscape(Config.chatModel));
    form.replace("%TZ%", htmlEscape(Config.tz));
    form.replace("%V1%", Config.voiceEnabled ? "selected" : "");
    form.replace("%V0%", Config.voiceEnabled ? "" : "selected");
    String voices;
    String curVoice = Config.ttsVoiceOrDefault();
    for (auto v : VOICES) {
        voices += "<option value=\"" + String(v) + "\"" + (curVoice == v ? " selected" : "") + ">" + v + "</option>";
    }
    form.replace("%VOICES%", voices);
    page += form;
    server->send(200, "text/html; charset=utf-8", page);
}

void DianaSetup::handleSave() {
    String ssid = server->arg("ssid");
    if (ssid.isEmpty()) ssid = server->arg("ssid_sel");
    ssid.trim();
    String pass = server->arg("pass");
    String key = server->arg("key");
    key.trim();
    if (ssid.length()) {
        if (pass.isEmpty() && !Config.wifi.empty() && Config.wifi[0].ssid == ssid) pass = Config.wifi[0].pass;   // blank = keep
        Config.setWifi(ssid, pass);
    }
    // key field may hold several keys separated by newlines/commas/spaces
    key.replace(",", "\n");
    key.replace(" ", "\n");
    key.replace("\r", "\n");
    int ks = 0;
    while (ks < (int)key.length()) {
        int nl = key.indexOf('\n', ks);
        String one = (nl < 0) ? key.substring(ks) : key.substring(ks, nl);
        one.trim();
        if (one.length()) Config.addApiKey(one);
        if (nl < 0) break;
        ks = nl + 1;
    }
    Config.syncActiveKey();
    Config.userName = server->arg("name");
    Config.voiceEnabled = server->arg("voice") != "0";
    Config.ttsVoice = server->arg("tvoice");
    Config.chatModel = server->arg("model");
    Config.tz = server->arg("tz");
    bool ok = Config.save(SD.cardSize() > 0);
    String page = FPSTR(PAGE_HEAD); page.replace("%ID%", DIANA_ID);
    page += ok ? "<div class=\"ok\">Saved. Diana is rebooting and will connect to <b>" + htmlEscape(ssid) + "</b>.<br><br>You can close this page.</div></body></html>"
               : "<div class=\"ok\" style=\"border-color:#f33;color:#f33\">Could not save the configuration.</div></body></html>";
    server->send(200, "text/html; charset=utf-8", page);
    delay(400);
    _saved = ok;
}

bool DianaSetup::start() {
    if (_running) return true;
    WiFi.mode(WIFI_AP_STA);
    // scan first so the page can offer a dropdown
    int n = WiFi.scanNetworks();
    _scanOptions = "<option value=\"\">-- choose --</option>";
    for (int i = 0; i < n && i < 20; ++i) {
        String s = WiFi.SSID(i);
        if (s.isEmpty()) continue;
        _scanOptions += "<option value=\"" + htmlEscape(s) + "\">" + htmlEscape(s) + " (" + WiFi.RSSI(i) + " dBm)</option>";
    }
    WiFi.scanDelete();
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(SETUP_AP_SSID, psk().c_str())) return false;
    delay(200);
    dns = new DNSServer();
    dns->start(53, "*", IPAddress(192, 168, 4, 1));
    server = new WebServer(80);
    server->on("/", HTTP_GET, [this]() { handleRoot(); });
    server->on("/save", HTTP_POST, [this]() { handleSave(); });
    server->onNotFound([this]() {
        // captive-portal style redirect
        server->sendHeader("Location", "http://192.168.4.1/", true);
        server->send(302, "text/plain", "");
    });
    server->begin();
    _running = true;
    _saved = false;
    Serial.println("[SETUP] portal started at http://192.168.4.1");
    return true;
}

void DianaSetup::stop() {
    if (!_running) return;
    if (server) { server->stop(); delete server; server = nullptr; }
    if (dns) { dns->stop(); delete dns; dns = nullptr; }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    _running = false;
}

bool DianaSetup::loop() {
    if (!_running) return false;
    if (dns) dns->processNextRequest();
    if (server) server->handleClient();
    return !_saved;
}

int DianaSetup::clients() { return WiFi.softAPgetStationNum(); }

// WPA2 key for the setup AP: stable per device (from the MAC) and shown on the screen, so the
// portal - which accepts new credentials - is not an open network anyone nearby can join.
String DianaSetup::psk() {
    uint8_t mac[6]; WiFi.macAddress(mac);
    char b[16]; snprintf(b, sizeof(b), "%s%02x%02x", SETUP_AP_PSK_PREFIX, mac[4], mac[5]);
    return String(b);
}
