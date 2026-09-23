// WiFi, clock sync, geolocation and weather (same free services as the PC build:
// ip-api.com for location/timezone, open-meteo.com for weather).
#pragma once
#include <Arduino.h>

struct WeatherInfo {
    bool   ok = false;
    String city;
    String description;   // "clear skies"
    float  tempC = 0;
    float  windKmh = 0;
    int    humidity = -1;
    String summary() const;   // "clear skies, 21.3 C, wind 5 km/h"
};

class DianaNet {
public:
    bool connect(uint32_t timeoutPerNetworkMs = 9000, void (*progress)(const String&) = nullptr);
    bool isConnected();
    int  rssi();
    String ip();
    String ssid();

    bool syncTime(const String& posixTz);          // NTP; uses ip-api offset when tz is empty
    bool timeValid();
    String timeString(const char* fmt = "%H:%M");
    String dateTimeString();                       // "Tuesday 2026-09-15 18:44"

    bool locate();                                  // ip-api -> _lat/_lon/_city/_tzOffset
    bool weatherHere(WeatherInfo& out);             // uses located coordinates
    bool weatherFor(const String& city, WeatherInfo& out);   // open-meteo geocoding + forecast

    String city() const { return _city; }
    bool located() const { return _located; }

    // generic tiny HTTP(S) GET helper -> body (max bytes), used by tools
    static bool httpGet(const String& url, String& body, size_t maxLen = 6000, uint32_t timeoutMs = 8000);

private:
    bool   _located = false;
    float  _lat = 0, _lon = 0;
    String _city;
    long   _tzOffset = 0;
    static bool fetchWeather(float lat, float lon, const String& cityName, WeatherInfo& out);
    static const char* wmoDescription(int code);
};

extern DianaNet Net;
