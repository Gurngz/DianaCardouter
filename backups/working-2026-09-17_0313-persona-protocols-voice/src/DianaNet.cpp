#include "DianaNet.h"
#include "DianaConfig.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

DianaNet Net;

String WeatherInfo::summary() const {
    if (!ok) return "weather unavailable";
    String s = description + ", " + String(tempC, 1) + " C, wind " + String((int)windKmh) + " km/h";
    if (humidity >= 0) s += ", humidity " + String(humidity) + "%";
    return s;
}

static const char* wlStatusName(int s) {
    switch (s) {
        case WL_IDLE_STATUS:    return "idle";
        case WL_NO_SSID_AVAIL:  return "no-ssid (AP not found / wrong name / 5GHz)";
        case WL_SCAN_COMPLETED: return "scan-done";
        case WL_CONNECTED:      return "connected";
        case WL_CONNECT_FAILED: return "connect-failed (bad password?)";
        case WL_CONNECTION_LOST:return "connection-lost";
        case WL_DISCONNECTED:   return "disconnected";
        default:                return "unknown";
    }
}

bool DianaNet::connect(uint32_t timeoutPerNetworkMs, void (*progress)(const String&)) {
    if (!Config.hasWifi()) return false;
    if (timeoutPerNetworkMs < 15000) timeoutPerNetworkMs = 15000;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname("diana-cardputer");

    // One diagnostic scan so we can see which 2.4 GHz APs are actually visible.
    int n = WiFi.scanNetworks();
    Serial.printf("[NET] scan found %d networks:\n", n);
    for (int i = 0; i < n && i < 16; ++i) {
        Serial.printf("   '%s' rssi=%d ch=%d enc=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
    }
    WiFi.scanDelete();

    for (auto& cred : Config.wifi) {
        if (progress) progress("WiFi: " + cred.ssid);
        Serial.printf("[NET] connecting to '%s' (pass %d chars)...\n", cred.ssid.c_str(), (int)cred.pass.length());
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.begin(cred.ssid.c_str(), cred.pass.c_str());
        uint32_t t0 = millis();
        int last = -99;
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutPerNetworkMs) {
            int st = WiFi.status();
            if (st != last) { Serial.printf("[NET]   status=%d %s\n", st, wlStatusName(st)); last = st; }
            delay(150);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[NET] connected to %s ip=%s rssi=%d\n", cred.ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return true;
        }
        Serial.printf("[NET] '%s' failed: final status=%d %s\n", cred.ssid.c_str(), WiFi.status(), wlStatusName(WiFi.status()));
    }
    return false;
}

bool DianaNet::isConnected() { return WiFi.status() == WL_CONNECTED; }
int  DianaNet::rssi() { return isConnected() ? WiFi.RSSI() : 0; }
String DianaNet::ip() { return WiFi.localIP().toString(); }
String DianaNet::ssid() { return WiFi.SSID(); }

bool DianaNet::timeValid() { return time(nullptr) > 1600000000; }

bool DianaNet::syncTime(const String& posixTz) {
    if (!isConnected()) return false;
    if (posixTz.length()) {
        configTzTime(posixTz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    } else {
        if (!_located) locate();
        configTime(_tzOffset, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    }
    uint32_t t0 = millis();
    while (!timeValid() && millis() - t0 < 8000) delay(100);
    return timeValid();
}

String DianaNet::timeString(const char* fmt) {
    if (!timeValid()) return "";
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[48];
    strftime(buf, sizeof(buf), fmt, &t);
    return String(buf);
}

String DianaNet::dateTimeString() { return timeString("%A %Y-%m-%d %H:%M"); }

bool DianaNet::httpGet(const String& url, String& body, size_t maxLen, uint32_t timeoutMs) {
    body = "";
    HTTPClient http;
    http.setTimeout(timeoutMs);
    http.setReuse(false);
    http.setUserAgent("Diana-Cardputer/" DIANA_VERSION);
    bool ok = false;
    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(15);
        if (!http.begin(client, url)) return false;
        int code = http.GET();
        if (code == 200) {
            body = http.getString();
            ok = true;
        } else {
            Serial.printf("[NET] GET %s -> %d\n", url.c_str(), code);
        }
        http.end();
    } else {
        WiFiClient client;
        if (!http.begin(client, url)) return false;
        int code = http.GET();
        if (code == 200) {
            body = http.getString();
            ok = true;
        } else {
            Serial.printf("[NET] GET %s -> %d\n", url.c_str(), code);
        }
        http.end();
    }
    if (body.length() > maxLen) body = body.substring(0, maxLen);
    return ok;
}

bool DianaNet::locate() {
    if (!isConnected()) return false;
    String body;
    if (!httpGet("http://ip-api.com/json/?fields=status,lat,lon,city,offset", body, 1024, 6000)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (String(doc["status"] | "") != "success") return false;
    _lat = doc["lat"] | 0.0f;
    _lon = doc["lon"] | 0.0f;
    _city = doc["city"] | "your area";
    _tzOffset = doc["offset"] | 0L;
    _located = true;
    Serial.printf("[NET] located: %s (%.3f, %.3f) tz offset %ld\n", _city.c_str(), _lat, _lon, _tzOffset);
    return true;
}

const char* DianaNet::wmoDescription(int code) {
    switch (code) {
        case 0: return "clear skies";      case 1: return "mainly clear";      case 2: return "partly cloudy";
        case 3: return "overcast";         case 45: return "foggy";            case 48: return "freezing fog";
        case 51: return "light drizzle";   case 53: return "moderate drizzle"; case 55: return "heavy drizzle";
        case 56: case 57: return "freezing drizzle";
        case 61: return "light rain";      case 63: return "moderate rain";    case 65: return "heavy rain";
        case 66: case 67: return "freezing rain";
        case 71: return "light snow";      case 73: return "moderate snow";    case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: return "rain showers";    case 81: return "heavy showers";    case 82: return "violent showers";
        case 85: return "snow showers";    case 86: return "heavy snow showers";
        case 95: return "thunderstorms";   case 96: case 99: return "thunderstorms with hail";
    }
    return "mixed conditions";
}

bool DianaNet::fetchWeather(float lat, float lon, const String& cityName, WeatherInfo& out) {
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4) +
                 "&current=temperature_2m,weathercode,wind_speed_10m,relative_humidity_2m&temperature_unit=celsius&wind_speed_unit=kmh&timezone=auto";
    String body;
    if (!httpGet(url, body, 3000, 9000)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject cur = doc["current"];
    if (cur.isNull()) return false;
    out.ok = true;
    out.city = cityName;
    out.tempC = cur["temperature_2m"] | 0.0f;
    out.windKmh = cur["wind_speed_10m"] | 0.0f;
    out.humidity = cur["relative_humidity_2m"] | -1;
    out.description = wmoDescription(cur["weathercode"] | 0);
    return true;
}

bool DianaNet::weatherHere(WeatherInfo& out) {
    if (!_located && !locate()) return false;
    return fetchWeather(_lat, _lon, _city, out);
}

bool DianaNet::weatherFor(const String& city, WeatherInfo& out) {
    String q = city;
    q.trim();
    if (q.isEmpty() || q.equalsIgnoreCase("here") || q.equalsIgnoreCase("current location")) return weatherHere(out);
    String enc;
    for (size_t i = 0; i < q.length(); ++i) {
        char c = q[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') enc += c;
        else if (c == ' ') enc += "%20";
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); enc += b; }
    }
    String body;
    if (!httpGet("https://geocoding-api.open-meteo.com/v1/search?name=" + enc + "&count=1&language=en&format=json", body, 3000, 9000)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject r = doc["results"][0];
    if (r.isNull()) return false;
    String name = String(r["name"] | city.c_str());
    const char* country = r["country"] | "";
    if (*country) name += ", " + String(country);
    return fetchWeather(r["latitude"] | 0.0f, r["longitude"] | 0.0f, name, out);
}
